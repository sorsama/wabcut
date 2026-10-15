pragma ComponentBehavior: Bound
import QtQuick
import QtQml.Models

// The LUT menu of the Color panel: none, the built-in looks or camera input LUTs, the project's .cube files, and
// "Import .cube…" (the panel opens its file dialog on `browse`).
ContextMenu {
    id: menu

    /// "input" (Basic > Input LUT) or "look" (Creative > Look).
    property alias slot: choice.slot
    signal browse()

    QtObject {
        id: choice
        property string slot: "look"
    }

    function openAt(target) {
        popup(target, 0, target.height)
    }

    ContextMenuItem {
        text: qsTr("None")
        onTriggered: Color.setLut(choice.slot, "")
    }
    ContextMenuSeparator {}
    Instantiator {
        model: Color.lutChoices.filter(c => choice.slot === "input" ? c.kind !== "look" : c.kind !== "input")
        delegate: ContextMenuItem {
            required property var modelData
            text: modelData.offline ? qsTr("%1 (offline)").arg(modelData.name) : modelData.name
            iconName: modelData.kind === "file" ? "folder-open" : ""
            onTriggered: Color.setLut(choice.slot, modelData.reference)
        }
        onObjectAdded: (index, object) => menu.insertItem(index + 2, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        text: qsTr("Import .cube…")
        iconName: "import"
        onTriggered: menu.browse()
    }
}
