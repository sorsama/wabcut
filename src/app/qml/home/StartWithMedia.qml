pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// "Start with media" on the New project page (the all-in-one Import with media files; design language v3): the files
// (name, size, a remove x), the first video read by GamingStart.inspect() off the GUI thread (its size, rate, length and
// codec, with chips: 4K, high fps, long -> proxies, VFR, HDR, and one line on what each means), "Add the clips to the
// sequence", and - when the first video has more than one audio stream (a game recording: game, mic, voice chat) - the
// audio streams table: include, the track name (Game / Mic / Discord suggested), the channel layout and the track it
// lands on. `tracks()` is the plan Home.createFromMedia takes. The objectNames of the old gaming card are kept.
HomeCard {
    id: card
    objectName: "homeStartWithMedia"

    /// [{ url, name, sizeText, kind ("video" | "audio" | "image" | "file" | "captions" | "lottie" | "layered") }]
    property var files: []
    /// The first video file (GamingStart reads it); "" when there is none.
    property url firstVideo: ""
    property bool addClips: true
    readonly property var recording: GamingStart.recording
    readonly property bool loaded: card.recording.ok === true && !card.inspecting && card.firstVideo.toString() !== ""
    readonly property bool inspecting: GamingStart.inspecting
    /// The streams table shows: the first video has several audio streams.
    readonly property bool showStreams: card.loaded && streams.count > 1
    /// Bumped when a stream is included or left out: the rows' target tracks follow it.
    property int includeRevision: 0
    readonly property int includedCount: {
        card.includeRevision
        let n = 0
        for (let i = 0; i < streams.count; ++i) {
            if (streams.get(i).include)
                ++n
        }
        return n
    }

    signal removeRequested(int index)
    signal addClipsToggled(bool on)
    signal createRequested()

    /// The audio plan for Home.createFromMedia: [{ index, name, include }].
    function tracks(): var {
        const list = []
        for (let i = 0; i < streams.count; ++i) {
            const row = streams.get(i)
            list.push({ index: row.stream, name: row.name.trim() !== "" ? row.name.trim() : row.suggested, include: row.include })
        }
        return list
    }
    /// The sequence track a stream lands on ("A2"), or "" when it is left out.
    function targetOf(row: int): string {
        if (row < 0 || row >= streams.count || !streams.get(row).include)
            return ""
        let n = 0
        for (let i = 0; i <= row; ++i) {
            if (streams.get(i).include)
                ++n
        }
        return "A" + n
    }
    function load() {
        ++card.includeRevision
        streams.clear()
        if (!card.loaded)
            return
        const audio = card.recording.audio ?? []
        for (const a of audio)
            streams.append({ stream: a.index, name: a.suggested, suggested: a.suggested, include: true, layout: a.layout })
    }
    function iconFor(kind: string): string {
        return kind === "video" ? "film" : kind === "audio" ? "music" : kind === "image" ? "image"
             : kind === "captions" ? "captions" : kind === "lottie" ? "sparkles" : kind === "layered" ? "layers" : "file-down"
    }

    title: qsTr("Start with media")
    hue: "pink"
    trailing: [
        Text {
            objectName: "homeMediaCount"
            text: qsTr("%n file(s)", "", card.files.length)
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    ]

    Component.onCompleted: card.load()
    Connections {
        target: GamingStart
        function onRecordingChanged() {
            card.load()
        }
    }
    ListModel {
        id: streams
    }

    // ---- The files ----
    ListView {
        id: fileList
        objectName: "homeMediaFiles"
        Layout.fillWidth: true
        readonly property real rowHeight: Theme.space(11)
        implicitHeight: Math.min(count, 5) * rowHeight + Math.max(0, Math.min(count, 5) - 1) * spacing
        model: card.files
        spacing: Theme.space(0.5)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height
        ScrollBar.vertical: ThinScrollBar {}

        delegate: Rectangle {
            id: fileRow
            required property var modelData
            required property int index
            readonly property bool isFirstVideo: card.firstVideo.toString() !== "" && fileRow.modelData.url.toString() === card.firstVideo.toString()
            objectName: "homeMediaFile_" + index
            width: fileList.width - (fileList.interactive ? Theme.space(3) : 0)
            height: fileList.rowHeight
            radius: height / 2
            color: Theme.raised

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space(3)
                anchors.rightMargin: Theme.space(1)
                spacing: Theme.space(2.5)
                Icon {
                    name: card.iconFor(fileRow.modelData.kind ?? "file")
                    size: Theme.iconSm
                    color: fileRow.isFirstVideo ? Theme.accentStroke : Theme.textSecondary
                }
                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: Theme.space(20)
                    text: fileRow.modelData.name ?? ""
                    elide: Text.ElideMiddle
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Theme.weightLabel
                }
                Text {
                    objectName: fileRow.isFirstVideo ? "gamingSummary" : ""
                    Layout.maximumWidth: fileRow.width * 0.55
                    text: {
                        if (!fileRow.isFirstVideo || !card.loaded)
                            return fileRow.isFirstVideo && card.inspecting ? qsTr("Reading…") : (fileRow.modelData.sizeText ?? "")
                        const r = card.recording
                        const parts = [r.sizeText, qsTr("%1 fps").arg(r.fps.text), r.durationText, r.videoCodec]
                        if (r.fileSizeText !== "")
                            parts.push(r.fileSizeText)
                        return parts.join(" · ")
                    }
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
                IconButton {
                    objectName: "homeMediaRemove_" + fileRow.index
                    compact: true
                    iconName: "x"
                    iconSize: Theme.iconXs
                    tooltip: qsTr("Leave out %1").arg(fileRow.modelData.name ?? "")
                    onClicked: card.removeRequested(fileRow.index)
                }
            }
        }
    }

    // Reading the first video, or why it could not be read.
    BeamProgress {
        objectName: "gamingInspectProgress"
        Layout.fillWidth: true
        visible: card.inspecting
        indeterminate: true
        active: card.inspecting
        loader: "build"
        label: qsTr("Reading the video")
    }
    Text {
        objectName: "gamingInspectError"
        Layout.fillWidth: true
        visible: !card.loaded && !card.inspecting && card.firstVideo.toString() !== "" && (card.recording.error ?? "") !== ""
        text: card.recording.error ?? ""
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    // Chips (4K, high fps, long, VFR, HDR) and what they mean for the edit.
    Flow {
        objectName: "gamingBadges"
        Layout.fillWidth: true
        spacing: Theme.space(1)
        visible: badgeRepeater.count > 0

        Repeater {
            id: badgeRepeater
            model: {
                const r = card.recording
                if (!card.loaded)
                    return []
                const list = []
                if (r.is4k)
                    list.push({ id: "4k", text: qsTr("4K"), tone: Theme.accentStroke })
                if (r.highFps)
                    list.push({ id: "highFps", text: qsTr("High fps"), tone: Theme.accentStroke })
                if (r.isLong)
                    list.push({ id: "long", text: qsTr("Long · proxies"), tone: Theme.warning })
                if (r.vfr)
                    list.push({ id: "vfr", text: qsTr("VFR"), tone: Theme.textSecondary })
                if (r.hdr !== "")
                    list.push({ id: "hdr", text: qsTr("HDR · tone-mapped"), tone: Theme.info })
                return list
            }
            delegate: Rectangle {
                id: badge
                required property var modelData
                objectName: "gamingBadge_" + modelData.id
                implicitWidth: badgeText.implicitWidth + Theme.space(3)
                implicitHeight: badgeText.implicitHeight + Theme.space(1)
                radius: Theme.radiusPill
                color: Qt.alpha(modelData.tone, 0.18)
                Accessible.role: Accessible.StaticText
                Accessible.name: modelData.text

                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: badge.modelData.text
                    color: Theme.dark ? badge.modelData.tone : Theme.textPrimary
                    font.family: Theme.fontSans
                    font.capitalization: Font.AllUppercase
                    font.pixelSize: Theme.textMicro
                    font.letterSpacing: Theme.microTracking
                    font.weight: Font.DemiBold
                }
            }
        }
    }
    Repeater {
        model: {
            const r = card.recording
            if (!card.loaded)
                return []
            const notes = []
            if (r.proxies)
                notes.push({ id: "proxies", icon: "zap", text: r.proxyReason })
            if (r.vfr)
                notes.push({ id: "vfr", icon: "timer", text: qsTr("Variable frame rate: the sequence runs at a steady %1 fps.").arg(r.fps.text) })
            if (r.hdr !== "")
                notes.push({ id: "hdr", icon: "sun", text: qsTr("HDR (%1): shown and edited tone-mapped to SDR.").arg(r.hdr) })
            return notes
        }
        delegate: RowLayout {
            id: note
            required property var modelData
            objectName: "gamingNote_" + modelData.id
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Icon {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: Theme.space(0.5)
                name: note.modelData.icon
                size: Theme.iconXs
                color: Theme.textTertiary
            }
            Text {
                Layout.fillWidth: true
                text: note.modelData.text
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }

    Toggle {
        objectName: "homeAddClips"
        Layout.fillWidth: true
        text: qsTr("Add the clips to the sequence")
        tooltip: qsTr("Edit the files into the sequence one after another; off: only into the Library")
        checked: card.addClips
        onToggled: card.addClipsToggled(checked)
    }

    // ---- Audio streams (a recording with several) ----
    ColumnLayout {
        objectName: "gamingStreams"
        Layout.fillWidth: true
        Layout.fillHeight: false
        visible: card.showStreams
        spacing: Theme.space(1)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                Layout.fillWidth: true
                text: qsTr("Audio streams")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMicro + 1
                font.weight: Theme.weightTitle
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Theme.microTracking
            }
            Text {
                text: qsTr("%n track(s)", "", card.includedCount)
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
        Repeater {
            model: streams
            delegate: RowLayout {
                id: streamRow
                required property int index
                required property int stream
                required property string name
                required property string suggested
                required property bool include
                required property string layout
                objectName: "gamingStream_" + index
                Layout.fillWidth: true
                spacing: Theme.space(2)

                Toggle {
                    objectName: "gamingInclude_" + streamRow.index
                    checked: streamRow.include
                    tooltip: qsTr("Include audio stream %1").arg(streamRow.index + 1)
                    onToggled: {
                        streams.setProperty(streamRow.index, "include", checked)
                        ++card.includeRevision
                    }
                }
                FieldInput {
                    objectName: "gamingTrackName_" + streamRow.index
                    Layout.fillWidth: true
                    enabled: streamRow.include
                    opacity: enabled ? 1 : Theme.disabledOpacity
                    text: streamRow.name
                    placeholderText: streamRow.suggested
                    maximumLength: 64
                    Accessible.name: qsTr("Track name for audio stream %1").arg(streamRow.index + 1)
                    onTextEdited: streams.setProperty(streamRow.index, "name", text)
                    onAccepted: card.createRequested()
                }
                Text {
                    Layout.preferredWidth: Theme.space(14)
                    text: streamRow.layout
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
                Text {
                    objectName: "gamingTarget_" + streamRow.index
                    Layout.preferredWidth: Theme.space(16)
                    horizontalAlignment: Text.AlignRight
                    // Recomputed when any row is included or left out.
                    text: {
                        card.includeRevision
                        const target = card.targetOf(streamRow.index)
                        return target !== "" ? qsTr("→ %1").arg(target) : qsTr("Left out")
                    }
                    color: streamRow.include ? Theme.accentStroke : Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
        }
    }
}
