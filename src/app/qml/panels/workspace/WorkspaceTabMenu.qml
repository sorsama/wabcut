import QtQuick

// 0.1.3 workspaces: a workspace tab's menu (right-click, the Menu key or Shift+F10 on the tab). Its own QtQuick-only
// file: a file importing QtQuick.Controls would get Qt's ContextMenu instead of Wabcut's.
ContextMenu {
    id: root
    objectName: "workspaceTabMenu"

    /// The workspace the menu is for.
    property int workspaceId: 0
    readonly property int index: Workspaces.all.findIndex(w => w.id === root.workspaceId)
    /// The tabs start an inline rename.
    signal renameRequested(int id)
    /// A workspace was made from this one: the tabs switch to it and rename it.
    signal created(int id)

    ContextMenuItem {
        objectName: "workspaceMenuRename"
        text: qsTr("Rename")
        iconName: "pencil"
        onTriggered: root.renameRequested(root.workspaceId)
    }
    ContextMenuItem {
        objectName: "workspaceMenuDuplicate"
        text: qsTr("Duplicate")
        iconName: "copy"
        onTriggered: root.created(Workspaces.create(root.workspaceId, ""))
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "workspaceMenuMoveLeft"
        text: qsTr("Move left")
        iconName: "chevron-left"
        enabled: root.index > 0
        onTriggered: Workspaces.move(root.workspaceId, -1)
    }
    ContextMenuItem {
        objectName: "workspaceMenuMoveRight"
        text: qsTr("Move right")
        iconName: "chevron-right"
        enabled: root.index >= 0 && root.index < Workspaces.all.length - 1
        onTriggered: Workspaces.move(root.workspaceId, 1)
    }
    ContextMenuSeparator {}
    // Every panel back where the workspace's preset puts them.
    ContextMenuItem {
        objectName: "workspaceMenuReset"
        text: qsTr("Reset layout")
        iconName: "rotate-ccw"
        onTriggered: Workspaces.reset(root.workspaceId)
    }
    // The last workspace stays.
    ContextMenuItem {
        objectName: "workspaceMenuDelete"
        text: qsTr("Delete workspace")
        iconName: "trash-2"
        enabled: Workspaces.all.length > 1
        onTriggered: Workspaces.remove(root.workspaceId)
    }
}
