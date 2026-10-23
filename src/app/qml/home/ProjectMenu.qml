pragma ComponentBehavior: Bound
import QtQuick

// A project's menu on Home (right-click on a card or row, its "..." button, the Menu key or Shift+F10): Open, Open Next
// to This One, Show in Explorer, Copy Path, Star / Unstar, Locate... (a missing project: pick the moved file) and
// Remove from List. The choice is reported after the menu has closed (`chosen`), so a dialog it opens keeps the focus.
// In its own file, importing only QtQuick, so the Wabcut ContextMenu is not shadowed by QtQuick.Controls'.
ContextMenu {
    id: menu
    objectName: "homeProjectMenu"

    /// The project (a Home.recent entry) the menu is for.
    property var project: null
    /// The project is the one open behind Home (Open Next to This One makes no sense for it).
    property bool current: false
    readonly property bool missing: menu.project !== null && menu.project.missing === true
    // Read again whenever the starred list changes.
    readonly property bool starred: menu.project !== null && Preferences.homeStarred !== ""
                                    && Preferences.isStarred(menu.project.path)
    property string pendingAction: ""

    /// "open", "openAlongside", "reveal", "copyPath", "star", "locate" or "remove".
    signal chosen(string action, var project)

    /// Shows the menu for `project` at (`x`, `y`) in `item`.
    function openFor(project: var, item: Item, x: real, y: real, isCurrent: bool) {
        menu.project = project
        menu.current = isCurrent
        menu.pendingAction = ""
        menu.popup(item, x, y)
    }

    onClosed: {
        const action = menu.pendingAction
        menu.pendingAction = ""
        if (action !== "" && menu.project !== null)
            menu.chosen(action, menu.project)
    }

    ContextMenuItem {
        objectName: "homeProjectMenuOpen"
        text: menu.current ? qsTr("Back to the Project") : qsTr("Open")
        iconName: "folder-open"
        enabled: !menu.missing
        onTriggered: menu.pendingAction = "open"
    }
    ContextMenuItem {
        objectName: "homeProjectMenuOpenAlongside"
        text: qsTr("Open Next to This One")
        iconName: "columns-3"
        enabled: !menu.missing && !menu.current
        onTriggered: menu.pendingAction = "openAlongside"
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "homeProjectMenuReveal"
        text: Qt.platform.os === "osx" ? qsTr("Reveal in Finder") : qsTr("Show in Explorer")
        iconName: "folder"
        enabled: !menu.missing
        onTriggered: menu.pendingAction = "reveal"
    }
    ContextMenuItem {
        objectName: "homeProjectMenuCopyPath"
        text: qsTr("Copy Path")
        iconName: "copy"
        onTriggered: menu.pendingAction = "copyPath"
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "homeProjectMenuStar"
        text: menu.starred ? qsTr("Unstar") : qsTr("Star")
        iconName: "bookmark"
        onTriggered: menu.pendingAction = "star"
    }
    ContextMenuItem {
        objectName: "homeProjectMenuLocate"
        visible: menu.missing
        height: visible ? implicitHeight : 0
        text: qsTr("Locate…")
        iconName: "folder-search"
        onTriggered: menu.pendingAction = "locate"
    }
    ContextMenuItem {
        objectName: "homeProjectMenuRemove"
        text: qsTr("Remove from List")
        iconName: "x"
        onTriggered: menu.pendingAction = "remove"
    }
}
