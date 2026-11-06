pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// A track group's header row in the Tracks panel (0.7, card 124; a TimelineView.groupRows delegate): above the first track
// of the group, in the column of track headers. A chevron folds the group's tracks into this row (a collapsed group is
// this row alone; its lane shows where its tracks have clips) and shows them again; the name (double-click, or Rename
// Group... in the menu: an inline field, Enter keeps it, Esc cancels) and a badge with the number of tracks; the switches
// every track of the group takes together - mute (audio) or output (video), solo, lock - lit while all of the tracks have
// them, in an amber / green / plain ring while only some do. Right click (or the Menu key / Shift+F10 on the chevron) for
// the group's menu: label colour, track height, target, output, Ungroup. A bar down the left edge carries the group's
// label colour. Every change is one undo step through ProjectController.
Item {
    id: root

    /// Which of the lanes' group rows this is (TimelineView.groupRows; the Repeater counts the rows, so a change to a group
    /// updates this header in place and keeps the keyboard focus on its chevron).
    required property int index
    /// The lanes (TimelinePanel sets it): the rows, and the track height rows of the menu act on it.
    property TimelineView view: null

    readonly property var row: root.view ? (root.view.groupRows[root.index] ?? ({})) : ({})
    readonly property string groupId: root.row.id ?? ""
    readonly property string groupName: root.row.name ?? ""
    readonly property bool audio: (root.row.kind ?? "") === "audio"
    readonly property bool collapsed: root.row.collapsed === true
    readonly property int labelIndex: root.row.colorIndex ?? -1
    readonly property int trackCount: root.row.count ?? 0
    /// How many of the group's tracks have each switch: 0 none, 1 some, 2 all.
    readonly property int mutedState: root.row.muted ?? 0
    readonly property int soloState: root.row.solo ?? 0
    readonly property int lockedState: root.row.locked ?? 0
    readonly property int outputState: root.row.enabled ?? 2
    /// The name field is open (Rename Group...).
    property bool renaming: false

    /// An edit was refused: the Tracks panel shows `text`.
    signal messageRequested(string text)

    objectName: "trackGroupHeader_" + root.groupId
    y: root.row.y ?? 0
    height: root.row.height ?? 0
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Track group %1, %2, %3").arg(root.groupName).arg(qsTr("%n track(s)", "", root.trackCount))
                         .arg(root.collapsed ? qsTr("collapsed") : qsTr("expanded"))

    function openMenu(target, point) {
        menuLoader.active = true
        const menu = menuLoader.item as TrackGroupMenu
        menu.openFor(target, root.groupId, point)
    }
    function startRename() {
        root.renaming = true
    }
    /// Closes the name field: `keep` renames (one undo step), `refocus` puts the focus back on the chevron.
    function finishRename(name: string, keep: bool, refocus: bool) {
        if (!root.renaming)
            return
        root.renaming = false
        if (keep && name.trim() !== "" && name.trim() !== root.groupName
                && !ProjectController.renameTrackGroup(root.groupId, name))
            root.messageRequested(ProjectController.lastError)
        if (refocus)
            fold.forceActiveFocus(Qt.OtherFocusReason)
    }
    function toggleFlag(flag: string, state: int) {
        if (!ProjectController.setTrackGroupFlag(root.groupId, flag, state !== 2))
            root.messageRequested(ProjectController.lastError)
    }

    // The row is a pill on the raised surface, like the track headers below it.
    Rectangle {
        id: plate
        anchors.fill: parent
        anchors.topMargin: Theme.hairline
        anchors.bottomMargin: Theme.hairline
        radius: Theme.timelinePill(height)
        color: Theme.raised
        border.width: Theme.hairline
        border.color: root.labelIndex >= 0 ? Theme.clipColor(root.labelIndex) : Theme.border
    }
    // The group's label colour, a bar down the left edge (the track headers' label bar, kept for the group).
    Rectangle {
        objectName: "trackGroupStripe_" + root.groupId
        visible: root.labelIndex >= 0
        anchors.left: plate.left
        anchors.leftMargin: Theme.hairline
        anchors.top: plate.top
        anchors.bottom: plate.bottom
        anchors.topMargin: Theme.space(1)
        anchors.bottomMargin: Theme.space(1)
        width: Theme.space(0.75)
        radius: width / 2
        color: root.labelIndex >= 0 ? Theme.clipColor(root.labelIndex) : "transparent"
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: eventPoint => root.openMenu(root, eventPoint.position)
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onDoubleTapped: eventPoint => {
            // Only on the background or the name: the chevron and the switches keep their own clicks.
            const hit = root.childAt(eventPoint.position.x, eventPoint.position.y)
            if (hit === null || hit === plate || hit === nameLabel)
                root.startRename()
        }
    }
    Loader {
        id: menuLoader
        objectName: "trackGroupMenuLoader"
        active: false
        sourceComponent: TrackGroupMenu {
            view: root.view
            onRenameRequested: root.startRename()
            onFailed: message => root.messageRequested(message)
        }
    }

    IconButton {
        id: fold
        objectName: "trackGroupFold_" + root.groupId
        anchors.left: plate.left
        anchors.leftMargin: Theme.space(1.5)
        anchors.verticalCenter: parent.verticalCenter
        compact: true
        iconName: "chevron-down"
        iconSize: Theme.iconSm
        rotation: root.collapsed ? -90 : 0
        tooltip: root.collapsed ? qsTr("Expand %1").arg(root.groupName) : qsTr("Collapse %1").arg(root.groupName)
        Accessible.name: root.collapsed ? qsTr("Expand group %1").arg(root.groupName) : qsTr("Collapse group %1").arg(root.groupName)
        Accessible.role: Accessible.Button
        Behavior on rotation {
            enabled: !Theme.reduceMotion
            NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic }
        }
        onClicked: {
            if (!ProjectController.setTrackGroupCollapsed(root.groupId, !root.collapsed))
                root.messageRequested(ProjectController.lastError)
        }
        // The Menu key / Shift+F10 open the group's menu from the chevron (the right click has no keyboard twin).
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier) !== 0)) {
                root.openMenu(fold, Qt.point(0, fold.height))
                event.accepted = true
            }
        }
        Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Menu
    }

    // The name, and how many tracks the group holds.
    Text {
        id: nameLabel
        objectName: "trackGroupName_" + root.groupId
        anchors.left: fold.right
        anchors.leftMargin: Theme.space(1)
        anchors.right: countBadge.left
        anchors.rightMargin: Theme.space(1)
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.renaming
        text: root.groupName
        elide: Text.ElideRight
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Bold
    }
    Rectangle {
        id: countBadge
        objectName: "trackGroupCount_" + root.groupId
        anchors.right: toggles.left
        anchors.rightMargin: Theme.space(1)
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.renaming
        width: Math.max(height, countText.implicitWidth + Theme.space(2))
        height: Theme.space(3.5)
        radius: height / 2
        color: Theme.well
        Text {
            id: countText
            anchors.centerIn: parent
            text: root.trackCount
            color: Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
            font.weight: Font.Bold
        }
        HoverHandler { id: countHover }
        Loader {
            active: countHover.hovered
            sourceComponent: Tooltip {
                parent: countBadge
                visible: true
                text: qsTr("%n track(s) in %1", "", root.trackCount).arg(root.groupName)
            }
        }
    }
    // Rename Group...: a field over the row (Enter keeps the name, Esc cancels, leaving it keeps it too).
    Loader {
        id: renameField
        objectName: "trackGroupRenameLoader"
        active: root.renaming
        anchors.left: fold.right
        anchors.leftMargin: Theme.space(1)
        anchors.right: parent.right
        anchors.rightMargin: Theme.space(1)
        anchors.verticalCenter: parent.verticalCenter
        z: 3
        sourceComponent: FieldInput {
            objectName: "trackGroupRenameField"
            text: root.groupName
            Accessible.name: qsTr("Name of group %1").arg(root.groupName)
            Component.onCompleted: {
                forceActiveFocus()
                selectAll()
            }
            onAccepted: root.finishRename(text, true, true)
            onActiveFocusChanged: {
                if (!activeFocus)
                    root.finishRename(text, true, false)
            }
            Keys.onEscapePressed: event => {
                root.finishRename("", false, true)
                event.accepted = true
            }
            Keys.onShortcutOverride: event => event.accepted = true
        }
    }

    // The switches every track of the group takes together. A click turns them all on, or all off when they all were on.
    Row {
        id: toggles
        anchors.right: plate.right
        anchors.rightMargin: Theme.space(1)
        anchors.verticalCenter: parent.verticalCenter
        spacing: 0
        visible: !root.renaming

        IconButton {
            objectName: "trackGroupOutput_" + root.groupId
            visible: !root.audio
            compact: true
            implicitWidth: Theme.space(5.5)
            iconName: root.outputState === 0 ? "eye-off" : "eye"
            iconSize: Theme.iconSm
            iconColor: root.outputState === 1 ? Theme.warning : Theme.textSecondary
            tooltip: root.outputState === 2 ? qsTr("Hide the output of every track in %1").arg(root.groupName)
                                            : qsTr("Show the output of every track in %1").arg(root.groupName)
            Accessible.role: Accessible.CheckBox
            Accessible.checked: root.outputState === 2
            onClicked: root.toggleFlag("enabled", root.outputState)
        }
        IconButton {
            objectName: "trackGroupMute_" + root.groupId
            visible: root.audio
            compact: true
            implicitWidth: Theme.space(5.5)
            checkable: true
            checked: root.mutedState === 2
            iconName: root.mutedState === 2 ? "volume-x" : "volume-2"
            iconSize: Theme.iconSm
            iconColor: root.mutedState === 1 ? Theme.warning : Theme.textSecondary
            checkedColor: Theme.warning
            checkedIconColor: Theme.clipInk
            tooltip: root.mutedState === 2 ? qsTr("Unmute every track in %1").arg(root.groupName)
                                           : qsTr("Mute every track in %1").arg(root.groupName)
            onClicked: root.toggleFlag("muted", root.mutedState)
        }
        IconButton {
            objectName: "trackGroupSolo_" + root.groupId
            compact: true
            implicitWidth: Theme.space(5.5)
            checkable: true
            checked: root.soloState === 2
            iconName: root.audio ? "headphones" : "crosshair"
            iconSize: Theme.iconSm
            iconColor: root.soloState === 1 ? Theme.success : Theme.textSecondary
            checkedColor: Theme.success
            checkedIconColor: Theme.clipInk
            tooltip: root.soloState === 2 ? qsTr("Unsolo every track in %1").arg(root.groupName)
                                          : qsTr("Solo every track in %1").arg(root.groupName)
            onClicked: root.toggleFlag("solo", root.soloState)
        }
        IconButton {
            objectName: "trackGroupLock_" + root.groupId
            compact: true
            implicitWidth: Theme.space(5.5)
            checkable: true
            checked: root.lockedState === 2
            iconName: root.lockedState === 2 ? "lock" : "lock-open"
            iconSize: Theme.iconSm
            iconColor: root.lockedState === 1 ? Theme.textPrimary : Theme.textSecondary
            tooltip: root.lockedState === 2 ? qsTr("Unlock every track in %1").arg(root.groupName)
                                            : qsTr("Lock every track in %1").arg(root.groupName)
            onClicked: root.toggleFlag("locked", root.lockedState)
        }
    }
}
