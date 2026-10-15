pragma ComponentBehavior: Bound
import QtQuick

// A template's menu in the Essential Graphics browser: add it at the playhead, or delete it from its library. Its own
// file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "graphicTemplateMenu"

    property string key: ""

    ContextMenuItem {
        objectName: "graphicTemplateAdd"
        text: qsTr("Add at playhead")
        iconName: "plus"
        onTriggered: Graphics.instantiateAtPlayhead(menu.key)
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
