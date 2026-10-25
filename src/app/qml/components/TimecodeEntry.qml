pragma ComponentBehavior: Bound
import QtQuick

// Typed timecode entry (0.2 UX, stream T; Premiere's "click the timecode and type"): shows `text` (the playhead or the
// source position). A click - or Enter, Space or a typed digit while it has keyboard focus - turns it into a field,
// prefilled and selected (or seeded with the typed key). Enter hands the text to `seek`, a function (text) -> bool
// (ProjectController.seekToText, SourceMonitor.seekToText): a timecode in the display format ("00:01:02:03",
// "1:02:03"), digits packed from the right ("110" = 00:00:01:10), Premiere's "2." = two seconds, frames, or an offset
// from the playhead with a leading + or - ("+15" frames on, "-1:00" a second back). Esc (or a click elsewhere) cancels;
// text `seek` cannot read keeps the field open, shakes it (a red edge only under reduce motion) and says what it takes.
// While typing, the field claims its keys (ShortcutOverride) so the keymap does not take digits, + or - meanwhile, and
// `preview` (optional, (text) -> string) shows where Enter would go. Focus goes back where it was afterwards.
// `pill`: the monitors' accent pill; otherwise plain accent text (the Timeline's corner) that becomes a field pill.
// 0.4: the panel around it hands it numpad keys (`openFromKeypad`): a numpad digit, + , - or . with the Timeline or a
// monitor focused opens the field seeded with that key, as Premiere's "type a timecode" (main-row digits stay commands).
Item {
    id: root

    /// The time shown while not editing.
    property string text: ""
    /// What the field starts with (the time in the units `seek` reads; `text` by default).
    property string editText: text
    /// What the time is, for screen readers and the tooltips ("Playhead", "Source position").
    property string label: qsTr("Playhead")
    property bool pill: true
    /// The idle pill's fill (`pill`) and the text's colour.
    property color fill: Theme.accentFill
    property color color: pill ? Theme.accentOnFill : Theme.accentStroke
    property int pixelSize: Theme.textMd
    /// (text) -> bool: moves the playhead (or the source); false = not a time it can read.
    property var seek: null
    /// (text) -> string: the time Enter would go to ("" when the text is not a time). Optional.
    property var preview: null
    /// The field is open.
    property bool editing: false
    /// The last Enter was not a time (typing clears it).
    property bool invalid: false
    readonly property real padding: pill ? Theme.space(2) : 0
    readonly property string previewText: {
        if (!root.editing || root.invalid || !root.preview || field.text.trim() === "")
            return ""
        const target = root.preview(field.text)
        return target !== field.text.trim() ? target : ""
    }

    /// Enter moved the playhead.
    signal committed()

    // Where focus was before the field opened (it goes back there).
    property Item returnFocus: null

    function startEditing(seed: string) {
        if (root.editing || !root.enabled)
            return
        root.returnFocus = root.Window.activeFocusItem
        root.invalid = false
        field.text = seed !== "" ? seed : root.editText
        root.editing = true
        field.forceActiveFocus(Qt.MouseFocusReason)
        if (seed !== "")
            field.cursorPosition = field.text.length
        else
            field.selectAll()
    }
    function finish() {
        if (!root.editing)
            return
        root.editing = false
        root.invalid = false
        const back = root.returnFocus
        root.returnFocus = null
        if (back && back !== field && back.visible)
            back.forceActiveFocus(Qt.OtherFocusReason)
    }
    function cancel() {
        root.finish()
    }
    /// Enter: seeks to the typed time; an unreadable one keeps the field open.
    function commit() {
        const typed = field.text.trim()
        if (typed === "") {
            root.finish()
            return
        }
        const moved = root.seek ? root.seek(typed) === true : false
        if (moved) {
            root.finish()
            root.committed()
            return
        }
        root.invalid = true
        if (!Theme.reduceMotion)
            shake.restart()
    }

    implicitWidth: Math.max(display.implicitWidth, root.editing ? field.implicitWidth + Theme.space(0.5) : 0)
                   + 2 * root.padding
    implicitHeight: Math.max(display.implicitHeight, Theme.controlHeightSmall)
    // Tab stops here when it is enabled (Qt skips disabled items); while editing, the field has the focus.
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: qsTr("%1 %2").arg(root.label).arg(root.text)
    Accessible.description: qsTr("Press Enter to type a time to go to")
    Accessible.onPressAction: root.startEditing("")

    // Keys while it has focus (not editing): Enter or Space opens the field; a digit, + or - opens it seeded with it.
    function opensWith(event: KeyEvent): bool {
        return event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space
               || /^[0-9+\-]$/.test(event.text)
    }
    /// 0.4: a numpad key from the panel around it (Premiere's "type a timecode"): opens the field seeded with it.
    /// True when it took the key (a numpad digit, +, -, . or , with no other modifier; never while editing).
    function openFromKeypad(event: KeyEvent): bool {
        if (!root.enabled || !root.visible || root.editing || !(event.modifiers & Qt.KeypadModifier))
            return false
        if ((event.modifiers & ~Qt.KeypadModifier) !== Qt.NoModifier || !/^[0-9+\-.,]$/.test(event.text))
            return false
        root.startEditing(event.text)
        return root.editing
    }
    Keys.onShortcutOverride: event => event.accepted = !root.editing && root.opensWith(event)
    Keys.onPressed: event => {
        if (root.editing || !root.opensWith(event))
            return
        const seeded = /^[0-9+\-]$/.test(event.text)
        root.startEditing(seeded ? event.text : "")
        event.accepted = true
    }

    transform: Translate {
        id: nudge
    }
    SequentialAnimation {
        id: shake
        NumberAnimation { target: nudge; property: "x"; to: -Theme.space(1); duration: 40; easing.type: Easing.OutCubic }
        NumberAnimation { target: nudge; property: "x"; to: Theme.space(1); duration: 70; easing.type: Easing.InOutCubic }
        NumberAnimation { target: nudge; property: "x"; to: -Theme.space(0.5); duration: 60; easing.type: Easing.InOutCubic }
        NumberAnimation { target: nudge; property: "x"; to: 0; duration: 50; easing.type: Easing.OutCubic }
    }

    // The plate: the accent pill (monitors), a hover pill (the Timeline corner), or the field while editing.
    Rectangle {
        id: plate
        anchors.fill: parent
        anchors.leftMargin: root.pill ? 0 : -Theme.space(1.5)
        anchors.rightMargin: root.pill ? 0 : -Theme.space(1.5)
        radius: height / 2
        visible: root.pill || root.editing || hover.hovered
        color: root.editing ? Theme.raised : root.pill ? root.fill : Theme.hover
        border.width: root.editing ? Theme.focusRingWidth : 0
        border.color: root.invalid ? Theme.danger : Theme.focus
    }

    Text {
        id: display
        anchors.centerIn: parent
        visible: !root.editing
        text: root.text
        color: root.color
        font.family: Theme.fontMono
        font.pixelSize: root.pixelSize
        font.weight: Font.Bold
    }

    TextInput {
        id: field
        objectName: root.objectName + "Field"
        anchors.centerIn: parent
        width: Math.max(display.implicitWidth, implicitWidth + Theme.space(0.5))
        visible: root.editing
        color: root.invalid ? Theme.danger : Theme.textPrimary
        selectionColor: Theme.accentFill
        selectedTextColor: Theme.accentOnFill
        selectByMouse: true
        font.family: Theme.fontMono
        font.pixelSize: root.pixelSize
        font.weight: Font.Bold
        horizontalAlignment: TextInput.AlignHCenter
        inputMethodHints: Qt.ImhPreferNumbers | Qt.ImhNoPredictiveText
        validator: RegularExpressionValidator {
            regularExpression: /[0-9:;.,+\- ]{0,24}/
        }
        Accessible.role: Accessible.EditableText
        Accessible.name: qsTr("Type a time for %1").arg(root.label)
        Accessible.description: qsTr("A timecode, frames, or + or - and an offset; Enter goes there, Escape cancels")

        onTextEdited: root.invalid = false
        onActiveFocusChanged: {
            if (!activeFocus && root.editing)
                root.cancel()
        }
        // Everything typed here is the field's: digits, separators, + and - are timeline keys otherwise.
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
                                                           || event.key === Qt.Key_Delete || event.key === Qt.Key_Left
                                                           || event.key === Qt.Key_Right || event.key === Qt.Key_Home
                                                           || event.key === Qt.Key_End || event.key === Qt.Key_Escape
                                                           || event.key === Qt.Key_Return || event.key === Qt.Key_Enter
        // Enter commits here (taken, so it does not reach the closed entry and open it again); Esc cancels.
        Keys.onReturnPressed: event => {
            event.accepted = true
            root.commit()
        }
        Keys.onEnterPressed: event => {
            event.accepted = true
            root.commit()
        }
        Keys.onEscapePressed: event => {
            event.accepted = true
            root.cancel()
        }
    }

    HoverHandler {
        id: hover
        enabled: root.enabled
        cursorShape: Qt.IBeamCursor
    }
    TapHandler {
        enabled: root.enabled && !root.editing
        onTapped: root.startEditing("")
    }

    // Idle: what a click does. Editing: where Enter goes, or why it did not.
    Loader {
        active: hover.hovered && !root.editing
        sourceComponent: Tooltip {
            parent: root
            visible: true
            text: qsTr("Click to type a time (+ or - for an offset)")
        }
    }
    Tooltip {
        objectName: root.objectName + "Hint"
        delay: 0
        visible: root.editing && (root.invalid || root.previewText !== "")
        text: root.invalid ? qsTr("Not a time. Try 00:00:10:00, 1000, +15 or -1:00")
                           : qsTr("Enter goes to %1").arg(root.previewText)
    }

    FocusRing {
        anchors.fill: plate
        targetRadius: plate.height / 2
        visible: root.activeFocus && !root.editing
    }
}
