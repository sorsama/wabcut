pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// ● TRANSCRIPT (rail key "captions"): edit the video by editing its text (M5e).
//   Transcript   the sequence's words by speaker and time; click to seek, select to light the timeline, Delete / Keep
//                only with a dry-run preview (struck-through words, red ghost on the timeline), drag a paragraph to
//                move it, find; or, without a transcript, transcribe the sequence or the selected clips.
//   Clean up     fillers and silences with a keep/remove choice each, previewed before they go.
//   Captions     generate a caption track from the transcript, edit cues inline, style the monitor preview, export
//                SRT/VTT.
// Every edit is a transcript or caption command (TranscriptBridge, CaptionsBridge): one undo step.
Panel {
    id: root
    objectName: "transcriptPanel"

    title: qsTr("Transcript")

    /// The user asked to set up a transcription provider (Main opens Preferences > AI).
    signal setupRequested()

    // Focus target for the rail's shortcut.
    function focusContent() {
        tabs.forceActiveFocus(Qt.TabFocusReason)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(3)

        PillTabs {
            id: tabs
            objectName: "transcriptTabs"
            Layout.fillWidth: true
            compact: true
            model: [
                { label: qsTr("Transcript"), icon: "type" },
                { label: qsTr("Clean up"), icon: "scissors" },
                { label: qsTr("Captions"), icon: "captions" }
            ]
            Accessible.name: qsTr("Transcript view")
            onCurrentIndexChanged: Transcript.cancelPreview()
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            TranscriptPage {
                onSetupRequested: root.setupRequested()
            }
            CleanupPage {}
            CaptionsPage {}
        }
    }
}
