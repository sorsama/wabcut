pragma ComponentBehavior: Bound
import QtQuick

// The Library's tools menu (M5k, the header's ··· button): Import Image Sequence (0.2), Import Folder (0.4: a folder as a
// bin, its subfolders nested, in the open bin; the folder picker opens once the menu has closed), hover scrub, unused
// media (show, remove), duplicate media and Make Subclip; Sort by (M5n: the keyboard's way to sort, as a click on a list header does; the current column
// again reverses the order). Its own file: QtQuick.Controls' ContextMenu attached type would hide Wabcut's ContextMenu
// in a file that imports the controls.
ContextMenu {
    id: menu
    objectName: "libraryToolsMenu"

    signal removeUnusedRequested()
    signal makeSubclipRequested()
    signal importImageSequenceRequested()
    // 0.2 (P.3)
    signal newSearchBinRequested()
    signal importFromProjectRequested()
    signal productionRequested()
    // 0.4: Import folder… (the window's folder picker: Main.openImportFolder)
    signal importFolderRequested()

    // The item to run once the menu has closed ("importFolder"): a native dialog opened while the menu closes loses
    // its focus.
    property string pending: ""

    onAboutToShow: menu.pending = ""
    onClosed: {
        const action = menu.pending
        menu.pending = ""
        if (action === "importFolder")
            menu.importFolderRequested()
    }

    ContextMenuItem {
        objectName: "libraryImportImageSequence"
        text: qsTr("Import image sequence…")
        iconName: "import"
        onTriggered: menu.importImageSequenceRequested()
    }
    ContextMenuItem {
        objectName: "libraryImportFolder"
        text: qsTr("Import folder…")
        iconName: "folder-plus"
        onTriggered: menu.pending = "importFolder"
    }
    ContextMenuItem {
        objectName: "libraryImportFromProject"
        text: qsTr("Import from project…")
        iconName: "layers"
        onTriggered: menu.importFromProjectRequested()
    }
    ContextMenuItem {
        objectName: "libraryNewSearchBin"
        text: qsTr("New search bin…")
        iconName: "folder-search"
        onTriggered: menu.newSearchBinRequested()
    }
    ContextMenuItem {
        objectName: "libraryOpenProduction"
        text: qsTr("Productions and other projects")
        iconName: "folders"
        onTriggered: menu.productionRequested()
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "libraryHoverScrubItem"
        text: qsTr("Hover scrub")
        checkable: true
        checked: Library.hoverScrub
        command: "project.hoverScrub"
        onTriggered: Library.hoverScrub = !Library.hoverScrub
    }
    ContextMenuItem {
        objectName: "libraryLargerThumbnails"
        text: qsTr("Larger thumbnails")
        enabled: Library.thumbnailScale < 2
        onTriggered: Library.thumbnailScale += 0.25
    }
    ContextMenuItem {
        objectName: "librarySmallerThumbnails"
        text: qsTr("Smaller thumbnails")
        enabled: Library.thumbnailScale > 0.5
        onTriggered: Library.thumbnailScale -= 0.25
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "libraryShowUnused"
        text: qsTr("Show unused media (%1)").arg(Library.unusedCount)
        iconName: "search"
        enabled: Library.unusedCount > 0
        onTriggered: Library.filters = { unused: true }
    }
    ContextMenuItem {
        objectName: "libraryRemoveUnused"
        text: qsTr("Remove unused media…")
        iconName: "trash-2"
        enabled: Library.unusedCount > 0
        onTriggered: menu.removeUnusedRequested()
    }
    ContextMenuItem {
        objectName: "libraryShowDuplicates"
        text: qsTr("Show duplicate media (%1)").arg(Library.duplicateGroups.length)
        iconName: "copy"
        enabled: Library.duplicateGroups.length > 0
        onTriggered: Library.filters = { duplicates: true }
    }
    ContextMenuSeparator {}
    ContextMenu {
        id: sortMenu
        objectName: "librarySortMenu"
        title: qsTr("Sort by")

        Instantiator {
            model: Library.columns
            delegate: ContextMenuItem {
                id: sortItem
                required property var modelData
                readonly property bool current: Library.sortColumn === modelData.id
                objectName: "librarySortBy_" + modelData.id
                text: !sortItem.current ? modelData.title
                      : Library.sortAscending ? qsTr("%1, ascending").arg(modelData.title)
                      : qsTr("%1, descending").arg(modelData.title)
                checkable: true
                checked: sortItem.current
                onTriggered: {
                    Library.sortBy(modelData.id)
                    sortItem.checked = Qt.binding(() => sortItem.current)
                }
            }
            onObjectAdded: (index, object) => sortMenu.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => sortMenu.removeItem(object as ContextMenuItem)
        }
    }
    ContextMenuSeparator {}
    NewItemSubmenu {} // P.2: Bars and Tone, Universal Counting Leader and the other synthetic items
    ContextMenuItem {
        text: qsTr("Make subclip…")
        iconName: "scissors"
        command: "clip.makeSubclip"
        enabled: Library.canMakeSubclip
        onTriggered: menu.makeSubclipRequested()
    }
}
