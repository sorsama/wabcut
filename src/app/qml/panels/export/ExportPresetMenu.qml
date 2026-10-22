pragma ComponentBehavior: Bound
import QtQuick

// A menu of export presets (0.2: the watch folder's preset, the preset manager). QtQuick only, so ContextMenu is
// Wabcut's own (a file importing QtQuick.Controls gets Qt's).
ContextMenu {
    id: menu

    property var presets: []
    property string current: ""

    signal chosen(string id)

    Instantiator {
        model: menu.presets
        delegate: ContextMenuItem {
            required property var modelData
            text: modelData.name
            iconName: modelData.icon
            checkable: true
            checked: modelData.id === menu.current
            onTriggered: menu.chosen(modelData.id)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object as ContextMenuItem)
        onObjectRemoved: (index, object) => menu.removeItem(object as ContextMenuItem)
    }
}
