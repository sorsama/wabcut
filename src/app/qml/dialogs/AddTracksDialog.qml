pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Add Tracks... (0.2 UX, stream T; Premiere's Sequence > Add Tracks dialog, opened from a track header's menu): how many
// video and audio tracks to add, where each kind goes - before the first track, or after any track of that kind
// (Premiere's numbering: a video track "after V1" sits above it, an audio track "after A1" below it) - and the new audio
// tracks' channel format. One undo step (ProjectController.addTracks); the header's own track is the default place
// for its kind, the end of the stack for the other. It opens with that kind's count focused and selected (typing
// replaces it); Enter in a count field adds.
WabcutDialog {
    id: dialog
    objectName: "addTracksDialog"

    /// The track whose menu opened the dialog ("" = none: both kinds go after the last track).
    property string fromTrack: ""
    property string fromKind: "video"
    /// [{ id: "first" | "after:<track key>", name }] for each kind (V1 / A1 first), read when the dialog opens.
    property var videoPlaces: []
    property var audioPlaces: []
    property string videoPlace: ""
    property string audioPlace: ""
    readonly property var formats: ["stereo", "mono", "5.1", "adaptive"]

    // The sequence's tracks (ProjectController.tracks, display order: top video first, then A1 down).
    component TrackRow: QtObject {
        required property string trackId
        required property string shortName
        required property string kind
    }
    Instantiator {
        id: trackRows
        model: ProjectController.tracks
        delegate: TrackRow {}
    }

    /// Opens the dialog for the header of `trackId` (its kind gets one new track after it by default).
    function openFor(trackId: string) {
        const info = ProjectController.trackInfo(trackId)
        dialog.fromTrack = info.id !== undefined ? trackId : ""
        dialog.fromKind = info.kind ?? "video"
        const video = []
        const audio = []
        for (let i = 0; i < trackRows.count; ++i) {
            const row = trackRows.objectAt(i) as TrackRow
            if (!row)
                continue
            // Premiere's numbering: a video track after V1 sits above it, an audio track after A1 below it.
            if (row.kind === "video")
                video.unshift({ id: "after:" + row.trackId, name: qsTr("After %1 (above it)").arg(row.shortName) })
            else
                audio.push({ id: "after:" + row.trackId, name: qsTr("After %1 (below it)").arg(row.shortName) })
        }
        dialog.videoPlaces = [{ id: "first", name: qsTr("Before the first track (a new V1)") }].concat(video)
        dialog.audioPlaces = [{ id: "first", name: qsTr("Before the first track (a new A1)") }].concat(audio)
        const lastVideo = video.length > 0 ? video[video.length - 1].id : "first"
        const lastAudio = audio.length > 0 ? audio[audio.length - 1].id : "first"
        dialog.videoPlace = dialog.fromKind === "video" && dialog.fromTrack !== "" ? "after:" + dialog.fromTrack : lastVideo
        dialog.audioPlace = dialog.fromKind === "audio" && dialog.fromTrack !== "" ? "after:" + dialog.fromTrack : lastAudio
        videoCount.value = dialog.fromKind === "video" ? 1 : 0
        audioCount.value = dialog.fromKind === "audio" ? 1 : 0
        formatTabs.currentIndex = 0
        errorText.text = ""
        open()
        // The header's kind gets the focus with its count selected: typing replaces it (3 is three tracks, not 13).
        const field = dialog.fromKind === "audio" ? audioCount : videoCount
        field.forceActiveFocus()
        field.selectAll()
    }
    /// Enter in a count field: the typed count first (the field commits it only after Enter's accepted signal), then
    /// Add.
    function acceptFrom(field: NumberField) {
        const typed = Number.parseFloat(field.text.replace(",", "."))
        if (!Number.isNaN(typed))
            field.value = field.clampValue(typed)
        dialog.accept()
    }
    /// The addTracks spec the dialog describes: each kind "first" or "after" its own reference track.
    function spec(): var {
        const out = { video: Math.round(videoCount.value), audio: Math.round(audioCount.value),
                      audioFormat: dialog.formats[formatTabs.currentIndex] }
        if (dialog.videoPlace === "first") {
            out.videoPlacement = "first"
        } else {
            out.videoPlacement = "after"
            out.videoTrack = dialog.videoPlace.substring(6)
        }
        if (dialog.audioPlace === "first") {
            out.audioPlacement = "first"
        } else {
            out.audioPlacement = "after"
            out.audioTrack = dialog.audioPlace.substring(6)
        }
        return out
    }
    function accept() {
        const request = dialog.spec()
        if (request.video + request.audio === 0) {
            dialog.close()
            return
        }
        if (ProjectController.addTracks(request).length > 0)
            dialog.close()
        else
            errorText.text = ProjectController.lastError !== "" ? ProjectController.lastError : qsTr("No tracks were added")
    }

    title: qsTr("Add Tracks")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(130), Theme.space(130))

    component RowLabel: Text {
        Layout.preferredWidth: Theme.space(26)
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        font.weight: Font.DemiBold
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 3
        columnSpacing: Theme.space(3)
        rowSpacing: Theme.space(3)

        // Video tracks: how many, and where.
        RowLabel {
            text: qsTr("Video tracks")
        }
        NumberField {
            id: videoCount
            objectName: "addTracksVideoCount"
            Layout.preferredWidth: Theme.numberFieldWidth
            from: 0
            to: 99
            Accessible.name: qsTr("Video tracks to add")
            onValueModified: v => videoCount.value = v
            onAccepted: dialog.acceptFrom(videoCount)
        }
        EditingChoice {
            objectName: "addTracksVideoPlacement"
            Layout.alignment: Qt.AlignLeft
            enabled: videoCount.value > 0
            options: dialog.videoPlaces
            current: dialog.videoPlace
            tooltip: qsTr("Where the video tracks go")
            Accessible.name: qsTr("Video track placement: %1").arg(text)
            onChosen: id => dialog.videoPlace = id
        }

        // Audio tracks: how many, where, and their channel format.
        RowLabel {
            text: qsTr("Audio tracks")
        }
        NumberField {
            id: audioCount
            objectName: "addTracksAudioCount"
            Layout.preferredWidth: Theme.numberFieldWidth
            from: 0
            to: 99
            Accessible.name: qsTr("Audio tracks to add")
            onValueModified: v => audioCount.value = v
            onAccepted: dialog.acceptFrom(audioCount)
        }
        EditingChoice {
            objectName: "addTracksAudioPlacement"
            Layout.alignment: Qt.AlignLeft
            enabled: audioCount.value > 0
            options: dialog.audioPlaces
            current: dialog.audioPlace
            tooltip: qsTr("Where the audio tracks go")
            Accessible.name: qsTr("Audio track placement: %1").arg(text)
            onChosen: id => dialog.audioPlace = id
        }

        RowLabel {
            text: qsTr("Audio format")
        }
        PillTabs {
            id: formatTabs
            objectName: "addTracksAudioFormat"
            Layout.columnSpan: 2
            Layout.fillWidth: true
            compact: true
            enabled: audioCount.value > 0
            model: [qsTr("Stereo"), qsTr("Mono"), qsTr("5.1"), qsTr("Adaptive")]
            Accessible.name: qsTr("Channel format of the new audio tracks")
        }
    }
    Text {
        id: errorText
        objectName: "addTracksError"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "addTracksCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "addTracksOk"
            variant: "primary"
            text: qsTr("Add")
            enabled: videoCount.value + audioCount.value > 0
            onClicked: dialog.accept()
        }
    ]
}
