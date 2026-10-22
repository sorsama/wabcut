pragma ComponentBehavior: Bound
import QtQuick

// A choice in the editing dialogs (P.2, E5): the chosen option's name on a pill; a click lists the options
// ([{ id, name }]) with a check mark on `current`.
PillButton {
    id: choice

    property var options: []
    property string current: ""

    signal chosen(string id)

    compact: true
    variant: "secondary"
    iconName: "chevron-down"
    LayoutMirroring.enabled: true // label first, chevron last
    LayoutMirroring.childrenInherit: true
    text: {
        for (const o of choice.options) {
            if (o.id === choice.current)
                return o.name
        }
        return choice.current
    }
    onClicked: menu.popup(choice, 0, choice.height + Theme.space(1))

    OptionsMenu {
        id: menu
        options: choice.options
        current: choice.current
        onChosen: id => choice.chosen(id)
    }
}
