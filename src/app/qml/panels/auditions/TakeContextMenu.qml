pragma ComponentBehavior: Bound
import QtQuick

// 0.6 (card #74): the right-click menu of a take in the Take Selector (and the Menu key / Shift+F10 on a row): Play
// (audition it in place), Use This Take, Rename… (the host opens the row's name field once the menu has closed:
// renameRequested), Duplicate Take, Move Up / Move Down and Remove Take. Every change is one undo step (Auditions).
// Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "takeContextMenu"

    /// The take (an index in Auditions.takes).
    property int takeIndex: -1
    property int takeCount: 0
    property bool takeActive: false
    /// Rename… was chosen: the name field opens after the menu has closed (it takes the focus).
    property int pendingRename: -1

    signal renameRequested(int index)

    function openAt(target: Item, point: point, index: int) {
        const takes = Auditions.takes
        menu.takeIndex = index
        menu.takeCount = takes.length
        menu.takeActive = index >= 0 && index < takes.length && takes[index].active === true
        menu.pendingRename = -1
        menu.popup(target, point.x, point.y)
    }

    onClosed: {
        const index = menu.pendingRename
        menu.pendingRename = -1
        if (index >= 0)
            menu.renameRequested(index)
    }

    ContextMenuItem {
        objectName: "takeMenuPlay"
        text: qsTr("Play")
        iconName: "play"
        onTriggered: Auditions.previewTake(menu.takeIndex)
    }
    ContextMenuItem {
        objectName: "takeMenuUse"
        text: qsTr("Use This Take")
        iconName: "check"
        enabled: !menu.takeActive
        onTriggered: Auditions.useTake(menu.takeIndex)
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "takeMenuRename"
        text: qsTr("Rename…")
        iconName: "pencil"
        onTriggered: menu.pendingRename = menu.takeIndex
    }
    ContextMenuItem {
        objectName: "takeMenuDuplicate"
        text: qsTr("Duplicate Take")
        iconName: "copy"
        onTriggered: Auditions.duplicateTake(menu.takeIndex)
    }
    ContextMenuItem {
        objectName: "takeMenuMoveUp"
        text: qsTr("Move Up")
        iconName: "chevron-up"
        enabled: menu.takeIndex > 0
        onTriggered: Auditions.moveTake(menu.takeIndex, menu.takeIndex - 1)
    }
    ContextMenuItem {
        objectName: "takeMenuMoveDown"
        text: qsTr("Move Down")
        iconName: "chevron-down"
        enabled: menu.takeIndex >= 0 && menu.takeIndex < menu.takeCount - 1
        onTriggered: Auditions.moveTake(menu.takeIndex, menu.takeIndex + 1)
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "takeMenuRemove"
        text: qsTr("Remove Take")
        iconName: "trash-2"
        onTriggered: Auditions.removeTake(menu.takeIndex)
    }
}
