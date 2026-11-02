pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// 0.6 (P1 round, stream 1A): Premiere's Clip Mismatch Warning. The first clip that lands in an empty sequence (a Library
// drag, files from the desktop, the Source Monitor) has another frame size or frame rate than the sequence (a 1440p60
// OBS recording in a 1080p30 sequence made from a preset): the timeline asks whether the sequence should take the clip's
// settings. Change sequence settings makes the drop and the new settings one undo step (TimelineView.matchSequenceToClip);
// Keep existing settings leaves the drop as it is. "Always do this" keeps the answer in Settings > Timeline & editing
// (Preferences.clipMismatch: "change" or "keep"; "ask" asks again).
WabcutDialog {
    id: dialog
    objectName: "clipMismatchDialog"

    /// TimelineView.clipMismatch(): { media, name, mediaText, sequenceText }.
    property var mismatch: ({})
    /// ProjectController.revision right after the drop (the drop must still be the last step to join it).
    property int revision: -1
    property bool remember: false

    /// The answer: `change` (the sequence takes the clip's settings) and whether to keep it.
    signal answered(bool change, bool remember)

    function ask(info: var, rev: int) {
        dialog.mismatch = info
        dialog.revision = rev
        dialog.remember = false
        open()
        changeButton.forceActiveFocus()
    }
    function answer(change: bool) {
        close()
        dialog.answered(change, dialog.remember)
    }

    title: qsTr("Clip Mismatch Warning")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(120), Theme.space(120))

    Text {
        objectName: "clipMismatchMessage"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        //: %1 = the clip's name, %2 = its frame size and rate, %3 = the sequence's
        text: qsTr("\"%1\" (%2) does not match the sequence's settings (%3). Change the sequence to match the clip?")
                  .arg(dialog.mismatch.name ?? "").arg(dialog.mismatch.mediaText ?? "").arg(dialog.mismatch.sequenceText ?? "")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("Keeping the settings scales the clip to the frame and plays it at the sequence's frame rate.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    Toggle {
        objectName: "clipMismatchRemember"
        text: qsTr("Always do this")
        checked: dialog.remember
        onToggled: dialog.remember = checked
    }

    footerData: [
        PillButton {
            objectName: "clipMismatchKeep"
            text: qsTr("Keep existing settings")
            onClicked: dialog.answer(false)
        },
        PillButton {
            id: changeButton
            objectName: "clipMismatchChange"
            text: qsTr("Change sequence settings")
            variant: "primary"
            onClicked: dialog.answer(true)
        }
    ]
}
