pragma ComponentBehavior: Bound
import QtQuick

// A project's menu on Home (right-click on a card or row, its "..." button, the Menu key or Shift+F10): Open, Open Next
// to This One, Show in Explorer, Copy Path, Star / Unstar, Locate... (a missing project: pick the moved file) and
// Remove from List. The choice is reported after the menu has closed (`chosen`), so a dialog it opens keeps the focus.
// In its own file, importing only QtQuick, so the Wabcut ContextMenu is not shadowed by QtQuick.Controls'.
// P1 round (0.6, #49): Rename…, Duplicate…, Versions… (the recovery dashboard for this project) and Move to Recycle Bin…
// (Move to Trash… on macOS and Linux). Rename and Move to Recycle Bin grey out with the reason while the project is open here or
// in another Wabcut, or missing (Home.projectBlocker).
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
    /// Why Rename… and Move to Recycle Bin… cannot act ("" when they can), read when the menu opens.
    property string blocker: ""

    /// "open", "openAlongside", "reveal", "copyPath", "star", "locate", "remove", and (P1 round) "rename", "duplicate",
    /// "versions" or "trash".
    signal chosen(string action, var project)

    /// Shows the menu for `project` at (`x`, `y`) in `item`.
    function openFor(project: var, item: Item, x: real, y: real, isCurrent: bool) {
        menu.project = project
        menu.current = isCurrent
        menu.blocker = Home.projectBlocker(project.url)
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
    // P1 round (#49): the project's files.
    ContextMenuItem {
        objectName: "homeProjectMenuRename"
        text: qsTr("Rename…")
        iconName: "pencil"
        enabled: menu.blocker === ""
        disabledReason: menu.blocker
        onTriggered: menu.pendingAction = "rename"
    }
    ContextMenuItem {
        objectName: "homeProjectMenuDuplicate"
        text: qsTr("Duplicate…")
        iconName: "copy"
        enabled: !menu.missing
        disabledReason: qsTr("The project is missing: use Locate… to find it first")
        onTriggered: menu.pendingAction = "duplicate"
    }
    ContextMenuItem {
        objectName: "homeProjectMenuVersions"
        text: qsTr("Versions…")
        iconName: "clock"
        onTriggered: menu.pendingAction = "versions"
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
    ContextMenuItem {
        objectName: "homeProjectMenuTrash"
        text: Qt.platform.os === "windows" ? qsTr("Move to Recycle Bin…") : qsTr("Move to Trash…")
        iconName: "trash-2"
        enabled: menu.blocker === ""
        disabledReason: menu.blocker
        onTriggered: menu.pendingAction = "trash"
    }
}
