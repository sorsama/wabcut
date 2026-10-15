pragma ComponentBehavior: Bound
import QtQuick

// A menu of choices ([{ id, name, count? }]) with a check mark on `current`; children (fixed items) follow them.
// In its own file, importing only QtQuick, so the Wabcut ContextMenu is not shadowed by QtQuick.Controls'.
ContextMenu {
    id: menu

    property var options: []
    property string current: ""

    signal chosen(string id)

    Instantiator {
        model: menu.options
        delegate: ContextMenuItem {
            required property var modelData
            text: modelData.count !== undefined ? qsTr("%1 (%2)").arg(modelData.name).arg(modelData.count) : modelData.name
            checkable: true
            checked: modelData.id === menu.current
            onTriggered: menu.chosen(modelData.id)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object as ContextMenuItem)
        onObjectRemoved: (index, object) => menu.removeItem(object as ContextMenuItem)
    }
}
