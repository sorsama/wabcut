import QtQuick

// 0.1.3 workspaces: a workspace tab's menu (right-click, the Menu key or Shift+F10 on the tab). Its own QtQuick-only
// file: a file importing QtQuick.Controls would get Qt's ContextMenu instead of Wabcut's.
// P1 round (#32): the Window menu's workspace rows too - Save Changes to This Workspace, Save as New Workspace…, Reset to
// Saved Layout, Reset to <Preset> Preset… and Lock Workspace (the window runs them: actionRequested, Main.menuAction).
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
    /// P1 round (#32): an action only the window can do for the workspace (Main.menuAction's "saveWorkspaceChanges",
    /// "saveAsNewWorkspace", "resetWorkspaceToPreset", "lockWorkspace"), once the menu has closed.
    signal actionRequested(string action, int id)
    property string pendingAction: ""
    onClosed: {
        const action = root.pendingAction
        root.pendingAction = ""
        if (action !== "")
            Qt.callLater(() => root.actionRequested(action, root.workspaceId))
    }

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
    ContextMenuItem {
        objectName: "workspaceMenuSave"
        text: qsTr("Save Changes to This Workspace")
        iconName: "save"
        onTriggered: root.pendingAction = "saveWorkspaceChanges"
    }
    ContextMenuItem {
        objectName: "workspaceMenuSaveAsNew"
        text: qsTr("Save as New Workspace…")
        iconName: "copy"
        onTriggered: root.pendingAction = "saveAsNewWorkspace"
    }
    // Every panel back where Save Changes kept them, else where the workspace's preset puts them.
    ContextMenuItem {
        objectName: "workspaceMenuReset"
        text: qsTr("Reset to Saved Layout")
        iconName: "rotate-ccw"
        onTriggered: Workspaces.reset(root.workspaceId)
    }
    ContextMenuItem {
        objectName: "workspaceMenuResetToPreset"
        text: qsTr("Reset to %1 Preset…").arg(Workspaces.presetLabel(Workspaces.resolve(root.workspaceId).preset))
        iconName: "layout-grid"
        onTriggered: root.pendingAction = "resetWorkspaceToPreset"
    }
    ContextMenuItem {
        objectName: "workspaceMenuLock"
        text: qsTr("Lock Workspace")
        iconName: "lock"
        checkable: true
        checked: Workspaces.isLocked(root.workspaceId)
        onTriggered: {
            checked = Qt.binding(() => Workspaces.isLocked(root.workspaceId))
            root.pendingAction = "lockWorkspace"
        }
    }
    ContextMenuSeparator {}
    // The last workspace stays.
    ContextMenuItem {
        objectName: "workspaceMenuDelete"
        text: qsTr("Delete workspace")
        iconName: "trash-2"
        enabled: Workspaces.all.length > 1
        onTriggered: Workspaces.remove(root.workspaceId)
    }
}
