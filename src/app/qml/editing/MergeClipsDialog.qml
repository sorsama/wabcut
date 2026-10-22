pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Merge Clips (P.2, E5): Premiere's Clip > Merge Clips on the selected Library items - a video clip and separately
// recorded audio, synced by In points, Out points, timecode, clip markers or the audio waveforms, optionally without
// the camera's own sound. The merged clip appears in the Library and edits like any clip (media.mergeClips).
WabcutDialog {
    id: dialog
    objectName: "mergeClipsDialog"

    property var mediaKeys: []
    property string sync: "inPoints"
    property bool removeCameraAudio: false
    property string errorText: ""
    readonly property var syncOptions: [
        { id: "inPoints", name: qsTr("In Point") }, { id: "outPoints", name: qsTr("Out Point") },
        { id: "timecode", name: qsTr("Timecode") }, { id: "markers", name: qsTr("Clip Marker") },
        { id: "audio", name: qsTr("Audio") }
    ]

    function openFor(keys) {
        dialog.mediaKeys = keys
        dialog.errorText = ""
        dialog.sync = "inPoints"
        dialog.removeCameraAudio = false
        nameField.text = ""
        open()
    }
    function apply() {
        const error = Editing.mergeClips(dialog.mediaKeys, dialog.sync, dialog.removeCameraAudio, nameField.text)
        if (error === "")
            dialog.close()
        else
            dialog.errorText = error
    }

    title: qsTr("Merge Clips")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(140), Theme.space(140))

    Text {
        Layout.fillWidth: true
        text: qsTr("%n clip(s) selected: the first with video is the camera, the rest the recorded sound.", "",
                   dialog.mediaKeys.length)
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        Text {
            text: qsTr("Synchronize point")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        EditingChoice {
            objectName: "mergeClipsSync"
            options: dialog.syncOptions
            current: dialog.sync
            onChosen: id => dialog.sync = id
        }
    }
    FieldInput {
        id: nameField
        objectName: "mergeClipsName"
        Layout.fillWidth: true
        placeholderText: qsTr("Merged clip name (default: \"<video> - Merged\")")
        Accessible.name: qsTr("Merged clip name")
    }
    Toggle {
        objectName: "mergeClipsRemoveCameraAudio"
        text: qsTr("Remove audio from the video clip")
        checked: dialog.removeCameraAudio
        onToggled: dialog.removeCameraAudio = checked
    }
    Text {
        objectName: "mergeClipsError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.errorText
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "mergeClipsOk"
            variant: "primary"
            text: qsTr("OK")
            enabled: dialog.mediaKeys.length >= 2
            onClicked: dialog.apply()
        }
    ]
}
