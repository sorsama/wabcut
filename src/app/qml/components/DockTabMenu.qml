import QtQuick

// A panel tab's menu (UX rework, Premiere's panel menu): right-click a group's tab or a lone panel's tab, or press the
// Menu key / Shift+F10 on a group's tabs. Close Panel, Close Other Panels in Group, Undock Panel (a window of its own),
// Undock Panel Group (or Dock Panel Group while it floats), Maximize / Restore Frame. DockSlot makes it on first use.
// Its own QtQuick-only file: a file importing QtQuick.Controls would get Qt's ContextMenu instead of Wabcut's. The
// choice runs once the menu has closed (DockManager.tabMenuAction), so a group or tab that goes away takes no open
// menu with it.
ContextMenu {
    id: menu
    objectName: "dockTabMenu"

    /// The DockSlot of the group (typed Item: DockSlot itself creates the menu and sets it).
    property Item host: null
    readonly property DockSlot owner: menu.host as DockSlot
    /// The panel whose tab was clicked (a Workspaces.catalog key).
    property string panel: ""
    readonly property var group: menu.owner && menu.owner.manager ? menu.owner.manager.leafOf(menu.owner.dockKey) : null
    readonly property bool several: menu.group !== null && menu.group.panels.length > 1
    readonly property bool floating: menu.owner !== null && menu.owner.floating
    readonly property bool maximized: menu.owner !== null && menu.owner.maximized

    // The choice, run once the menu has closed.
    property string pending: ""
    function choose(what: string) {
        menu.pending = what
    }
    onClosed: {
        const what = menu.pending
        menu.pending = ""
        const manager = menu.owner ? menu.owner.manager : null
        if (what === "" || !manager)
            return
        const gid = menu.owner.dockKey
        const key = menu.panel
        Qt.callLater(() => manager.tabMenuAction(what, gid, key))
    }

    ContextMenuItem {
        objectName: "dockTabMenuClose"
        text: qsTr("Close Panel")
        iconName: "x"
        onTriggered: menu.choose("close")
    }
    ContextMenuItem {
        objectName: "dockTabMenuCloseOthers"
        text: qsTr("Close Other Panels in Group")
        iconName: "x"
        enabled: menu.several
        onTriggered: menu.choose("closeOthers")
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "dockTabMenuUndock"
        text: qsTr("Undock Panel")
        iconName: "external-link"
        // A lone panel already in a window of its own has nowhere further to go.
        enabled: menu.several || !menu.floating
        onTriggered: menu.choose("undock")
    }
    ContextMenuItem {
        objectName: "dockTabMenuUndockGroup"
        text: menu.floating ? qsTr("Dock Panel Group") : qsTr("Undock Panel Group")
        iconName: menu.floating ? "minimize-2" : "external-link"
        onTriggered: menu.choose("undockGroup")
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "dockTabMenuMaximize"
        text: menu.maximized ? qsTr("Restore Frame") : qsTr("Maximize Frame")
        iconName: menu.maximized ? "square" : "scaling"
        command: "window.maximizeFrame"
        // A floating group keeps its own window.
        enabled: !menu.floating
        onTriggered: menu.choose("maximize")
    }
}
