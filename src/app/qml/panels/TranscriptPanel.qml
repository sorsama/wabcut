pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// ● TRANSCRIPT (panel key "captions"): edit the video by editing its text (M5e).
//   Transcript   the sequence's words by speaker and time; click to seek, select to light the timeline, Delete / Keep
//                only with a dry-run preview (struck-through words, red ghost on the timeline), drag a paragraph to
//                move it, find; or, without a transcript, transcribe the sequence or the selected clips.
//                P0 round: Sequence | Source - Source reads the clip in the Source Monitor (SourceTranscriptPage):
//                select a soundbite as words, then Insert (,) or Overwrite (.) it.
//   Clean up     fillers and silences with a keep/remove choice each, previewed before they go.
//   Captions     generate a caption track from the transcript, edit cues inline, style the monitor preview, export
//                SRT/VTT.
// Every edit is a transcript or caption command (TranscriptBridge, CaptionsBridge): one undo step.
Panel {
    id: root
    objectName: "transcriptPanel"

    title: qsTr("Transcript")

    /// P0 round: the Transcript tab shows the Source Monitor's clip (Source) instead of the sequence (Sequence).
    property bool sourceMode: false

    /// The user asked to set up a transcription provider (Main opens Preferences > AI).
    signal setupRequested()

    // Focus target for Window > Transcript.
    function focusContent() {
        tabs.forceActiveFocus(Qt.TabFocusReason)
    }
    /// 0.5 (stream A): the Captions tab showing cue `index`, its text open for typing when `edit` (the timeline's caption
    /// lane: a double-click, the cue menu's Edit Text / Select in Captions; Main calls this).
    function showCaptionCue(index: int, edit: bool) {
        tabs.currentIndex = 2
        captionsPage.showCue(index, edit)
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

            // The Transcript tab: the sequence's words, or (P0 round) the Source Monitor clip's.
            ColumnLayout {
                spacing: Theme.space(2)

                PillTabs {
                    objectName: "transcriptModeTabs"
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("Sequence"), qsTr("Source")]
                    currentIndex: root.sourceMode ? 1 : 0
                    Accessible.name: qsTr("Transcript of the sequence or of the source clip")
                    onActivated: index => {
                        Transcript.cancelPreview()
                        root.sourceMode = index === 1
                    }
                }
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.sourceMode ? 1 : 0

                    TranscriptPage {
                        onSetupRequested: root.setupRequested()
                    }
                    SourceTranscriptPage {
                        onSetupRequested: root.setupRequested()
                    }
                }
            }
            CleanupPage {}
            CaptionsPage {
                id: captionsPage
            }
        }
    }
}
