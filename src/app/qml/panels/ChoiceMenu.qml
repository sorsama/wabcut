pragma ComponentBehavior: Bound
import QtQuick

// The menu of an Effect Controls choice with many options (0.2: a transition's effect, Premiere's direction and type
// popups): one checkable item per option [{ value, label }], the current one checked. Its own file, importing only
// QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours (as TakesSubmenu).
ContextMenu {
    id: menu

    property var options: []
    property string current: ""

    signal chosen(string value)

    Instantiator {
        model: menu.options
        delegate: ContextMenuItem {
            required property var modelData
            objectName: "choiceItem_" + modelData.value
            text: modelData.label
            checkable: true
            checked: modelData.value === menu.current
            enabled: modelData.value !== "custom"
            onTriggered: menu.chosen(modelData.value)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
}
