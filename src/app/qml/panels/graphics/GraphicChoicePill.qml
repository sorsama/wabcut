pragma ComponentBehavior: Bound
import QtQuick

// A pill that opens a menu of choices ([{ id, name }]) and shows the current one's name (the Essential Graphics panel's
// pickers: fonts, weights, blend modes, presets, pin targets).
PillButton {
    id: pill

    property var options: []
    property string current: ""
    /// Shown when no option is current.
    property string placeholder: ""

    signal chosen(string id)

    function nameOf(id) {
        for (let i = 0; i < pill.options.length; ++i) {
            if (String(pill.options[i].id) === String(id))
                return pill.options[i].name
        }
        return pill.placeholder !== "" ? pill.placeholder : String(id)
    }

    compact: true
    iconName: "chevron-down"
    text: pill.nameOf(pill.current)
    onClicked: menu.popup(pill, 0, pill.height + Theme.space(1))

    OptionsMenu {
        id: menu
        options: pill.options
        current: pill.current
        onChosen: id => pill.chosen(id)
    }
}
