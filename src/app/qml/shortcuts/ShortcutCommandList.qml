pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The Keyboard Shortcuts editor's command list (Shortcuts.commands, filtered by Shortcuts.search and scopeFilter),
// in sections by menu group. A row: a drag grip (drop the command on a key), the name, its panel (panel commands),
// its shortcuts as keycaps (× removes one), + to type a new one, and a reset dot when it differs from the key set's
// defaults. Commands Wabcut does not run yet are dimmed; their keys are kept for when they arrive.
// Typing a shortcut: the row's capture field takes every key (it claims ShortcutOverride, so no window shortcut
// fires) and reports the chord (chordCaptured); Esc cancels. Keyboard: ↑/↓ choose a row, Return types a shortcut.
FocusScope {
    id: root
    objectName: "shortcutsCommandList"

    /// The command whose new shortcut is being typed ("" when none).
    property string captureFor: ""
    readonly property alias list: list

    signal captureRequested(string commandId)
    signal chordCaptured(string commandId, string chordText)
    signal captureCancelled()
    signal removeRequested(string commandId, string chordText)
    signal resetRequested(string commandId)
    signal commandSelected(string commandId)

    function reveal(commandId) {
        const row = Shortcuts.commands.indexOf(commandId)
        if (row < 0)
            return false
        list.currentIndex = row
        list.positionViewAtIndex(row, ListView.Contain)
        return true
    }

    ListView {
        id: list
        objectName: "shortcutsCommands"
        anchors.fill: parent
        clip: true
        model: Shortcuts.commands
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: true
        keyNavigationEnabled: true
        highlightMoveDuration: 0
        currentIndex: -1
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Commands")
        ScrollBar.vertical: ThinScrollBar {}

        onCurrentIndexChanged: {
            if (currentIndex >= 0)
                root.commandSelected(Shortcuts.commands.get(currentIndex).commandId)
        }
        Keys.onReturnPressed: {
            if (currentIndex >= 0)
                root.captureRequested(Shortcuts.commands.get(currentIndex).commandId)
        }

        section.property: "group"
        section.criteria: ViewSection.FullString
        section.delegate: Item {
            id: sectionHeader
            required property string section
            width: ListView.view ? ListView.view.width : 0
            height: Theme.sectionHeaderHeight
            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.space(2)
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.space(1)
                text: sectionHeader.section
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.labelTracking
            }
        }

        delegate: Rectangle {
            id: row
            required property int index
            required property string commandId
            required property string name
            required property string premiereName
            required property string scopeName
            required property bool application
            required property var shortcuts
            required property var chords
            required property bool overridden
            required property bool available
            readonly property bool current: ListView.isCurrentItem
            readonly property bool capturing: root.captureFor === commandId

            objectName: "shortcutRow_" + commandId
            width: ListView.view ? ListView.view.width - Theme.space(3) : 0
            height: Theme.controlHeight
            radius: Theme.radiusField
            color: capturing ? Theme.accentWash : current ? Theme.hover : rowArea.containsMouse ? Qt.alpha(Theme.hover, 0.5) : "transparent"
            border.width: current && list.activeFocus ? Theme.hairline : 0
            border.color: Theme.accentStroke

            MouseArea {
                id: rowArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    list.currentIndex = row.index
                    list.forceActiveFocus(Qt.MouseFocusReason)
                }
                onDoubleClicked: root.captureRequested(row.commandId)
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space(1)
                anchors.rightMargin: Theme.space(2)
                spacing: Theme.space(2)

                // Drag grip: drop the command on a key of the keyboard.
                Item {
                    id: grip
                    implicitWidth: Theme.iconSm + Theme.space(2)
                    implicitHeight: Theme.controlHeightSmall
                    Accessible.ignored: true
                    Icon {
                        anchors.centerIn: parent
                        name: "grip-vertical"
                        size: Theme.iconSm
                        color: gripHandler.active || gripHover.hovered ? Theme.accentStroke : Theme.textTertiary
                    }
                    HoverHandler {
                        id: gripHover
                        cursorShape: Qt.OpenHandCursor
                    }
                    Item {
                        id: dragSource
                        anchors.fill: parent
                        Drag.active: gripHandler.active
                        Drag.dragType: Drag.Automatic
                        Drag.supportedActions: Qt.CopyAction
                        Drag.mimeData: { "application/x-wabcut-command": row.commandId }
                        DragHandler {
                            id: gripHandler
                            target: null
                        }
                    }
                }

                // Name and panel
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: Theme.space(30)
                    spacing: Theme.space(2)
                    Text {
                        Layout.fillWidth: true
                        text: row.name
                        elide: Text.ElideRight
                        color: row.available ? Theme.textPrimary : Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }
                    Rectangle {
                        visible: !row.application
                        implicitWidth: scopeText.implicitWidth + Theme.space(3)
                        implicitHeight: Theme.space(5)
                        radius: Theme.radiusPill
                        color: Qt.alpha(Theme.keyPanel, 0.16)
                        border.width: Theme.hairline
                        border.color: Qt.alpha(Theme.keyPanel, 0.6)
                        Text {
                            id: scopeText
                            anchors.centerIn: parent
                            text: row.scopeName
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                }

                // Shortcuts (keycaps), or the capture field
                Row {
                    visible: !row.capturing
                    spacing: Theme.space(1)
                    Repeater {
                        model: row.shortcuts
                        delegate: Rectangle {
                            id: chip
                            required property string modelData
                            required property int index
                            implicitWidth: chipRow.implicitWidth + Theme.space(3)
                            implicitHeight: Theme.space(6)
                            radius: Theme.radiusBadge + 1
                            color: Theme.raised
                            border.width: Theme.hairline
                            border.color: chipHover.hovered ? Theme.borderStrong : Theme.border
                            HoverHandler { id: chipHover }
                            Row {
                                id: chipRow
                                anchors.centerIn: parent
                                spacing: Theme.space(1)
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: chip.modelData
                                    color: Theme.textPrimary
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.textSm
                                }
                                IconButton {
                                    id: removeButton
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: chipHover.hovered || removeButton.activeFocus
                                    compact: true
                                    iconName: "x"
                                    iconSize: Theme.iconXs
                                    implicitWidth: Theme.iconSm
                                    implicitHeight: Theme.iconSm
                                    tooltip: qsTr("Remove %1 from “%2”").arg(chip.modelData).arg(row.name)
                                    onClicked: root.removeRequested(row.commandId, row.chords[chip.index])
                                }
                            }
                        }
                    }
                }
                Rectangle {
                    id: capture
                    objectName: "shortcutCapture"
                    visible: row.capturing
                    implicitWidth: Theme.space(44)
                    implicitHeight: Theme.space(6)
                    radius: Theme.radiusBadge + 1
                    color: Theme.raised
                    border.width: Theme.focusRingWidth
                    border.color: Theme.accentStroke
                    focus: row.capturing
                    property string held: ""

                    onVisibleChanged: {
                        held = ""
                        if (visible)
                            forceActiveFocus()
                    }
                    Text {
                        anchors.centerIn: parent
                        text: capture.held !== "" ? capture.held + "…" : qsTr("Type a shortcut… (Esc cancels)")
                        color: capture.held !== "" ? Theme.textPrimary : Theme.textTertiary
                        font.family: capture.held !== "" ? Theme.fontMono : Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    // Every key belongs to the capture: no window shortcut or keymap command fires.
                    Keys.onShortcutOverride: event => event.accepted = true
                    Keys.onPressed: event => {
                        event.accepted = true
                        if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) {
                            root.captureCancelled()
                            return
                        }
                        const chord = Shortcuts.chordText(event.key, event.modifiers)
                        if (chord === "") {
                            // A modifier on its own: show what is held so far.
                            const parts = []
                            if (event.modifiers & Qt.ControlModifier)
                                parts.push(Shortcuts.macModifiers ? "⌘" : qsTr("Ctrl"))
                            if (event.modifiers & Qt.AltModifier)
                                parts.push(Shortcuts.macModifiers ? "⌥" : qsTr("Alt"))
                            if (event.modifiers & Qt.ShiftModifier)
                                parts.push(Shortcuts.macModifiers ? "⇧" : qsTr("Shift"))
                            if (event.modifiers & Qt.MetaModifier)
                                parts.push(Shortcuts.macModifiers ? "⌃" : qsTr("Win"))
                            capture.held = parts.join("+")
                            return
                        }
                        root.chordCaptured(row.commandId, chord)
                    }
                    Keys.onReleased: event => event.accepted = true
                }

                IconButton {
                    objectName: "shortcutAdd"
                    compact: true
                    iconName: "plus"
                    iconSize: Theme.iconXs
                    tooltip: qsTr("Add a shortcut to “%1”").arg(row.name)
                    onClicked: {
                        list.currentIndex = row.index
                        root.captureRequested(row.commandId)
                    }
                }
                Item {
                    implicitWidth: Theme.controlHeightSmall
                    implicitHeight: Theme.controlHeightSmall
                    IconButton {
                        anchors.fill: parent
                        visible: row.overridden
                        compact: true
                        iconName: "rotate-ccw"
                        iconSize: Theme.iconXs
                        tooltip: qsTr("Reset “%1” to the default keys").arg(row.name)
                        onClicked: root.resetRequested(row.commandId)
                    }
                }
            }

            Loader {
                active: !row.available && rowArea.containsMouse
                sourceComponent: Tooltip {
                    parent: row
                    visible: true
                    text: qsTr("Not in Wabcut yet: its keys are kept for when it arrives")
                }
            }
        }
    }

    EmptyState {
        objectName: "shortcutsNoMatch"
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.space(8), Theme.space(100))
        visible: list.count === 0
        illustration: "search"
        text: qsTr("No command matches “%1”").arg(Shortcuts.search)
    }
}
