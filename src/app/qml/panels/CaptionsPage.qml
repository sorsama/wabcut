pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import QtQuick.Dialogs

// The Captions tab (M5e; the C.7 caption workshop in M5m): the caption tracks of the sequence (one shown and edited,
// drawn on the program monitor), captions from the transcript (with its speakers; "Update from transcript" after a
// correction on the Transcript tab rewrites the cues' text and keeps their timing), Translate and Dub (AI-12), and the
// cues: text, timing (typed or a frame at a time), split, join, speaker, reading speed. Export as SRT or WebVTT. The
// look lives in the track (the Caption Style panel beside the monitor in the Captions workspace). Every edit is one
// undo step.
Item {
    id: root
    objectName: "captionsPage"

    readonly property bool hasTrack: Captions.track !== ""
    property int charsPerLine: 42
    property int lines: 2

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        // The caption tracks: the one shown, a new one from a preset.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            PillButton {
                id: trackPill
                objectName: "captionsTrackMenu"
                Layout.fillWidth: true
                compact: true
                iconName: "chevron-down"
                text: root.hasTrack ? (Captions.trackInfo.name ?? "") + ((Captions.trackInfo.language ?? "") !== ""
                                                                       ? " · " + Captions.trackInfo.language.toUpperCase() : "")
                                    : qsTr("No caption track")
                tooltip: qsTr("The caption track shown and edited")
                onClicked: trackMenu.popup(trackPill, 0, trackPill.height + Theme.space(1))
                // OptionsMenu, not ContextMenu: this file imports QtQuick.Controls, whose ContextMenu would shadow ours.
                OptionsMenu {
                    id: trackMenu
                    options: Captions.tracks.map(t => ({
                        id: t.id,
                        name: t.language !== "" ? qsTr("%1 · %2").arg(t.name).arg(t.language.toUpperCase()) : t.name
                    }))
                    current: Captions.track
                    onChosen: id => Captions.track = id
                    ContextMenuSeparator {}
                    ContextMenuItem {
                        objectName: "captionsNewTrack"
                        text: qsTr("New caption track")
                        iconName: "plus"
                        onTriggered: Captions.addTrack(qsTr("Captions"), "")
                    }
                }
            }
            IconButton {
                objectName: "captionsDeleteTrack"
                compact: true
                visible: root.hasTrack
                iconName: "trash-2"
                tooltip: qsTr("Delete the caption track")
                onClicked: Captions.removeTrack()
            }
        }

        // Captions from the transcript.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            PillButton {
                objectName: "captionsGenerate"
                Layout.fillWidth: true
                variant: root.hasTrack ? "secondary" : "primary"
                iconName: "captions"
                text: root.hasTrack ? qsTr("Regenerate") : qsTr("Generate captions")
                enabled: Captions.canGenerate
                tooltip: Captions.canGenerate ? qsTr("Captions from the transcript, with its speakers") : qsTr("Transcribe the sequence first")
                onClicked: Captions.generate(root.charsPerLine, root.lines)
            }
            NumberField {
                Layout.preferredWidth: Theme.numberFieldWidth
                from: 8
                to: 200
                value: root.charsPerLine
                suffix: qsTr("ch")
                onValueModified: v => root.charsPerLine = Math.round(v)
                Accessible.name: qsTr("Characters per line")
            }
            PillButton {
                compact: true
                text: root.lines === 1 ? qsTr("1 line") : qsTr("2 lines")
                tooltip: qsTr("Lines per caption")
                onClicked: root.lines = root.lines === 1 ? 2 : 1
            }
        }
        PillButton {
            objectName: "captionsUpdateFromTranscript"
            Layout.fillWidth: true
            visible: root.hasTrack
            compact: true
            variant: Captions.updateSuggested ? "primary" : "secondary"
            iconName: "rotate-ccw"
            text: qsTr("Update from transcript")
            enabled: Captions.canGenerate
            tooltip: qsTr("Rewrite the captions' text from the corrected transcript; their timing stays")
            onClicked: Captions.updateFromTranscript()
        }
        // After a correction on the Transcript tab.
        Rectangle {
            objectName: "captionsUpdateBanner"
            Layout.fillWidth: true
            visible: root.hasTrack && Captions.updateSuggested
            implicitHeight: bannerText.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: Theme.accentWash
            border.width: Theme.hairline
            border.color: Theme.accentStroke
            Text {
                id: bannerText
                anchors.fill: parent
                anchors.margins: Theme.space(1.5)
                wrapMode: Text.Wrap
                text: qsTr("The transcript was corrected. Update from transcript to bring the fixes into these captions.")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        // Going global (AI-12): new caption tracks in other languages, and a voice-over in one of them.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            visible: root.hasTrack
            PillButton {
                objectName: "captionsTranslate"
                Layout.fillWidth: true
                compact: true
                iconName: "type"
                text: qsTr("Translate…")
                tooltip: qsTr("New caption tracks in other languages, timed to each language's reading speed")
                onClicked: TranslateCaptions.open()
            }
            PillButton {
                objectName: "captionsDub"
                Layout.fillWidth: true
                compact: true
                iconName: "audio-lines"
                text: qsTr("Dub…")
                tooltip: qsTr("An AI voice-over of a caption track with your voice provider")
                onClicked: Dub.open("dub")
            }
        }

        EmptyState {
            objectName: "captionsEmptyState"
            Layout.fillWidth: true
            Layout.topMargin: Theme.space(4)
            visible: !root.hasTrack
            illustration: "transcript"
            text: Captions.canGenerate ? qsTr("Generate a caption track from the transcript, or start an empty one, then edit its cues here.")
                                       : qsTr("Captions come from the transcript: transcribe the sequence first, or start an empty caption track.")
        }

        // Cues
        RowLayout {
            Layout.fillWidth: true
            visible: root.hasTrack
            GroupLabel {
                Layout.fillWidth: true
                text: qsTr("%n caption(s)", "", Captions.cues.length)
            }
            PillButton {
                objectName: "captionsAddCue"
                compact: true
                variant: "ghost"
                iconName: "plus"
                text: qsTr("At playhead")
                tooltip: qsTr("A new caption at the playhead")
                onClicked: Captions.addCueAtPlayhead("")
            }
        }
        ListView {
            id: cues
            objectName: "captionCues"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.hasTrack
            clip: true
            model: Captions.cues
            spacing: Theme.space(1)
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: false
            ScrollBar.vertical: ThinScrollBar {}
            currentIndex: Captions.activeCue
            highlightFollowsCurrentItem: false
            onCurrentIndexChanged: {
                if (currentIndex >= 0)
                    positionViewAtIndex(currentIndex, ListView.Contain)
            }

            delegate: CaptionCueRow {
                width: ListView.view ? ListView.view.width - Theme.space(3) : 0
                last: index === Captions.cues.length - 1
            }
        }

        // Export
        RowLayout {
            Layout.fillWidth: true
            visible: root.hasTrack
            spacing: Theme.space(1)
            Text {
                Layout.fillWidth: true
                text: Captions.message
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                objectName: "captionsExportSrt"
                compact: true
                iconName: "upload"
                text: qsTr("SRT")
                tooltip: qsTr("Export the captions as SubRip (.srt)")
                onClicked: {
                    saveDialog.format = "srt"
                    saveDialog.open()
                }
            }
            PillButton {
                objectName: "captionsExportVtt"
                compact: true
                iconName: "upload"
                text: qsTr("VTT")
                tooltip: qsTr("Export the captions as WebVTT (.vtt)")
                onClicked: {
                    saveDialog.format = "vtt"
                    saveDialog.open()
                }
            }
        }
    }

    FileDialog {
        id: saveDialog
        property string format: "srt"
        fileMode: FileDialog.SaveFile
        defaultSuffix: format
        nameFilters: format === "vtt" ? [qsTr("WebVTT captions (*.vtt)")] : [qsTr("SubRip captions (*.srt)")]
        onAccepted: Captions.exportFile(selectedFile, format)
    }
}
