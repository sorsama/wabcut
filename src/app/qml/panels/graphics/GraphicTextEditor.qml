pragma ComponentBehavior: Bound
import QtQuick

// Inline text editing on the program monitor (C.13, M5o): a TextEdit over a graphic's text layer - or, since 0.1.3, a
// title clip's text (MonitorPanel) - in its font at the monitor's scale. Point text: Enter commits, Shift+Enter breaks
// the line; paragraph text: Enter breaks the line, Ctrl+Enter commits. Esc cancels; leaving the editor commits. The
// editor shapes and moves the caret like the rest of Qt (by grapheme clusters: a Thai consonant with its marks is one
// step). The keys are its own while it has focus (Space would otherwise play).
// 0.7 (card #81): with some text selected, the project's text styles show as buttons under the box (and Ctrl+Alt+1..9 take
// the first nine): each styles just the selected characters (styleRangeRequested); the editor keeps its focus meanwhile.
Rectangle {
    id: root
    objectName: "graphicTextEditor"

    /// GraphicCanvas.selection.editor: { x, y, width, height (fractions), fontFamily, fontSize (fraction of the height),
    /// fontWeight, italic, color, align, text, language }.
    property var info: ({})
    property int layerId: 0
    property bool paragraph: false
    /// The overlay's size (the canvas the fractions are of).
    property real canvasWidth: 1
    property real canvasHeight: 1
    readonly property alias text: edit.text
    readonly property alias textEdit: edit
    /// The TextEdit's objectName and spoken name (a title's editor has its own).
    property alias editName: edit.objectName
    property string accessibleName: qsTr("Text of the graphic layer")
    readonly property real pad: Theme.space(1)

    signal committed(string text)
    signal cancelled()
    /// 0.7 (card #81): a text style was chosen for the selected characters [from, to) (UTF-16 units of the editor's text).
    signal styleRangeRequested(int from, int to, string name)

    /// The project's text styles ([{ name, ... }], GraphicTools.textStyleItems()).
    property var styleNames: []
    readonly property bool hasSelection: edit.selectionStart !== edit.selectionEnd

    property bool closing: false

    /// Shows the editor over the layer with its text, the caret at the end.
    function open(layer, editorInfo, isParagraph) {
        layerId = layer
        info = editorInfo
        paragraph = isParagraph
        closing = false
        edit.text = editorInfo.text ?? ""
        visible = true
        edit.forceActiveFocus()
        edit.cursorPosition = edit.length
    }
    function finish(commit) {
        if (!visible || closing)
            return
        closing = true
        visible = false
        if (commit)
            committed(edit.text)
        else
            cancelled()
    }

    visible: false
    x: (info.x ?? 0) * canvasWidth - pad
    y: (info.y ?? 0) * canvasHeight - pad
    width: Math.max((info.width ?? 0) * canvasWidth, Theme.space(20)) + 2 * pad
    height: Math.max((info.height ?? 0) * canvasHeight, edit.contentHeight) + 2 * pad
    radius: Theme.radiusBadge
    color: Theme.scrim
    border.width: Theme.focusRingWidth
    border.color: Theme.accentStroke

    // 0.7 (card #81): the text styles for the selected characters.
    Flow {
        id: styleBar
        objectName: "graphicTextStyleBar"
        visible: root.visible && root.hasSelection && root.styleNames.length > 0
        anchors.top: parent.bottom
        anchors.topMargin: Theme.space(1)
        anchors.left: parent.left
        width: Math.max(root.width, Theme.space(80))
        spacing: Theme.space(1)

        Text {
            height: Theme.controlHeightSmall
            verticalAlignment: Text.AlignVCenter
            text: qsTr("Style the selection:")
            color: Theme.scrimText
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Repeater {
            model: root.styleNames
            delegate: PillButton {
                required property var modelData
                required property int index
                objectName: "graphicTextStyleButton_" + index
                compact: true
                variant: "secondary"
                focusPolicy: Qt.NoFocus // the editor keeps its focus (and its selection) while a style is chosen
                text: modelData.name
                tooltip: index < 9 ? qsTr("Style only the selected text with %1 (Ctrl+Alt+%2)").arg(modelData.name).arg(index + 1)
                                   : qsTr("Style only the selected text with %1").arg(modelData.name)
                onClicked: root.styleRangeRequested(edit.selectionStart, edit.selectionEnd, modelData.name)
            }
        }
    }

    TextEdit {
        id: edit
        objectName: "graphicTextEdit"
        anchors.fill: parent
        anchors.margins: root.pad
        color: root.info.color ?? Theme.scrimText
        selectionColor: Theme.accentFill
        selectedTextColor: Theme.accentOnFill
        font.family: (root.info.fontFamily ?? "") !== "" ? root.info.fontFamily : Theme.fontSans
        font.pixelSize: Math.max(Theme.textSm, Math.round((root.info.fontSize ?? 0) * root.canvasHeight))
        font.weight: root.info.fontWeight ?? Font.Normal
        font.italic: root.info.italic ?? false
        wrapMode: root.paragraph ? TextEdit.Wrap : TextEdit.NoWrap
        horizontalAlignment: root.info.align === "center" ? TextEdit.AlignHCenter
                           : root.info.align === "right" ? TextEdit.AlignRight
                           : root.info.align === "justify" ? TextEdit.AlignJustify : TextEdit.AlignLeft
        selectByMouse: true
        persistentSelection: true
        Accessible.role: Accessible.EditableText
        Accessible.name: root.accessibleName
        Accessible.description: root.paragraph ? qsTr("Enter starts a new line, Ctrl+Enter keeps the text, Esc cancels")
                                               : qsTr("Enter keeps the text, Shift+Enter starts a new line, Esc cancels")

        // Every key is the editor's while it edits (the keymap would turn Space into Play).
        Keys.onShortcutOverride: event => event.accepted = true
        Keys.onPressed: event => {
            const digit = event.key - Qt.Key_1
            if ((event.modifiers & Qt.ControlModifier) && (event.modifiers & Qt.AltModifier) && digit >= 0 && digit < 9) {
                // Ctrl+Alt+1..9: the n-th text style for the selected characters.
                if (root.hasSelection && digit < root.styleNames.length)
                    root.styleRangeRequested(edit.selectionStart, edit.selectionEnd, root.styleNames[digit].name)
                event.accepted = true
            } else if (event.key === Qt.Key_Escape) {
                root.finish(false)
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                const commit = root.paragraph ? (event.modifiers & Qt.ControlModifier) !== 0
                                              : (event.modifiers & Qt.ShiftModifier) === 0
                if (commit) {
                    root.finish(true)
                    event.accepted = true
                }
            }
        }
        onActiveFocusChanged: {
            if (!activeFocus && !editSpelling.visible)
                root.finish(true)
        }

        // 0.2: misspelled words underlined; right-click one for suggestions (the editor stays open).
        SpellHighlighter {
            objectName: "graphicTextEditSpelling"
            document: edit.textDocument
            language: root.info.language ?? ""
            color: Theme.danger
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: eventPoint => editSpelling.openAt(eventPoint.position.x, eventPoint.position.y)
        }
        SpellingMenu {
            id: editSpelling
            target: edit
            language: root.info.language ?? ""
            onCorrected: text => {
                edit.text = text
                edit.forceActiveFocus()
            }
            onClosed: {
                if (root.visible)
                    edit.forceActiveFocus()
            }
        }
    }
}
