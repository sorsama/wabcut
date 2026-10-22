pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Modify > Timecode (P.2, E5): Premiere's Clip > Modify > Timecode on the selected Library items - the file's timecode
// or another start timecode, and the reel name. Multicam and Merge Clips sync by it (media.setTimecode).
WabcutDialog {
    id: dialog
    objectName: "timecodeDialog"

    property var mediaKeys: []
    property var info: ({})
    property bool useFile: true
    property string errorText: ""

    function openFor(keys) {
        dialog.mediaKeys = keys
        dialog.info = Editing.mediaTimecode(keys)
        dialog.errorText = dialog.info.name === undefined ? qsTr("Select clips in the Library") : ""
        dialog.useFile = (dialog.info.timecode ?? "") === ""
        timecodeField.text = dialog.useFile ? (dialog.info.fileTimecode ?? "") || "00:00:00:00" : dialog.info.timecode
        reelField.text = dialog.info.reelName ?? ""
        open()
    }
    function apply() {
        const error = Editing.applyMediaTimecode(dialog.mediaKeys, dialog.useFile ? "" : timecodeField.text, reelField.text)
        if (error === "")
            dialog.close()
        else
            dialog.errorText = error
    }

    title: qsTr("Modify Clip: Timecode")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(130), Theme.space(130))

    Text {
        Layout.fillWidth: true
        text: dialog.info.name ?? ""
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
    }
    Toggle {
        objectName: "timecodeUseFile"
        text: (dialog.info.fileTimecode ?? "") !== "" ? qsTr("File Timecode (%1)").arg(dialog.info.fileTimecode)
                                                      : qsTr("File Timecode (the file has none)")
        checked: dialog.useFile
        onToggled: dialog.useFile = checked
    }
    RowLayout {
        spacing: Theme.space(3)
        enabled: !dialog.useFile
        Text {
            text: qsTr("Set Timecode")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        FieldInput {
            id: timecodeField
            objectName: "timecodeStart"
            Layout.preferredWidth: Theme.space(40)
            font.family: Theme.fontMono
            Accessible.name: qsTr("Start timecode")
        }
    }
    RowLayout {
        spacing: Theme.space(3)
        Text {
            text: qsTr("Reel Name")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        FieldInput {
            id: reelField
            objectName: "timecodeReel"
            Layout.fillWidth: true
            Accessible.name: qsTr("Reel name")
        }
    }
    Text {
        objectName: "timecodeError"
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
            objectName: "timecodeOk"
            variant: "primary"
            text: qsTr("OK")
            enabled: dialog.info.name !== undefined
            onClicked: dialog.apply()
        }
    ]
}
