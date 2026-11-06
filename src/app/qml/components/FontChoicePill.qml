pragma ComponentBehavior: Bound
import QtQuick

// A pill that shows the font in use and opens the shared FontPicker under itself (card 053): what every font choice of
// the editor is - Effect Controls' Text section, Essential Graphics, the Brand Kit, the caption styles. `family` is the
// installed family in use; `extras` are the choices that are not families (the default font, the brand kit's roles) and
// `current` marks the one in use among them (default: the family). `chosen` gets a family or an extra's id; the family is
// also put at the top of the picker's recently used fonts.
PillButton {
    id: pill

    property string family: ""
    property string current: ""
    /// Shown when nothing is chosen.
    property string placeholder: ""
    property var extras: []
    property string sampleText: ""
    property string language: ""
    property alias picker: picker

    signal chosen(string id)

    function nameOf(id: string): string {
        for (let i = 0; i < pill.extras.length; ++i) {
            if (String(pill.extras[i].id) === id)
                return pill.extras[i].name
        }
        return id
    }
    readonly property string shown: pill.current !== "" ? pill.current : pill.family

    compact: true
    iconName: "chevron-down"
    text: pill.shown !== "" ? pill.nameOf(pill.shown)
          : pill.placeholder !== "" ? pill.placeholder : qsTr("Default font")
    onClicked: picker.open()

    FontPicker {
        id: picker
        parent: pill
        x: 0
        y: pill.height + Theme.space(1)
        current: pill.shown
        extras: pill.extras
        sampleText: pill.sampleText
        language: pill.language
        onChosen: id => {
            if (id !== "" && pill.extras.every(e => String(e.id) !== id))
                Fonts.noteUsed(id)
            pill.chosen(id)
        }
    }
}
