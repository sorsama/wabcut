pragma ComponentBehavior: Bound
import QtQuick

// The Production panel's menu on a project (0.2, P.3): open it (next to the projects open), browse it read-only, import
// all of it. Its own file: QtQuick.Controls' ContextMenu would hide Wabcut's in a file that imports the controls.
ContextMenu {
    id: menu
    objectName: "productionProjectMenu"

    property var entry: ({})

    signal openRequested(url file)

    function openFor(target, projectEntry, point) {
        entry = projectEntry
        popup(target, point.x, point.y)
    }

    ContextMenuItem {
        objectName: "productionMenuEdit"
        text: qsTr("Open and edit")
        iconName: "film"
        enabled: !(menu.entry.current ?? false)
        onTriggered: menu.openRequested(menu.entry.url)
    }
    ContextMenuItem {
        objectName: "productionMenuBrowse"
        text: qsTr("Browse read-only")
        iconName: "layers"
        enabled: !(menu.entry.current ?? false)
        onTriggered: Production.browse(menu.entry.url)
    }
    ContextMenuItem {
        objectName: "productionMenuImportAll"
        text: qsTr("Import into this project")
        iconName: "import"
        enabled: !(menu.entry.current ?? false)
        onTriggered: Production.importProject(menu.entry.url)
    }
}
