pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Shapes

// New gaming video (M5o, gaming): a game recording becomes a project in one step. Drop a recording (anywhere on the
// Home screen: HomeScreen sends a single video here) or choose one; GamingStart.inspect() reads it off the GUI thread
// and the card shows its size, frame rate, duration and codec with badges (4K, high fps, long: proxies, VFR, HDR shown
// tone-mapped), one row per audio stream (include it or leave it out, its track name with Game / Mic / Discord
// suggested, the channel layout and the track it lands on), then the project's name (the recording's) and folder.
// Create asks Main, through `gamingRequested`, to run GamingStart.create() after the unsaved-changes prompt.
HomeCard {
    id: card
    objectName: "homeGamingCard"

    property url folder: Home.defaultFolder
    property string error: ""
    property bool dragActive: false
    readonly property var recording: GamingStart.recording
    readonly property bool loaded: recording.ok === true
    readonly property bool inspecting: GamingStart.inspecting
    /// Bumped when a stream is included or left out: the rows' target tracks follow it.
    property int includeRevision: 0

    signal gamingRequested(string name, url folder, var tracks)

    /// Reads a recording (a drop on the Home screen, the file dialog).
    function inspect(url) {
        error = ""
        GamingStart.inspect(url)
    }
    function choose() {
        recordingDialog.open()
    }
    /// Back to the empty card (Home opening again).
    function reset() {
        error = ""
        GamingStart.clear()
    }
    /// The audio plan for GamingStart.create(): [{ index, name, include }].
    function tracks() {
        const list = []
        for (let i = 0; i < streams.count; ++i) {
            const row = streams.get(i)
            list.push({ index: row.stream, name: row.name.trim() !== "" ? row.name.trim() : row.suggested, include: row.include })
        }
        return list
    }
    /// The sequence track a stream lands on ("A2"), or "" when it is left out.
    function targetOf(row) {
        if (row < 0 || row >= streams.count || !streams.get(row).include)
            return ""
        let n = 0
        for (let i = 0; i <= row; ++i) {
            if (streams.get(i).include)
                ++n
        }
        return "A" + n
    }
    function create() {
        if (!loaded) {
            error = qsTr("Choose a game recording first.")
            return
        }
        error = Home.validateProject(nameField.text, folder)
        if (error === "")
            gamingRequested(nameField.text.trim(), folder, tracks())
    }
    function load() {
        ++includeRevision
        streams.clear()
        const audio = recording.audio ?? []
        for (const a of audio)
            streams.append({ stream: a.index, name: a.suggested, suggested: a.suggested, include: true, layout: a.layout })
        if (loaded)
            nameField.text = recording.name
    }

    title: qsTr("New gaming video")
    hue: "pink"

    Component.onCompleted: load()
    Connections {
        target: GamingStart
        function onRecordingChanged() {
            card.load()
        }
    }
    ListModel {
        id: streams
    }

    Text {
        Layout.fillWidth: true
        visible: !card.loaded
        text: qsTr("Start from a game recording: the sequence matches its size and frame rate, and every audio stream gets its own named track.")
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    // Drop zone: a recording dropped anywhere on Home lights it.
    Item {
        id: zone
        objectName: "gamingDropZone"
        Layout.fillWidth: true
        visible: !card.loaded
        implicitHeight: zoneColumn.implicitHeight + Theme.space(8)

        scale: card.dragActive ? Theme.liftScale : 1
        Behavior on scale { NumberAnimation { duration: Theme.durBounce; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSpring } }

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusCard
            color: card.dragActive ? Theme.accentWash : Theme.sunken
            Behavior on color { ColorAnimation { duration: Theme.durFast } }

            Shape {
                anchors.fill: parent
                ShapePath {
                    strokeColor: card.dragActive ? Theme.accentStroke : Theme.borderStrong
                    strokeWidth: card.dragActive ? Theme.focusRingWidth : Theme.hairline * 1.5
                    strokeStyle: ShapePath.DashLine
                    dashPattern: [4, 3]
                    fillColor: "transparent"
                    PathRectangle {
                        x: Theme.hairline
                        y: Theme.hairline
                        width: zone.width - Theme.hairline * 2
                        height: zone.height - Theme.hairline * 2
                        radius: Theme.radiusCard
                    }
                }
            }
        }

        ColumnLayout {
            id: zoneColumn
            anchors.centerIn: parent
            width: parent.width - Theme.space(8)
            spacing: Theme.space(2)

            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: Theme.iconXl + Theme.space(4)
                implicitHeight: implicitWidth
                radius: width / 2
                color: card.dragActive ? Theme.accentFill : Theme.accentWash
                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                Icon {
                    anchors.centerIn: parent
                    name: "gamepad-2"
                    size: Theme.iconXl
                    color: card.dragActive ? Theme.accentOnFill : Theme.accentStroke
                }
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: card.inspecting ? qsTr("Reading the recording…")
                                      : card.dragActive ? qsTr("Drop to start a gaming video") : qsTr("Drop a game recording here")
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXl
                font.weight: Theme.weightTitle
            }
            BeamProgress {
                objectName: "gamingInspectProgress"
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space(6)
                Layout.rightMargin: Theme.space(6)
                visible: card.inspecting
                indeterminate: true
                active: card.inspecting
                loader: "build"
                label: qsTr("Reading the recording")
            }
            Text {
                Layout.fillWidth: true
                visible: !card.inspecting
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("MP4 · MKV · MOV · FLV · TS — from OBS, ShadowPlay, Game Bar or any capture tool")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                objectName: "gamingChooseButton"
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.space(1)
                enabled: !card.inspecting
                iconName: "folder-open"
                text: qsTr("Choose a recording…")
                tooltip: qsTr("Choose a game recording to start from")
                onClicked: card.choose()
            }
        }
    }

    // A file that could not be read.
    Text {
        objectName: "gamingInspectError"
        Layout.fillWidth: true
        visible: !card.loaded && !card.inspecting && (card.recording.error ?? "") !== ""
        text: card.recording.error ?? ""
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    // The recording: name, summary, badges.
    ColumnLayout {
        Layout.fillWidth: true
        visible: card.loaded
        spacing: Theme.space(2)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Icon {
                name: "film"
                size: Theme.iconSm
                color: Theme.accentStroke
            }
            Text {
                objectName: "gamingRecordingName"
                Layout.fillWidth: true
                text: card.recording.name ?? ""
                elide: Text.ElideMiddle
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textLg
                font.weight: Theme.weightTitle
            }
            PillButton {
                objectName: "gamingChangeButton"
                compact: true
                variant: "ghost"
                iconName: "folder-open"
                text: qsTr("Change…")
                tooltip: qsTr("Choose another recording")
                onClicked: card.choose()
            }
        }
        Text {
            objectName: "gamingSummary"
            Layout.fillWidth: true
            text: {
                const r = card.recording
                if (!card.loaded)
                    return ""
                const parts = [r.sizeText, qsTr("%1 fps").arg(r.fps.text), r.durationText, r.videoCodec]
                if (r.fileSizeText !== "")
                    parts.push(r.fileSizeText)
                return parts.join(" · ")
            }
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
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
                        color: badge.modelData.tone
                        font.family: Theme.fontSans
                        font.capitalization: Font.AllUppercase
                        font.pixelSize: Theme.textMicro
                        font.letterSpacing: Theme.microTracking
                        font.weight: Font.DemiBold
                    }
                }
            }
        }
        // Why proxies, and what VFR / HDR mean for the edit.
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
    }

    // One row per audio stream: include, track name, layout, the track it lands on.
    ColumnLayout {
        Layout.fillWidth: true
        visible: card.loaded
        spacing: Theme.space(1)

        Text {
            text: qsTr("Audio tracks")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
            font.letterSpacing: Theme.labelTracking
        }
        Text {
            objectName: "gamingNoAudio"
            visible: streams.count === 0
            text: qsTr("This recording has no audio.")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
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
                    onAccepted: card.create()
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

    // The project: name and folder.
    GridLayout {
        Layout.fillWidth: true
        visible: card.loaded
        columns: 2
        columnSpacing: Theme.space(2)
        rowSpacing: Theme.space(2)

        Text {
            text: qsTr("Name")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        FieldInput {
            id: nameField
            objectName: "gamingProjectName"
            Layout.fillWidth: true
            placeholderText: qsTr("Project name")
            Accessible.name: qsTr("Gaming project name")
            onTextEdited: card.error = ""
            onAccepted: card.create()
        }

        Text {
            text: qsTr("Location")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Theme.controlHeight
                radius: height / 2
                color: Theme.sunken
                Text {
                    objectName: "gamingProjectFolder"
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space(4)
                    anchors.rightMargin: Theme.space(4)
                    verticalAlignment: Text.AlignVCenter
                    text: Home.projectPath(nameField.text, card.folder)
                    elide: Text.ElideMiddle
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
            PillButton {
                objectName: "gamingBrowseButton"
                iconName: "folder-open"
                text: qsTr("Browse…")
                tooltip: qsTr("Choose where to save the gaming project")
                onClicked: folderDialog.open()
            }
        }
    }

    // The sequence it makes, the error, Create.
    RowLayout {
        Layout.fillWidth: true
        visible: card.loaded
        spacing: Theme.space(2)
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Text {
                objectName: "gamingSequenceSummary"
                Layout.fillWidth: true
                text: card.loaded ? qsTr("Sequence %1 · %2 fps").arg(card.recording.sizeText).arg(card.recording.fps.text) : ""
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
            Text {
                objectName: "gamingError"
                Layout.fillWidth: true
                visible: card.error !== ""
                text: card.error
                wrapMode: Text.Wrap
                color: Theme.danger
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
        PillButton {
            objectName: "gamingCreateButton"
            variant: "primary"
            iconName: "plus"

            text: qsTr("Create")
            tooltip: qsTr("Create the project with the recording on its timeline")
            onClicked: card.create()
        }
    }

    FileDialog {
        id: recordingDialog
        title: qsTr("Choose a game recording")
        fileMode: FileDialog.OpenFile
        nameFilters: GamingStart.nameFilters
        currentFolder: ProjectFile.folder
        onAccepted: card.inspect(selectedFile)
    }
    FolderDialog {
        id: folderDialog
        currentFolder: card.folder
        onAccepted: card.folder = selectedFolder
    }
}
