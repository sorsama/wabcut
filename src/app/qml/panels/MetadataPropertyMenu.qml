pragma ComponentBehavior: Bound
import QtQuick

// The Metadata panel's "Add property" menu (0.2, P.3): the XMP properties Wabcut knows, by schema. Its own file,
// importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours (as ChoiceMenu).
ContextMenu {
    id: menu
    objectName: "metadataPropertyMenu"

    /// [{ ns, prefix, property, name, label, group }] (Library.xmpFieldChoices()).
    property var choices: []

    signal chosen(var choice)

    function openFor(target) {
        if (choices.length === 0)
            choices = Library.xmpFieldChoices()
        popup(target, 0, target.height)
    }

    Instantiator {
        model: menu.choices
        delegate: ContextMenuItem {
            required property var modelData
            objectName: "metadataProperty_" + modelData.name
            text: qsTr("%1 · %2").arg(modelData.group).arg(modelData.label)
            onTriggered: menu.chosen(modelData)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
}
