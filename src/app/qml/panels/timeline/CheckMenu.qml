pragma ComponentBehavior: Bound
import QtQuick

// A menu of on/off options (0.1.3: the Tracks toolbar's view toggles when the panel is too narrow for their buttons):
// [{ key, name, icon, checked }]; picking one says which. In its own QtQuick-only file, so the Wabcut ContextMenu is
// not shadowed by QtQuick.Controls'.
ContextMenu {
    id: menu

    property var options: []

    signal toggled(string key)

    Instantiator {
        model: menu.options
        delegate: ContextMenuItem {
            required property var modelData
            objectName: "checkMenuItem_" + modelData.key
            text: modelData.name
            iconName: modelData.icon
            checkable: true
            checked: modelData.checked
            onTriggered: menu.toggled(modelData.key)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object as ContextMenuItem)
        onObjectRemoved: (index, object) => menu.removeItem(object as ContextMenuItem)
    }
}
