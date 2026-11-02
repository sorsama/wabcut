pragma ComponentBehavior: Bound
import QtQuick

// 0.6 (card #74): the timeline clip menu's Audition submenu. On an audition (the right-clicked clip is the one the
// Auditions bridge shows, or its linked sound/picture half): its takes, the active one checked (choosing one is Use This
// Take), then Duplicate Take, Flatten Audition and Take Selector. On another clip: New Audition from the selected clips
// (one clip: an audition with a copy of it). Add Selected Clips to Audition on either, when other clips are selected. Every change is one undo step. The host calls refresh() when it
// opens. Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "auditionSubmenu"

    /// The right-clicked clip ("c7").
    property string clipKey: ""
    /// The clip is the audition Auditions shows (its takes are listed).
    property bool isAudition: false
    /// Auditions.takes of that audition as the menu opened.
    property var takes: []
    /// A keymap command to run when the menu has closed (the panel opens then).
    signal commandRequested(string command)

    function refresh() {
        // The audition's picture or its sound half (a linked partner) both show its takes.
        menu.isAudition = menu.clipKey !== "" && Auditions.isShownAudition(menu.clipKey) && Auditions.count > 1
        menu.takes = menu.isAudition ? Auditions.takes : []
    }

    title: menu.isAudition ? qsTr("Audition (%1)").arg(Auditions.badge) : qsTr("Audition")
    onClipKeyChanged: refresh()

    Instantiator {
        model: menu.takes
        delegate: ContextMenuItem {
            required property var modelData
            objectName: "auditionTake_" + modelData.index
            text: qsTr("Take %1 · %2").arg(modelData.index + 1).arg(modelData.name)
            checkable: true
            checked: modelData.active === true
            onTriggered: Auditions.useTake(modelData.index)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object as ContextMenuItem)
        onObjectRemoved: (index, object) => menu.removeItem(object as ContextMenuItem)
    }
    ContextMenuSeparator {
        visible: menu.isAudition
        height: visible ? implicitHeight : 0
    }
    ContextMenuItem {
        objectName: "auditionMenuNew"
        visible: !menu.isAudition
        height: visible ? implicitHeight : 0
        text: ProjectController.selectedClips.length > 1 ? qsTr("New Audition") : qsTr("Duplicate as Audition")
        iconName: "plus"
        command: "audition.create"
        onTriggered: Auditions.createFromSelection()
    }
    ContextMenuItem {
        objectName: "auditionMenuAdd"
        visible: Auditions.canAddSelected
        height: visible ? implicitHeight : 0
        text: qsTr("Add Selected Clips to Audition")
        iconName: "folder-plus"
        command: "audition.addSelected"
        onTriggered: Auditions.addSelected()
    }
    ContextMenuItem {
        objectName: "auditionMenuDuplicate"
        visible: menu.isAudition
        height: visible ? implicitHeight : 0
        text: qsTr("Duplicate Take")
        iconName: "copy"
        command: "audition.duplicateTake"
        onTriggered: Auditions.duplicateTake(-1)
    }
    ContextMenuItem {
        objectName: "auditionMenuFlatten"
        visible: menu.isAudition
        height: visible ? implicitHeight : 0
        text: qsTr("Flatten Audition")
        iconName: "minimize-2"
        command: "audition.flatten"
        onTriggered: Auditions.flatten()
    }
    ContextMenuItem {
        objectName: "auditionMenuSelector"
        text: qsTr("Take Selector")
        iconName: "layers"
        command: "audition.showTakeSelector"
        onTriggered: menu.commandRequested("audition.showTakeSelector")
    }
}
