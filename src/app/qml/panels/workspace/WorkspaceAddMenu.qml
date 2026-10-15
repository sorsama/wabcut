pragma ComponentBehavior: Bound
import QtQuick

// 0.1.3 workspaces: the "+" after the workspace tabs: a new workspace from a preset (Edit, Color, Audio, Captions,
// Export) or as a copy of the current one. QtQuick-only (see WorkspaceTabMenu).
ContextMenu {
    id: root
    objectName: "workspaceAddMenu"

    /// The workspace a copy is made of.
    property int current: 0
    /// A workspace was made: the tabs switch to it and rename it.
    signal created(int id)

    Instantiator {
        model: Workspaces.presetOrder
        delegate: ContextMenuItem {
            required property string modelData
            objectName: "workspaceAddPreset_" + modelData
            text: qsTr("New from the %1 preset").arg(Workspaces.presetLabel(modelData))
            iconName: "layout-grid"
            onTriggered: root.created(Workspaces.createFromPreset(modelData, ""))
        }
        onObjectAdded: (index, object) => root.insertItem(index, object as ContextMenuItem)
        onObjectRemoved: (index, object) => root.removeItem(object as ContextMenuItem)
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "workspaceAddCopy"
        text: qsTr("New as a copy of %1").arg(Workspaces.label(root.current))
        iconName: "copy"
        onTriggered: root.created(Workspaces.create(root.current, ""))
    }
}
