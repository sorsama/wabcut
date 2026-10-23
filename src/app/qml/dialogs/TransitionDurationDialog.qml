pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Set Duration... of a timeline transition (0.2 UX, stream T; the transition's right-click menu): the length in the
// sequence's Display Format (timecode, a frame count, or feet+frames), prefilled and selected; it reads back the same
// way (ProjectController's typed-time reader), so Enter on the untouched field keeps the length. Enter sets it on the
// selected transition (Effect Controls' transition duration, one undo step; a length the clips' handles cannot give is
// shortened to what they have, as in Effect Controls). A length Wabcut cannot read stays in the field with a note.
WabcutDialog {
    id: dialog
    objectName: "transitionDurationDialog"

    property string transitionId: ""
    /// The current sequence's Video Display Format when the dialog opened ("timecode", "frames", "feet16mm",
    /// "feet35mm"): what the note under an unreadable length suggests.
    property string displayFormat: "timecode"

    /// Opens for the transition `info` describes (ProjectController.transitionInfo; it is the selected one).
    function openFor(info: var) {
        dialog.transitionId = info.id ?? ""
        dialog.displayFormat = Editing.sequenceSettings(Editing.currentSequenceKey()).videoDisplay ?? "timecode"
        durationField.text = info.durationText ?? ""
        errorText.text = ""
        open()
        durationField.forceActiveFocus()
        durationField.selectAll()
    }
    function accept() {
        const length = ProjectController.normalizeTimecode(durationField.text)
        if (length === "") {
            errorText.text = dialog.displayFormat === "frames" ? qsTr("Type a length in frames, like 30")
                           : dialog.displayFormat.startsWith("feet") ? qsTr("Type a length like 0001+08 (feet and frames), or in frames")
                           : qsTr("Type a length like 00:00:01:00, or 2. for two seconds")
            return
        }
        if (ProjectController.selectedTransition !== dialog.transitionId) {
            errorText.text = qsTr("The transition is no longer selected")
            return
        }
        ProjectController.effectControls.setTransitionDuration(length)
        dialog.close()
    }

    title: qsTr("Transition Duration")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(96), Theme.space(96))

    FieldInput {
        id: durationField
        objectName: "transitionDurationField"
        Layout.fillWidth: true
        font.family: Theme.fontMono
        inputMethodHints: Qt.ImhPreferNumbers | Qt.ImhNoPredictiveText
        Accessible.name: qsTr("Transition duration")
        onAccepted: dialog.accept()
        onTextEdited: errorText.text = ""
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
    }
    Text {
        id: errorText
        objectName: "transitionDurationError"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "transitionDurationCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "transitionDurationOk"
            variant: "primary"
            text: qsTr("Set Duration")
            enabled: durationField.text.trim() !== ""
            onClicked: dialog.accept()
        }
    ]
}
