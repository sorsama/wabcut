pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Delete Track on a track that has clips (0.2 UX, stream T; the track header menu): asks first, since the clips go
// with it. Linked video or audio on other tracks stays (unlinked). One undo step (ProjectController.deleteTrack);
// Ctrl+Z brings the track and its clips back.
WabcutDialog {
    id: dialog
    objectName: "deleteTrackDialog"

    property string trackId: ""
    property string shortName: ""
    property int clipCount: 0

    /// Asks about the track `info` describes (ProjectController.trackInfo).
    function openFor(info: var) {
        dialog.trackId = info.id ?? ""
        dialog.shortName = info.shortName ?? ""
        dialog.clipCount = info.clipCount ?? 0
        errorText.text = ""
        open()
        deleteButton.forceActiveFocus()
    }
    function accept() {
        if (ProjectController.deleteTrack(dialog.trackId))
            dialog.close()
        else
            errorText.text = ProjectController.lastError
    }

    title: qsTr("Delete %1?").arg(dialog.shortName)
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(110), Theme.space(110))

    Text {
        objectName: "deleteTrackMessage"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: dialog.clipCount === 1
              ? qsTr("%1 and its clip will be deleted. A linked clip on another track stays. Undo brings them back.")
                    .arg(dialog.shortName)
              : qsTr("%1 and its %2 clips will be deleted. Linked clips on other tracks stay. Undo brings them back.")
                    .arg(dialog.shortName).arg(dialog.clipCount)
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    Text {
        id: errorText
        objectName: "deleteTrackError"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "deleteTrackCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            id: deleteButton
            objectName: "deleteTrackOk"
            variant: "danger"
            iconName: "trash-2"
            text: qsTr("Delete Track")
            onClicked: dialog.accept()
            Keys.onReturnPressed: dialog.accept()
            Keys.onEnterPressed: dialog.accept()
        }
    ]
}
