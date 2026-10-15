pragma ComponentBehavior: Bound
import QtQuick

// The flyout of a timeline tool group (0.1.3, Premiere's tool flyouts): the group's tools with their icons and keys,
// the current one checked; choosing one makes it the panel's tool and the group's face. In its own QtQuick-only file,
// so the Wabcut ContextMenu is not shadowed by QtQuick.Controls'.
ContextMenu {
    id: menu

    /// The tools listed: [{ key, icon, name, command }].
    property var tools: []
    property string current: ""

    signal chosen(string key)

    Instantiator {
        model: menu.tools
        delegate: ContextMenuItem {
            required property var modelData
            objectName: "toolFlyoutItem_" + modelData.key
            text: modelData.name
            iconName: modelData.icon
            command: modelData.command
            checkable: true
            checked: modelData.key === menu.current
            onTriggered: menu.chosen(modelData.key)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object as ContextMenuItem)
        onObjectRemoved: (index, object) => menu.removeItem(object as ContextMenuItem)
    }
}
