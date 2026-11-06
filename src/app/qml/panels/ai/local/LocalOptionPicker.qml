pragma ComponentBehavior: Bound
import QtQuick

// A labelled choice of the local model library ("Sort", "Size", "Licence", ...): a small label over a pill that opens
// a menu of `options` ([{ id, name }]) with a check on `current`. Wraps like any other item in a Flow.
Column {
    id: root

    property string label: ""
    property var options: []
    property string current: ""
    property string buttonName: ""
    /// The label goes into the pill ("Sort: Recommended") instead of above it.
    property bool inline: false
    signal chosen(string id)

    readonly property string currentName: (root.options.find(o => o.id === root.current) ?? { name: "" }).name

    spacing: Theme.space(1)

    Text {
        visible: !root.inline
        text: root.label
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Font.DemiBold
    }

    PillButton {
        id: button
        objectName: root.buttonName
        compact: true
        iconName: "chevron-down"
        text: root.inline ? qsTr("%1: %2").arg(root.label).arg(root.currentName) : root.currentName
        Accessible.name: root.label + ": " + root.currentName
        tooltip: root.label
        onClicked: menu.popup(button, 0, button.height + Theme.space(1))

        OptionsMenu {
            id: menu
            options: root.options
            current: root.current
            onChosen: id => root.chosen(id)
        }
    }
}
