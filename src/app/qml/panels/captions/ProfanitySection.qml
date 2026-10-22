pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// p02 audio: Premiere's "Hide profanity" for captions (Captions.hideProfanity: captions generated or updated from the
// transcript show profane words as "f***"; "Hide now" masks the shown track's cues as they are), the editable word
// lists (extra words, allowed words: comma-separated, a trailing * matches words that start with it) and Bleep
// profanity: the same words bleeped or muted in the sound (audio redactions; the picture is untouched).
ColumnLayout {
    id: root

    spacing: Theme.space(1)

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        Toggle {
            objectName: "captionsHideProfanity"
            Layout.fillWidth: true
            text: qsTr("Hide profanity")
            tooltip: qsTr("Captions from the transcript show profane words as f***")
            checked: Captions.hideProfanity
            onToggled: Captions.hideProfanity = checked
        }
        PillButton {
            objectName: "captionsHideProfanityNow"
            compact: true
            variant: "ghost"
            text: qsTr("Hide now")
            tooltip: qsTr("Mask the profane words of this caption track as it is")
            onClicked: Captions.hideProfanityNow()
        }
        PillButton {
            id: bleep
            objectName: "captionsBleepProfanity"
            compact: true
            variant: "secondary"
            iconName: "volume-x"
            text: qsTr("Bleep")
            tooltip: qsTr("Bleep or mute the profane words in the sound")
            onClicked: bleepMenu.popup(bleep, 0, bleep.height)
            OptionsMenu {
                id: bleepMenu
                objectName: "captionsBleepMenu"
                options: [{ id: "bleep", name: qsTr("Bleep profanity") }, { id: "mute", name: qsTr("Mute profanity") }]
                onChosen: id => Captions.bleepProfanity(id)
            }
        }
    }
    FieldInput {
        objectName: "captionsProfanityWords"
        Layout.fillWidth: true
        visible: Captions.hideProfanity
        placeholderText: qsTr("More words to hide, comma-separated")
        text: Captions.profanityWords
        onEditingFinished: Captions.profanityWords = text
        Accessible.name: qsTr("More words to hide")
    }
    FieldInput {
        objectName: "captionsAllowedWords"
        Layout.fillWidth: true
        visible: Captions.hideProfanity
        placeholderText: qsTr("Words to allow, comma-separated")
        text: Captions.allowedWords
        onEditingFinished: Captions.allowedWords = text
        Accessible.name: qsTr("Words to allow")
    }
}
