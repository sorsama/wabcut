pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// File > Podcast workflow… (AI-13): from the camera and microphone recordings to an edited, captioned, chaptered
// episode in one go, on this computer and free. Add the recordings (import files or pick them from the project), choose
// the style, then run the steps: all at once (Run all) or one by one (Preview shows a step staged, with the timeline
// ghost; Apply makes it its own undo step; Skip leaves it out). Untick a step to leave it out of the run. After every
// applied step the project is checkpointed in its version history, so Resume continues after a crash.
WabcutDialog {
    id: sheet
    objectName: "podcastWorkflowSheet"

    readonly property var steps: PodcastWorkflow.steps
    readonly property var recordings: PodcastWorkflow.recordings
    readonly property var switchPresets: ["calm", "interview", "energetic"]
    readonly property var captionPresets: ["Podcast", "Clean", "Bold Box"]
    readonly property var loudnessTargets: [-16, -14]
    /// The first step that is neither done nor skipped (Preview / Apply act on it).
    readonly property var nextStep: {
        for (const s of steps) {
            if (s.enabled && s.status !== "done" && s.status !== "skipped")
                return s
        }
        return null
    }

    title: qsTr("Podcast workflow")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(250), Theme.space(250))
    onClosed: PodcastWorkflow.discard()

    function statusText(s) {
        switch (s.status) {
        case "done": return qsTr("Done")
        case "staged": return qsTr("Preview")
        case "running": return qsTr("Running")
        case "skipped": return qsTr("Skipped")
        case "failed": return qsTr("Failed")
        case "off": return s.reason !== "" ? s.reason : qsTr("Off")
        default: return qsTr("To do")
        }
    }
    function statusColor(s) {
        switch (s.status) {
        case "done": return Theme.success
        case "staged": return Theme.accentStroke
        case "running": return Theme.accentFill
        case "failed": return Theme.danger
        case "skipped":
        case "off": return Theme.textDisabled
        default: return Theme.textTertiary
        }
    }
    function minutes(seconds) {
        const s = Math.round(Math.max(0, seconds))
        return Math.floor(s / 60) + ":" + ((s % 60) < 10 ? "0" : "") + (s % 60)
    }
    function applyOptions() {
        PodcastWorkflow.setOptions({ name: nameField.text.trim() !== "" ? nameField.text.trim() : "Podcast",
                                     switchPreset: switchPresets[styleTabs.currentIndex],
                                     captionPreset: captionPresets[captionTabs.currentIndex],
                                     lufs: loudnessTargets[loudnessTabs.currentIndex],
                                     fillers: fillersToggle.checked, silences: pausesToggle.checked ? 1.0 : 0.0 })
    }

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    FileDialog {
        id: importDialog
        title: qsTr("Import the recordings")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Video and audio (*.mov *.mp4 *.mxf *.mkv *.wav *.mp3 *.m4a *.aif *.aiff *.flac)"), qsTr("All files (*)")]
        onAccepted: {
            const error = PodcastWorkflow.importFiles(selectedFiles)
            if (error !== "")
                messageText.text = error
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(4)

        // ---- The recordings and the style ----
        ColumnLayout {
            Layout.preferredWidth: Theme.space(86)
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(1.5)
            enabled: !PodcastWorkflow.busy && !PodcastWorkflow.running

            RowLayout {
                Layout.fillWidth: true
                SectionHeader { Layout.fillWidth: true; text: qsTr("Recordings") }
                PillButton {
                    objectName: "podcastImport"
                    compact: true
                    iconName: "import"
                    text: qsTr("Import…")
                    onClicked: importDialog.open()
                }
            }
            Repeater {
                model: sheet.recordings
                delegate: Rectangle {
                    id: recording
                    required property var modelData
                    objectName: "podcastRecording_" + modelData.key
                    Layout.fillWidth: true
                    implicitHeight: recordingRow.implicitHeight + Theme.space(1.5)
                    radius: Theme.radiusField
                    color: Theme.raised
                    RowLayout {
                        id: recordingRow
                        anchors.fill: parent
                        anchors.leftMargin: Theme.space(1.5)
                        anchors.rightMargin: Theme.space(0.5)
                        spacing: Theme.space(1.5)
                        Icon {
                            name: recording.modelData.kind === "camera" ? "camera" : "audio-lines"
                            size: Theme.iconSm
                            color: Theme.textSecondary
                        }
                        Text {
                            Layout.fillWidth: true
                            text: recording.modelData.name
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            elide: Text.ElideMiddle
                        }
                        Text {
                            text: sheet.minutes(recording.modelData.seconds)
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textSm
                        }
                        PillTabs {
                            Layout.preferredWidth: Theme.space(28)
                            compact: true
                            model: [qsTr("Camera"), qsTr("Mic")]
                            currentIndex: recording.modelData.kind === "camera" ? 0 : 1
                            onActivated: index => PodcastWorkflow.addMedia(recording.modelData.key, index === 0 ? "camera" : "mic")
                        }
                        IconButton {
                            iconName: "trash-2"
                            tooltip: qsTr("Leave out")
                            onClicked: PodcastWorkflow.removeMedia(recording.modelData.key)
                        }
                    }
                }
            }
            // The project's other media, one click to add (the first twelve; the Library has them all).
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Repeater {
                    model: PodcastWorkflow.library.filter(m => !m.added).slice(0, 12)
                    delegate: PillButton {
                        required property var modelData
                        compact: true
                        iconName: "plus"
                        text: modelData.name
                        tooltip: modelData.hasVideo ? qsTr("Add as a camera") : qsTr("Add as a microphone")
                        onClicked: PodcastWorkflow.addMedia(modelData.key)
                    }
                }
            }
            Note {
                visible: sheet.recordings.length === 0
                text: qsTr("Add each camera's recording and each microphone's. They are synced by their sound; every microphone gets its own dialogue track.")
            }

            SectionHeader { Layout.fillWidth: true; text: qsTr("Style") }
            Label { text: qsTr("Episode name") }
            FieldInput {
                id: nameField
                objectName: "podcastName"
                Layout.fillWidth: true
                text: PodcastWorkflow.options.name ?? "Podcast"
                onEditingFinished: sheet.applyOptions()
            }
            Label { text: qsTr("Camera switching") }
            PillTabs {
                id: styleTabs
                objectName: "podcastSwitchStyle"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Podcast calm"), qsTr("Interview"), qsTr("Energetic")]
                onActivated: sheet.applyOptions()
            }
            Label { text: qsTr("Captions") }
            PillTabs {
                id: captionTabs
                objectName: "podcastCaptionStyle"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Podcast"), qsTr("Clean"), qsTr("Bold box")]
                onActivated: sheet.applyOptions()
            }
            Label { text: qsTr("Loudness") }
            PillTabs {
                id: loudnessTabs
                objectName: "podcastLoudness"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("−16 LUFS podcast"), qsTr("−14 LUFS YouTube")]
                onActivated: sheet.applyOptions()
            }
            Toggle { id: fillersToggle; objectName: "podcastFillers"; Layout.fillWidth: true; text: qsTr("Remove “um” and “uh”"); checked: true; onToggled: sheet.applyOptions() }
            Toggle { id: pausesToggle; objectName: "podcastPauses"; Layout.fillWidth: true; text: qsTr("Shorten pauses over 1 s"); checked: true; onToggled: sheet.applyOptions() }
        }

        // ---- The steps ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(1.5)

            RowLayout {
                Layout.fillWidth: true
                SectionHeader { Layout.fillWidth: true; text: qsTr("Steps") }
                StatusPill {
                    objectName: "podcastCost"
                    label: qsTr("On this computer")
                    detail: qsTr("free")
                    dotColor: Theme.success
                }
            }
            ListView {
                id: stepList
                objectName: "podcastSteps"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(118)
                clip: true
                spacing: Theme.space(1)
                model: sheet.steps
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar {}
                delegate: Rectangle {
                    id: stepRow
                    required property var modelData
                    readonly property bool current: sheet.nextStep !== null && sheet.nextStep.step === modelData.step
                    readonly property bool settled: modelData.status === "done" || modelData.status === "skipped"
                    objectName: "podcastStep_" + modelData.step
                    width: stepList.width
                    implicitHeight: stepColumn.implicitHeight + Theme.space(2)
                    radius: Theme.radiusCard
                    color: current ? Theme.accentWash : Theme.raised
                    border.width: current ? 1.5 : Theme.hairline
                    border.color: current ? Theme.accentStroke : Theme.border
                    opacity: modelData.status === "off" || modelData.status === "skipped" ? 0.6 : 1
                    ColumnLayout {
                        id: stepColumn
                        anchors.fill: parent
                        anchors.margins: Theme.space(1)
                        spacing: Theme.space(0.5)
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space(1.5)
                            Toggle {
                                objectName: "podcastStepToggle_" + stepRow.modelData.step
                                checked: stepRow.modelData.enabled || stepRow.modelData.status === "done"
                                enabled: !stepRow.settled && !PodcastWorkflow.running && (stepRow.modelData.enabled || stepRow.modelData.reason === "unticked"
                                                                                           || stepRow.modelData.step === "shorts")
                                Accessible.name: stepRow.modelData.title
                                onToggled: PodcastWorkflow.setStepEnabled(stepRow.modelData.step, checked)
                            }
                            Text {
                                text: stepRow.modelData.index
                                color: Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textSm
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    Layout.fillWidth: true
                                    text: stepRow.modelData.title
                                    color: Theme.textPrimary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textMd
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: stepRow.modelData.summary !== "" ? stepRow.modelData.summary : stepRow.modelData.detail
                                    color: stepRow.modelData.status === "failed" ? Theme.danger : Theme.textTertiary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textXs
                                    elide: Text.ElideRight
                                }
                            }
                            StatusPill {
                                objectName: "podcastStepStatus_" + stepRow.modelData.step
                                label: sheet.statusText(stepRow.modelData)
                                dotColor: sheet.statusColor(stepRow.modelData)
                            }
                        }
                        // One step at a time: preview, apply or skip the next one.
                        RowLayout {
                            Layout.fillWidth: true
                            visible: stepRow.current && !PodcastWorkflow.running
                            spacing: Theme.space(1)
                            Item { Layout.fillWidth: true }
                            PillButton {
                                objectName: "podcastSkip"
                                compact: true
                                iconName: "skip-forward"
                                text: qsTr("Skip")
                                enabled: !PodcastWorkflow.busy
                                onClicked: PodcastWorkflow.skipStep(stepRow.modelData.step)
                            }
                            PillButton {
                                objectName: "podcastPreviewStep"
                                compact: true
                                iconName: "eye"
                                text: qsTr("Preview")
                                enabled: !PodcastWorkflow.busy && stepRow.modelData.status !== "staged"
                                onClicked: PodcastWorkflow.previewStep(stepRow.modelData.step)
                            }
                            PillButton {
                                objectName: "podcastApplyStep"
                                compact: true
                                variant: "primary"
                                iconName: "check"
                                text: qsTr("Apply")
                                enabled: !PodcastWorkflow.busy && stepRow.modelData.status === "staged"
                                onClicked: {
                                    const error = PodcastWorkflow.applyStep()
                                    if (error !== "")
                                        messageText.text = error
                                }
                            }
                        }
                    }
                }
                EmptyState {
                    anchors.centerIn: parent
                    width: parent.width - Theme.space(4)
                    visible: stepList.count === 0
                    illustration: "transcript"
                    text: qsTr("Add the recordings: the steps from syncing to chapters appear here, each one previewable and undoable.")
                }
            }
            // The run's progress: steps done of the enabled ones, and the step at work.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                visible: sheet.steps.length > 0
                RowLayout {
                    Layout.fillWidth: true
                    StatusPill {
                        objectName: "podcastProgressPill"
                        label: PodcastWorkflow.busy ? PodcastWorkflow.phase : qsTr("%1 of %2 steps done").arg(PodcastWorkflow.doneCount).arg(PodcastWorkflow.enabledCount)
                        detail: PodcastWorkflow.busy && PodcastWorkflow.progress >= 0 ? Math.round(PodcastWorkflow.progress * 100) + "%" : ""
                        dotColor: PodcastWorkflow.busy ? Theme.accentFill : Theme.success
                    }
                    Item { Layout.fillWidth: true }
                    PillButton {
                        objectName: "podcastOpenEpisode"
                        compact: true
                        visible: PodcastWorkflow.episode !== ""
                        iconName: "film"
                        text: qsTr("Open the episode")
                        onClicked: PodcastWorkflow.openEpisode()
                    }
                }
                BeamProgress {
                    objectName: "podcastBeam"
                    Layout.fillWidth: true
                    value: PodcastWorkflow.enabledCount > 0
                           ? (PodcastWorkflow.doneCount + (PodcastWorkflow.busy && PodcastWorkflow.progress > 0 ? PodcastWorkflow.progress : 0)) / PodcastWorkflow.enabledCount
                           : 0
                    active: PodcastWorkflow.busy
                    label: PodcastWorkflow.phase
                }
            }
            Note {
                visible: PodcastWorkflow.youtubeChapters !== ""
                text: qsTr("YouTube chapters (in the description):") + "\n" + PodcastWorkflow.youtubeChapters
                font.family: Theme.fontMono
                maximumLineCount: 8
                elide: Text.ElideRight
            }
            Note {
                id: messageText
                objectName: "podcastMessage"
                visible: text !== ""
                text: PodcastWorkflow.message
                color: Theme.warning
                maximumLineCount: 5
                elide: Text.ElideRight
            }
        }
    }

    footerData: [
        PillButton {
            text: PodcastWorkflow.busy ? qsTr("Stop") : qsTr("Close")
            onClicked: PodcastWorkflow.busy ? PodcastWorkflow.cancel() : sheet.close()
        },
        PillButton {
            objectName: "podcastResume"
            visible: PodcastWorkflow.canResume
            iconName: "rotate-ccw"
            text: qsTr("Resume the last run")
            enabled: !PodcastWorkflow.busy
            onClicked: {
                const error = PodcastWorkflow.resume()
                if (error !== "")
                    messageText.text = error
            }
        },
        PillButton {
            objectName: "podcastRunAll"
            variant: "primary"
            iconName: "sparkles"
            text: PodcastWorkflow.doneCount > 0 ? qsTr("Run the rest") : qsTr("Run all")
            enabled: !PodcastWorkflow.busy && !PodcastWorkflow.running && sheet.nextStep !== null
            onClicked: {
                sheet.applyOptions()
                const error = PodcastWorkflow.runAll()
                if (error !== "")
                    messageText.text = error
            }
        }
    ]
}
