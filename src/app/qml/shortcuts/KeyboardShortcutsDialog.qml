pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Keyboard Shortcuts (Edit > Keyboard Shortcuts: Ctrl+Alt+K, ⌥⌘K on macOS), Premiere's editor (M5f).
//   Key set      Premiere Pro (default), Wabcut, or the user's own sets (Save as…, Delete); "modified" when the keys
//                differ from the set. Import a Premiere .kys or an overrides .json, export the overrides, reset.
//   Keyboard     every key coloured by what it does with the modifiers shown (application / panel / both);
//                click a modifier key or a toggle to see other combinations, click a key to see its commands.
//   Commands     search (names, Premiere names, shortcut text) and a panel filter; + types a shortcut, × removes
//                one, the reset arrow restores a command's keys; drag a command onto a key to give it that key.
// A chord another command in the same panel already uses asks first (Reassign / Cancel); a chord an application
// command and a panel command share says which one wins where. Changes apply to the keys at once and are saved.
Popup {
    id: dialog
    objectName: "keyboardShortcutsDialog"

    property int modifiers: 0
    /// The clicked key (an entry of Shortcuts.keyboard), or null.
    property var selectedKey: null
    /// The command whose shortcut is being typed.
    property string captureFor: ""
    /// The command chosen in the list.
    property string currentCommand: ""
    /// A change waiting for confirmation: { commandId, name, chordText, check }.
    property var pending: null
    property string message: ""
    property string messageTone: "info"     // info | success | danger
    property var unmapped: []

    function openDialog() {
        Shortcuts.search = ""
        Shortcuts.scopeFilter = ""
        modifiers = 0
        selectedKey = null
        captureFor = ""
        currentCommand = ""
        pending = null
        message = ""
        unmapped = []
        open()
    }
    function say(text, tone) {
        message = text
        messageTone = tone
        unmapped = []
    }
    /// Asks before taking a chord from another command (or when a panel and the application share it); else assigns.
    function proposeAssign(commandId, chordText) {
        captureFor = ""
        const check = Shortcuts.checkAssign(commandId, chordText)
        if (!check.ok) {
            say(check.error, "danger")
            return
        }
        if (check.already) {
            say(qsTr("“%1” already has %2").arg(Shortcuts.commandName(commandId)).arg(check.chord), "info")
            return
        }
        if (check.conflicts.length > 0 || check.shadows.length > 0) {
            message = ""
            pending = { commandId: commandId, name: Shortcuts.commandName(commandId), chordText: check.chordText, check: check }
            return
        }
        assignNow(commandId, check.chordText)
    }
    function assignNow(commandId, chordText) {
        pending = null
        const result = Shortcuts.assign(commandId, chordText)
        if (!result.ok) {
            say(result.error, "danger")
            return
        }
        const chord = Shortcuts.displayChord(chordText)
        say(result.lost.length > 0
            ? qsTr("%1 now runs “%2” (taken from %3)").arg(chord).arg(Shortcuts.commandName(commandId)).arg(result.lost.map(n => "“" + n + "”").join(", "))
            : qsTr("%1 now runs “%2”").arg(chord).arg(Shortcuts.commandName(commandId)), "success")
    }
    function applyPending() {
        if (pending)
            assignNow(pending.commandId, pending.chordText)
    }
    /// The chord of `key` with the modifiers shown.
    function chordOfKey(key) {
        return key ? Shortcuts.chordText(key.key, modifiers | (key.keypad ? Qt.KeypadModifier : 0)) : ""
    }
    function assignToKey(commandId, key) {
        const chord = chordOfKey(key)
        if (chord === "" || commandId === "")
            return
        selectedKey = key
        proposeAssign(commandId, chord)
    }
    function applyPreset(id) {
        pending = null
        const error = Shortcuts.applyPreset(id)
        say(error !== "" ? error : qsTr("Key set: %1").arg(Shortcuts.presetName), error !== "" ? "danger" : "success")
    }
    function importFile(url) {
        pending = null
        const report = Shortcuts.importKeySet(url)
        if (!report.ok) {
            say(qsTr("Cannot import %1: %2").arg(report.fileName).arg(report.error), "danger")
            return
        }
        say(qsTr("Imported %1: %n command(s) changed", "", report.changed).arg(report.fileName)
            + (report.unmapped.length > 0 ? " · " + qsTr("%n Premiere command(s) have no Wabcut equivalent", "", report.unmapped.length) : ""),
            "success")
        unmapped = report.unmapped
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? Math.min(parent.width - Theme.gap * 2, Theme.space(330)) : Theme.space(330)
    height: parent ? Math.min(parent.height - Theme.gap * 2, Theme.space(240)) : Theme.space(220)
    modal: true
    focus: true
    // Esc cancels typing a shortcut first (the capture field takes it).
    closePolicy: captureFor === "" ? Popup.CloseOnEscape : Popup.NoAutoClose
    padding: Theme.pad + Theme.space(1)
    onClosed: captureFor = ""

    Overlay.modal: Rectangle {
        color: Theme.scrim
    }
    background: Rectangle {
        radius: Theme.radiusPanel
        color: Theme.panel
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
        NumberAnimation { property: "scale"; from: Theme.popScale; to: 1; duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durPress }
    }

    contentItem: ColumnLayout {
        spacing: Theme.space(3)
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Keyboard Shortcuts")

        // ---- Header: title, key set, import / export / reset, close
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)

            Rectangle {
                implicitWidth: Theme.controlHeightSmall
                implicitHeight: Theme.controlHeightSmall
                radius: Theme.radiusField
                color: Theme.accentWash
                Icon {
                    anchors.centerIn: parent
                    name: "keyboard"
                    size: Theme.iconSm
                    color: Theme.accentStroke
                }
            }
            Text {
                text: qsTr("Keyboard Shortcuts")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textTitle
                font.weight: Font.DemiBold
            }
            Item { Layout.fillWidth: true }
            PillButton {
                id: keySetButton
                objectName: "shortcutsKeySet"
                compact: true
                iconName: "chevron-down"
                text: Shortcuts.modified ? qsTr("%1 · modified").arg(Shortcuts.presetName) : Shortcuts.presetName
                tooltip: qsTr("Key set")
                onClicked: keySetMenu.popup(keySetButton, 0, keySetButton.height + Theme.space(1))

                OptionsMenu {
                    id: keySetMenu
                    objectName: "shortcutsKeySetMenu"
                    options: Shortcuts.presets
                    current: Shortcuts.preset
                    onChosen: id => dialog.applyPreset(id)

                    ContextMenuSeparator {}
                    ContextMenuItem {
                        text: qsTr("Save as a new key set…")
                        iconName: "save"
                        onTriggered: saveSet.open()
                    }
                    ContextMenuItem {
                        text: qsTr("Delete this key set")
                        iconName: "trash-2"
                        enabled: Shortcuts.preset.startsWith("user:")
                        onTriggered: {
                            const error = Shortcuts.deletePreset(Shortcuts.preset)
                            dialog.say(error !== "" ? error : qsTr("Key set deleted"), error !== "" ? "danger" : "info")
                        }
                    }
                }
                Popover {
                    id: saveSet
                    objectName: "shortcutsSaveSet"
                    title: qsTr("Save key set")
                    y: keySetButton.height + Theme.space(1)
                    transformOrigin: Item.Top
                    onOpened: {
                        setName.text = ""
                        setName.forceActiveFocus()
                    }
                    Column {
                        spacing: Theme.space(2)
                        TextField {
                            id: setName
                            width: Theme.space(64)
                            implicitHeight: Theme.controlHeightSmall + Theme.space(1)
                            leftPadding: Theme.space(3)
                            placeholderText: qsTr("Name")
                            placeholderTextColor: Theme.textTertiary
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            Accessible.name: qsTr("Key set name")
                            onAccepted: saveButton.clicked()
                            background: Rectangle {
                                radius: height / 2
                                color: Theme.raised
                                border.width: setName.activeFocus ? Theme.focusRingWidth : 0
                                border.color: Theme.focus
                            }
                        }
                        PillButton {
                            id: saveButton
                            anchors.right: parent.right
                            compact: true
                            variant: "primary"
                            text: qsTr("Save")
                            onClicked: {
                                const error = Shortcuts.savePreset(setName.text)
                                dialog.say(error !== "" ? error : qsTr("Saved the key set “%1”").arg(setName.text.trim()),
                                           error !== "" ? "danger" : "success")
                                if (error === "")
                                    saveSet.close()
                            }
                        }
                    }
                }
            }
            PillButton {
                objectName: "shortcutsImport"
                compact: true
                iconName: "import"
                text: qsTr("Import…")
                tooltip: qsTr("Import a Premiere Pro key set (.kys) or Wabcut shortcuts (.json)")
                onClicked: importDialog.open()
            }
            PillButton {
                objectName: "shortcutsExport"
                compact: true
                iconName: "upload"
                text: qsTr("Export…")
                tooltip: qsTr("Save your changes to the shortcuts as a .json file")
                onClicked: exportDialog.open()
            }
            PillButton {
                id: resetButton
                objectName: "shortcutsReset"
                compact: true
                iconName: "rotate-ccw"
                text: qsTr("Reset")
                tooltip: qsTr("Back to Premiere Pro's default shortcuts")
                onClicked: resetConfirm.open()

                Popover {
                    id: resetConfirm
                    title: qsTr("Reset shortcuts")
                    y: resetButton.height + Theme.space(1)
                    transformOrigin: Item.Top
                    Column {
                        spacing: Theme.space(3)
                        Text {
                            width: Theme.space(64)
                            wrapMode: Text.Wrap
                            text: qsTr("Every shortcut goes back to Premiere Pro's defaults. Saved key sets are kept.")
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        Row {
                            anchors.right: parent.right
                            spacing: Theme.space(2)
                            PillButton {
                                compact: true
                                text: qsTr("Cancel")
                                onClicked: resetConfirm.close()
                            }
                            PillButton {
                                objectName: "shortcutsResetConfirm"
                                compact: true
                                variant: "primary"
                                text: qsTr("Reset")
                                onClicked: {
                                    Shortcuts.resetAll()
                                    dialog.say(qsTr("Shortcuts reset to Premiere Pro's defaults"), "success")
                                    resetConfirm.close()
                                }
                            }
                        }
                    }
                }
            }
            IconButton {
                objectName: "shortcutsClose"
                compact: true
                iconName: "x"
                tooltip: qsTr("Close")
                onClicked: dialog.close()
            }
        }

        // ---- Modifiers shown, legend
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                text: qsTr("Keys with")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            Repeater {
                model: Shortcuts.modifiers
                delegate: PillButton {
                    required property var modelData
                    objectName: "shortcutsModifier_" + modelData.bit
                    compact: true
                    variant: (dialog.modifiers & modelData.bit) !== 0 ? "primary" : "secondary"
                    text: modelData.label
                    tooltip: qsTr("Show the shortcuts that use %1").arg(modelData.label)
                    onClicked: dialog.modifiers = dialog.modifiers ^ modelData.bit
                }
            }
            Item { Layout.fillWidth: true }
            Repeater {
                model: [
                    { color: Theme.keyApplication, split: false, label: qsTr("Application") },
                    { color: Theme.keyPanel, split: false, label: qsTr("Panel") },
                    { color: Theme.keyApplication, split: true, label: qsTr("Both") }
                ]
                delegate: Row {
                    id: legend
                    required property var modelData
                    spacing: Theme.space(1)
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Theme.swatchSize
                        height: Theme.swatchSize * 0.75
                        radius: Theme.radiusBadge
                        color: legend.modelData.color
                        Rectangle {
                            visible: legend.modelData.split
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: parent.width / 2
                            radius: Theme.radiusBadge
                            color: Theme.keyPanel
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: legend.modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }
            }
        }

        // ---- Keyboard
        KeyboardView {
            id: keyboard
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            Layout.maximumWidth: implicitWidth
            Layout.alignment: Qt.AlignHCenter
            modifiers: dialog.modifiers
            selectedId: dialog.selectedKey ? dialog.selectedKey.id : ""
            onModifierToggled: bit => dialog.modifiers = dialog.modifiers ^ bit
            onKeySelected: key => dialog.selectedKey = key
            onCommandDropped: (commandId, key) => dialog.assignToKey(commandId, key)
        }

        // ---- A change to confirm, or what just happened
        Rectangle {
            objectName: "shortcutsConflict"
            Layout.fillWidth: true
            visible: dialog.pending !== null
            implicitHeight: conflictRow.implicitHeight + Theme.space(4)
            radius: Theme.radiusField
            color: Qt.alpha(Theme.warning, 0.10)
            border.width: Theme.hairline
            border.color: Qt.alpha(Theme.warning, 0.55)

            RowLayout {
                id: conflictRow
                anchors.fill: parent
                anchors.margins: Theme.space(2)
                anchors.leftMargin: Theme.space(3)
                spacing: Theme.space(2)
                Icon {
                    Layout.alignment: Qt.AlignTop
                    name: "triangle-alert"
                    size: Theme.iconSm
                    color: Theme.warning
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    Text {
                        objectName: "shortcutsConflictText"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: {
                            const p = dialog.pending
                            if (!p)
                                return ""
                            if (p.check.conflicts.length > 0)
                                return qsTr("%1 is already used by %2. Reassign it to “%3”?")
                                    .arg(p.check.chord)
                                    .arg(p.check.conflicts.map(c => qsTr("“%1” (%2)").arg(c.name).arg(c.scopeName)).join(", "))
                                    .arg(p.name)
                            return qsTr("Give %1 to “%2”?").arg(p.check.chord).arg(p.name)
                        }
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }
                    Repeater {
                        model: dialog.pending ? dialog.pending.check.shadows : []
                        delegate: Text {
                            required property var modelData
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: modelData.message
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                }
                PillButton {
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: dialog.pending = null
                }
                PillButton {
                    objectName: "shortcutsReassign"
                    compact: true
                    variant: "primary"
                    text: dialog.pending && dialog.pending.check.conflicts.length > 0 ? qsTr("Reassign") : qsTr("Assign")
                    onClicked: dialog.applyPending()
                }
            }
        }
        Rectangle {
            objectName: "shortcutsMessage"
            Layout.fillWidth: true
            visible: dialog.pending === null && dialog.message !== ""
            readonly property color tone: dialog.messageTone === "danger" ? Theme.danger
                                        : dialog.messageTone === "success" ? Theme.success : Theme.info
            implicitHeight: messageColumn.implicitHeight + Theme.space(4)
            radius: Theme.radiusField
            color: Qt.alpha(tone, 0.10)
            border.width: Theme.hairline
            border.color: Qt.alpha(tone, 0.5)

            ColumnLayout {
                id: messageColumn
                anchors.fill: parent
                anchors.margins: Theme.space(2)
                anchors.leftMargin: Theme.space(3)
                spacing: Theme.space(1)
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        objectName: "shortcutsMessageText"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: dialog.message
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }
                    IconButton {
                        compact: true
                        iconName: "x"
                        iconSize: Theme.iconXs
                        tooltip: qsTr("Dismiss")
                        onClicked: dialog.message = ""
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: dialog.unmapped.length > 0
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    text: qsTr("Not imported: %1").arg(dialog.unmapped.join(", "))
                    color: Theme.textSecondary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
        }

        // ---- Commands and the selected key
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.space(3)

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Theme.space(2)

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    TextField {
                        id: search
                        objectName: "shortcutsSearch"
                        Layout.fillWidth: true
                        implicitHeight: Theme.controlHeightSmall + Theme.space(1)
                        leftPadding: Theme.space(8)
                        rightPadding: Theme.space(3)
                        placeholderText: qsTr("Search commands or shortcuts")
                        placeholderTextColor: Theme.textTertiary
                        color: Theme.textPrimary
                        selectionColor: Theme.accentFill
                        selectedTextColor: Theme.accentOnFill
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        selectByMouse: true
                        text: Shortcuts.search
                        Accessible.name: qsTr("Search commands or shortcuts")
                        onTextEdited: Shortcuts.search = text
                        Keys.onDownPressed: commandList.list.forceActiveFocus()
                        background: Rectangle {
                            radius: height / 2
                            color: Theme.raised
                            border.width: search.activeFocus ? Theme.focusRingWidth : 0
                            border.color: Theme.focus
                        }
                        Icon {
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.space(3)
                            anchors.verticalCenter: parent.verticalCenter
                            name: "search"
                            size: Theme.iconSm
                            color: Theme.textTertiary
                        }
                    }
                    PillButton {
                        id: scopeButton
                        objectName: "shortcutsScope"
                        compact: true
                        iconName: "chevron-down"
                        text: {
                            for (const s of Shortcuts.scopes) {
                                if (s.id === Shortcuts.scopeFilter)
                                    return s.name
                            }
                            return qsTr("All commands")
                        }
                        tooltip: qsTr("Show the commands of one panel")
                        onClicked: scopeMenu.popup(scopeButton, 0, scopeButton.height + Theme.space(1))

                        OptionsMenu {
                            id: scopeMenu
                            options: Shortcuts.scopes
                            current: Shortcuts.scopeFilter
                            onChosen: id => Shortcuts.scopeFilter = id
                        }
                    }
                }

                ShortcutCommandList {
                    id: commandList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    captureFor: dialog.captureFor
                    onCaptureRequested: commandId => {
                        dialog.pending = null
                        dialog.captureFor = commandId
                    }
                    onChordCaptured: (commandId, chordText) => dialog.proposeAssign(commandId, chordText)
                    onCaptureCancelled: dialog.captureFor = ""
                    onRemoveRequested: (commandId, chordText) => {
                        const error = Shortcuts.unassign(commandId, chordText)
                        dialog.say(error !== "" ? error : qsTr("Removed %1 from “%2”").arg(Shortcuts.displayChord(chordText)).arg(Shortcuts.commandName(commandId)),
                                   error !== "" ? "danger" : "info")
                    }
                    onResetRequested: commandId => {
                        Shortcuts.resetCommand(commandId)
                        dialog.say(qsTr("“%1” has its default keys again").arg(Shortcuts.commandName(commandId)), "info")
                    }
                    onCommandSelected: commandId => dialog.currentCommand = commandId
                }
            }

            // The selected key: what it does in each panel.
            Rectangle {
                id: keyDetail
                objectName: "shortcutsKeyDetail"
                Layout.preferredWidth: Theme.space(72)
                Layout.fillHeight: true
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border

                readonly property string chord: dialog.selectedKey ? dialog.chordOfKey(dialog.selectedKey) : ""
                readonly property var bound: dialog.selectedKey && Shortcuts.revision >= 0
                                             ? Shortcuts.commandsForKey(dialog.selectedKey.key, dialog.modifiers | (dialog.selectedKey.keypad ? Qt.KeypadModifier : 0))
                                             : []

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.space(3)
                    spacing: Theme.space(2)

                    Text {
                        Layout.fillWidth: true
                        visible: dialog.selectedKey === null
                        wrapMode: Text.Wrap
                        text: qsTr("Click a key to see what it does in each panel. To give a command a shortcut, press + on its row and type the keys, or drag the command onto a key.")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }

                    Rectangle {
                        visible: dialog.selectedKey !== null
                        Layout.alignment: Qt.AlignHCenter
                        implicitWidth: keyText.implicitWidth + Theme.space(8)
                        implicitHeight: Theme.controlHeight
                        radius: Theme.radiusBadge + 2
                        color: Theme.raised
                        border.width: Theme.hairline
                        border.color: Theme.borderStrong
                        Text {
                            id: keyText
                            objectName: "shortcutsKeyChord"
                            anchors.centerIn: parent
                            text: keyDetail.chord !== "" ? Shortcuts.displayChord(keyDetail.chord) : ""
                            color: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXl
                            font.weight: Font.DemiBold
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: dialog.selectedKey !== null && keyDetail.bound.length === 0
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("Not used")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    ListView {
                        id: boundList
                        objectName: "shortcutsKeyCommands"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: Theme.space(1)
                        model: keyDetail.bound
                        delegate: Rectangle {
                            id: boundRow
                            required property var modelData
                            width: ListView.view ? ListView.view.width : 0
                            implicitHeight: boundColumn.implicitHeight + Theme.space(2)
                            radius: Theme.radiusField
                            color: Theme.raised
                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.margins: Theme.space(1)
                                width: Theme.space(1)
                                radius: width / 2
                                color: boundRow.modelData.application ? Theme.keyApplication : Theme.keyPanel
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.space(3)
                                anchors.rightMargin: Theme.space(1)
                                ColumnLayout {
                                    id: boundColumn
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Text {
                                        Layout.fillWidth: true
                                        text: boundRow.modelData.name
                                        elide: Text.ElideRight
                                        color: Theme.textPrimary
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textMd
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: boundRow.modelData.scopeName
                                        elide: Text.ElideRight
                                        color: Theme.textTertiary
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textXs
                                    }
                                }
                                IconButton {
                                    compact: true
                                    iconName: "x"
                                    iconSize: Theme.iconXs
                                    tooltip: qsTr("Remove this shortcut from “%1”").arg(boundRow.modelData.name)
                                    onClicked: Shortcuts.unassign(boundRow.modelData.commandId, boundRow.modelData.chordText)
                                }
                            }
                        }
                    }
                    PillButton {
                        objectName: "shortcutsAssignKey"
                        Layout.fillWidth: true
                        visible: dialog.selectedKey !== null && dialog.currentCommand !== ""
                        compact: true
                        variant: "primary"
                        text: qsTr("Give to “%1”").arg(Shortcuts.commandName(dialog.currentCommand))
                        tooltip: qsTr("Assign this key to the command chosen in the list")
                        onClicked: dialog.assignToKey(dialog.currentCommand, dialog.selectedKey)
                    }
                }
            }
        }

        // ---- Footer
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Changes apply at once and are saved. %1 keys shown.").arg(Shortcuts.macModifiers ? qsTr("macOS") : qsTr("Windows"))
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                objectName: "shortcutsDone"
                variant: "primary"
                text: qsTr("Done")
                onClicked: dialog.close()
            }
        }
    }

    FileDialog {
        id: importDialog
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Key sets (*.kys *.json)"), qsTr("Premiere Pro key sets (*.kys)"), qsTr("Wabcut shortcuts (*.json)")]
        onAccepted: dialog.importFile(selectedFile)
    }
    FileDialog {
        id: exportDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("Wabcut shortcuts (*.json)")]
        onAccepted: {
            const error = Shortcuts.exportOverrides(selectedFile)
            dialog.say(error !== "" ? error : qsTr("Exported your shortcut changes"), error !== "" ? "danger" : "success")
        }
    }
}
