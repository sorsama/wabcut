pragma ComponentBehavior: Bound
import QtQuick

// 0.1.3 customizable workspaces: every workspace of the title bar in a list, for when their tabs no longer fit (the
// chevron after them). QtQuick-only (see WorkspaceTabMenu).
ContextMenu {
    id: root
    objectName: "workspaceListMenu"

    property int current: 0
    signal chosen(int id)

    Instantiator {
        model: Workspaces.shown
        delegate: ContextMenuItem {
            required property var modelData
            objectName: "workspaceListItem_" + Workspaces.keyOf(modelData.id)
            text: Workspaces.label(modelData.id)
            checkable: true
            checked: modelData.id === root.current
            command: Workspaces.commandOf(modelData.id)
            onTriggered: root.chosen(modelData.id)
        }
        onObjectAdded: (index, object) => root.insertItem(index, object as ContextMenuItem)
        onObjectRemoved: (index, object) => root.removeItem(object as ContextMenuItem)
    }
}
