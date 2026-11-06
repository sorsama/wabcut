pragma ComponentBehavior: Bound
import QtQuick

// The right-click menu of a track group's header row (0.7, card 124; ProjectController.trackGroupInfo, read when it
// opens): Rename Group..., Label (the group's colour, Premiere's 16 labels), Collapse / Expand Group, Track Height (the
// group's tracks at the minimum, the default or a large height: view state, TimelineView.setGroupTrackHeight), the
// switches for every track of the group (Mute / Show Output, Solo, Lock, Target: checked when all of its tracks have it),
// an audio group's Output (the Mix, or a new submix named after the group: one fader and effect stack for the whole group)
// and Ungroup. One undo step each. A field would lose its focus to the closing menu, so Rename... runs once the menu has
// closed. Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "trackGroupMenu"

    property string groupId: ""
    /// The lanes (TimelineView): the track height rows act on it.
    property var view: null
    /// What the group is (ProjectController.trackGroupInfo), read when the menu opens.
    property var info: ({})
    readonly property bool audio: (menu.info.kind ?? "") === "audio"
    /// The item to run once the menu has closed: "rename".
    property string pending: ""

    /// Rename Group...: the header opens its name field.
    signal renameRequested()
    /// An edit was refused: `message` says why.
    signal failed(string message)

    function openFor(target, group, point) {
        groupId = group
        info = ProjectController.trackGroupInfo(group)
        pending = ""
        popup(target, point.x, point.y)
    }
    function report(ok) {
        if (!ok)
            menu.failed(ProjectController.lastError)
    }
    function setHeight(height) {
        if (menu.view)
            menu.view.setGroupTrackHeight(menu.groupId, height)
    }
    function flag(name, state) {
        menu.report(ProjectController.setTrackGroupFlag(menu.groupId, name, state !== 2))
    }

    onClosed: {
        const action = menu.pending
        menu.pending = ""
        if (action === "rename")
            menu.renameRequested()
    }

    Item {
        implicitWidth: titleLabel.implicitWidth + Theme.space(6)
        implicitHeight: titleLabel.implicitHeight + Theme.space(2)
        Text {
            id: titleLabel
            objectName: "trackGroupMenuTitle"
            x: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%1 · %2").arg(menu.info.name ?? "").arg(qsTr("%n track(s)", "", menu.info.count ?? 0))
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
    }
    ContextMenuItem {
        objectName: "trackGroupMenuRename"
        text: qsTr("Rename Group…")
        iconName: "pencil"
        onTriggered: menu.pending = "rename"
    }
    ContextMenuItem {
        objectName: "trackGroupMenuCollapse"
        text: (menu.info.collapsed ?? false) ? qsTr("Expand Group") : qsTr("Collapse Group")
        iconName: (menu.info.collapsed ?? false) ? "unfold-vertical" : "fold-vertical"
        onTriggered: menu.report(ProjectController.setTrackGroupCollapsed(menu.groupId, !(menu.info.collapsed ?? false)))
    }
    ContextMenu {
        id: labelMenu
        objectName: "trackGroupMenuLabel"
        title: qsTr("Label")
        Instantiator {
            model: Library.labelNames
            delegate: ContextMenuItem {
                id: colour
                required property string modelData
                required property int index
                objectName: "trackGroupMenuLabel_" + index
                text: modelData
                iconName: "square"
                iconColor: Theme.clipColor(index)
                checkable: true
                checked: (menu.info.colorIndex ?? -1) === index
                onTriggered: {
                    colour.checked = Qt.binding(() => (menu.info.colorIndex ?? -1) === colour.index)
                    menu.report(ProjectController.setTrackGroupColor(menu.groupId, colour.index))
                }
            }
            onObjectAdded: (index, object) => labelMenu.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => labelMenu.removeItem(object as ContextMenuItem)
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            id: noLabel
            objectName: "trackGroupMenuLabelNone"
            text: qsTr("No Label")
            checkable: true
            checked: (menu.info.colorIndex ?? -1) < 0
            onTriggered: {
                noLabel.checked = Qt.binding(() => (menu.info.colorIndex ?? -1) < 0)
                menu.report(ProjectController.setTrackGroupColor(menu.groupId, -1))
            }
        }
    }
    ContextMenu {
        id: heightMenu
        objectName: "trackGroupMenuHeight"
        title: qsTr("Track Height")
        ContextMenuItem {
            objectName: "trackGroupHeightMinimum"
            text: qsTr("Minimum")
            onTriggered: menu.setHeight(Theme.trackHeightMin)
        }
        ContextMenuItem {
            objectName: "trackGroupHeightDefault"
            text: qsTr("Default")
            onTriggered: menu.setHeight(Theme.trackHeight(menu.info.kind ?? "video"))
        }
        ContextMenuItem {
            objectName: "trackGroupHeightLarge"
            text: qsTr("Large")
            onTriggered: menu.setHeight(Theme.trackHeight(menu.info.kind ?? "video") * 2)
        }
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        id: muteItem
        objectName: "trackGroupMenuMute"
        text: menu.audio ? qsTr("Mute Group") : qsTr("Show Group Output")
        iconName: menu.audio ? "volume-x" : "eye"
        checkable: true
        checked: menu.audio ? (menu.info.muted ?? 0) === 2 : (menu.info.enabled ?? 2) === 2
        onTriggered: {
            const state = menu.audio ? (menu.info.muted ?? 0) : (menu.info.enabled ?? 2)
            muteItem.checked = Qt.binding(() => menu.audio ? (menu.info.muted ?? 0) === 2 : (menu.info.enabled ?? 2) === 2)
            if (menu.audio)
                menu.flag("muted", state)
            else
                menu.flag("enabled", state)
        }
    }
    ContextMenuItem {
        id: soloItem
        objectName: "trackGroupMenuSolo"
        text: qsTr("Solo Group")
        iconName: menu.audio ? "headphones" : "crosshair"
        checkable: true
        checked: (menu.info.solo ?? 0) === 2
        onTriggered: {
            soloItem.checked = Qt.binding(() => (menu.info.solo ?? 0) === 2)
            menu.flag("solo", menu.info.solo ?? 0)
        }
    }
    ContextMenuItem {
        id: lockItem
        objectName: "trackGroupMenuLock"
        text: qsTr("Lock Group")
        iconName: "lock"
        checkable: true
        checked: (menu.info.locked ?? 0) === 2
        onTriggered: {
            lockItem.checked = Qt.binding(() => (menu.info.locked ?? 0) === 2)
            menu.flag("locked", menu.info.locked ?? 0)
        }
    }
    ContextMenuItem {
        id: targetItem
        objectName: "trackGroupMenuTarget"
        text: qsTr("Target Group")
        iconName: "crosshair"
        checkable: true
        checked: (menu.info.targeted ?? 0) === 2
        onTriggered: {
            targetItem.checked = Qt.binding(() => (menu.info.targeted ?? 0) === 2)
            menu.flag("targeted", menu.info.targeted ?? 0)
        }
    }
    ContextMenu {
        id: outputMenu
        objectName: "trackGroupMenuOutput"
        title: qsTr("Output")
        enabled: menu.audio
        ContextMenuItem {
            id: toMix
            objectName: "trackGroupOutputMix"
            text: qsTr("Mix")
            checkable: true
            checked: (menu.info.output ?? "mix") === "mix"
            onTriggered: {
                toMix.checked = Qt.binding(() => (menu.info.output ?? "mix") === "mix")
                menu.report(ProjectController.routeTrackGroup(menu.groupId, "mix"))
            }
        }
        ContextMenuItem {
            objectName: "trackGroupOutputNewSubmix"
            text: qsTr("New Submix for This Group")
            onTriggered: menu.report(ProjectController.routeTrackGroup(menu.groupId, "newSubmix"))
        }
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "trackGroupMenuUngroup"
        text: qsTr("Ungroup Tracks")
        iconName: "unlink"
        onTriggered: menu.report(ProjectController.ungroupTracks(menu.groupId))
    }
}
