pragma ComponentBehavior: Bound
import QtQuick

// A template's menu in the Essential Graphics browser: add it at the playhead, export it to a template file (built-in
// ones too), or delete it from its library. Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does
// not shadow ours.
ContextMenu {
    id: menu
    objectName: "graphicTemplateMenu"

    property string key: ""
    /// P1 (#50): the template whose export dialog opens once this menu has closed (a native dialog cannot open over it).
    property string pendingExport: ""
    /// 0.7 (card #87): the template the Move to Folder name box opens for once this menu has closed.
    property string pendingMove: ""
    /// Move to Folder...: the browser asks for the folder's name.
    signal moveRequested(string key)

    /// The Export template row: remembered, then run by onClosed (a native file dialog cannot open over a menu).
    function requestExport() {
        menu.pendingExport = menu.key
    }
    /// Runs the export the Export template row asked for, once the menu is closed.
    function finishPendingExport() {
        if (menu.pendingExport === "")
            return
        const key = menu.pendingExport
        menu.pendingExport = ""
        Graphics.openExportDialog(key)
    }
    onClosed: {
        menu.finishPendingExport()
        if (menu.pendingMove !== "") {
            const key = menu.pendingMove
            menu.pendingMove = ""
            menu.moveRequested(key)
        }
    }

    ContextMenuItem {
        objectName: "graphicTemplateAdd"
        text: qsTr("Add at playhead")
        iconName: "plus"
        onTriggered: Graphics.instantiateAtPlayhead(menu.key)
    }
    ContextMenuSeparator {}
    // 0.7 (card #87): file it under a folder of the browser (not the built-in ones: they ship in their own groups).
    ContextMenuItem {
        objectName: "graphicTemplateFolder"
        visible: !menu.key.startsWith("builtin/")
        height: visible ? implicitHeight : 0
        text: qsTr("Move to Folder…")
        iconName: "folder"
        onTriggered: menu.pendingMove = menu.key
    }
    // P1 round (#50): any template, built-in included, to a .wabcut-template.json another Wabcut imports.
    ContextMenuItem {
        objectName: "graphicTemplateExport"
        text: qsTr("Export template…")
        iconName: "download"
        onTriggered: menu.requestExport()
    }
    // The built-in callouts ship with the app: nothing to delete.
    ContextMenuItem {
        objectName: "graphicTemplateDelete"
        visible: !menu.key.startsWith("builtin/")
        height: visible ? implicitHeight : 0
        text: qsTr("Delete template")
        iconName: "trash-2"
        onTriggered: Graphics.removeTemplate(menu.key)
    }
}
