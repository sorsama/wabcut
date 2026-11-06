pragma ComponentBehavior: Bound
import QtQuick

// The LUT menu of the Color panel: none, the built-in looks or camera input LUTs, the project's loose .cube files, one
// submenu per folder of LUTs (0.7, card #85: "Import folder…" groups the files by their folders), "Import .cube…" and
// "Import folder…" (the panel opens its dialogs on `browse` and `browseFolder`). The rows after "None" are rebuilt each
// time the menu opens (rebuild()), from the project's LUTs as the Color bridge lists them.
ContextMenu {
    id: menu
    objectName: "lutMenu"

    /// "input" (Basic > Input LUT) or "look" (Creative > Look).
    property alias slot: choice.slot
    signal browse()
    signal browseFolder()

    /// The rows rebuild() put between "None" and the import rows: items and folder submenus, in order.
    readonly property int headCount: 2  // None, separator
    readonly property int tailCount: 3  // separator, Import .cube, Import folder
    /// Folder names with the number of LUTs in them, as the last rebuild() listed them (tests and the screenshots).
    property var folderNames: []

    QtObject {
        id: choice
        property string slot: "look"
    }

    Component {
        id: rowMaker
        ContextMenuItem {
            required property var entry
            objectName: "lutMenuRow_" + entry.reference
            text: entry.offline ? qsTr("%1 (offline)").arg(entry.name) : entry.name
            iconName: entry.kind === "file" ? "folder-open" : ""
            onTriggered: Color.setLut(choice.slot, entry.reference)
        }
    }
    Component {
        id: folderMaker
        ContextMenu {
            property string folder: ""
            objectName: "lutMenuFolder_" + folder
        }
    }

    function openAt(target) {
        popup(target, 0, target.height)
    }

    /// The LUTs this slot offers: Basic > Input LUT takes input LUTs and files, Creative > Look looks and files.
    function offered() {
        return Color.lutChoices.filter(c => choice.slot === "input" ? c.kind !== "look" : c.kind !== "input")
    }

    /// The submenu names for `entries`' folders, sorted, each with its entries in order.
    function groupsOf(entries) {
        const byName = {}
        for (const e of entries) {
            if (e.folder) {
                if (!byName[e.folder])
                    byName[e.folder] = []
                byName[e.folder].push(e)
            }
        }
        return Object.keys(byName).sort((a, b) => a.localeCompare(b)).map(name => ({ name: name, entries: byName[name] }))
    }

    function rebuild() {
        // Drop what the last rebuild added (taken out of the menu, then destroyed).
        while (menu.count > menu.headCount + menu.tailCount) {
            const row = menu.itemAt(menu.headCount) as ContextMenuItem
            if (!row)
                break
            if (row.subMenu) {
                const sub = menu.takeMenu(menu.headCount)
                if (sub)
                    sub.destroy()
            } else {
                const taken = menu.takeItem(menu.headCount)
                if (taken)
                    taken.destroy()
            }
        }
        const entries = menu.offered()
        let at = menu.headCount
        for (const e of entries) {
            if (!e.folder)
                menu.insertItem(at++, rowMaker.createObject(menu, { entry: e }))
        }
        const groups = menu.groupsOf(entries)
        for (const g of groups) {
            const sub = folderMaker.createObject(menu, { title: g.name, folder: g.name }) as ContextMenu
            for (const e of g.entries)
                sub.addItem(rowMaker.createObject(sub, { entry: e }))
            menu.insertMenu(at++, sub)
        }
        menu.folderNames = groups.map(g => g.name)
    }

    onAboutToShow: menu.rebuild()

    ContextMenuItem {
        objectName: "lutMenuNone"
        text: qsTr("None")
        onTriggered: Color.setLut(choice.slot, "")
    }
    ContextMenuSeparator {}
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "lutMenuImport"
        text: qsTr("Import .cube…")
        iconName: "import"
        onTriggered: menu.browse()
    }
    ContextMenuItem {
        objectName: "lutMenuImportFolder"
        text: qsTr("Import folder…")
        iconName: "folder-open"
        onTriggered: menu.browseFolder()
    }
}
