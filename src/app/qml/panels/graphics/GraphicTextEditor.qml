pragma ComponentBehavior: Bound
import QtQuick

// Inline text editing on the program monitor (C.13, M5o): a TextEdit over a graphic's text layer - or, since 0.1.3, a
// title clip's text (MonitorPanel) - in its font at the monitor's scale. Point text: Enter commits, Shift+Enter breaks
// the line; paragraph text: Enter breaks the line, Ctrl+Enter commits. Esc cancels; leaving the editor commits. The
// editor shapes and moves the caret like the rest of Qt (by grapheme clusters: a Thai consonant with its marks is one
// step). The keys are its own while it has focus (Space would otherwise play).
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
            if (event.key === Qt.Key_Escape) {
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
            if (!activeFocus)
                root.finish(true)
        }
    }
}
