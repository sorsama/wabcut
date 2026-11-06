pragma ComponentBehavior: Bound
import QtQuick

// The Sort menu of the Essential Graphics template browser (0.7, card #87): how the cards of every group are ordered.
// Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "graphicSortMenu"

    /// "name", "nameDesc", "length", "lengthDesc" or "fields".
    property string current: "name"
    signal chosen(string key)

    component SortItem: ContextMenuItem {
        required property string key
        objectName: "graphicSort_" + key
        iconName: menu.current === key ? "check" : ""
        onTriggered: menu.chosen(key)
    }

    SortItem {
        key: "name"
        text: qsTr("Name (A to Z)")
    }
    SortItem {
        key: "nameDesc"
        text: qsTr("Name (Z to A)")
    }
    SortItem {
        key: "length"
        text: qsTr("Length (shortest first)")
    }
    SortItem {
        key: "lengthDesc"
        text: qsTr("Length (longest first)")
    }
    SortItem {
        key: "fields"
        text: qsTr("Most fields first")
    }
}
