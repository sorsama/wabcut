pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Home's New project page (design language v3): easy by default, Advanced for pros. Two columns that fit the window:
//   Format    preset tiles with the frame's shape (Home.presets), grouped Landscape / Vertical & square / Custom; with
//             media from the all-in-one Import, "Match my media" (the first video's size and rate) comes first and is
//             chosen. Above it, "Start with media" (StartWithMedia): the files, Add the clips to the sequence and, for a
//             recording with several audio streams, the streams table.
//   Details   the name (a free one: "Untitled Project", "Untitled Project 2"...), the location (Browse...), the
//             summary line (Home.settingsSummary), Advanced settings (AdvancedSettings; its open state remembered),
//             the error, Cancel and the lime Create project.
// The last choices (preset, size, rate, Advanced values) are remembered (Preferences "home/newProjectChoices"). With a
// recording that has several audio streams, the audio tracks follow the streams included (one each) unless the
// Advanced field was changed, and never fewer than they need: the field, the summary and the project agree. Match my
// media creates the size and rate the tile shows. Create asks Main through `createRequested` /
// `createFromMediaRequested`; Main runs the unsaved-changes prompt and the backend (Home.createProject /
// Home.createFromMedia) and reports a refusal back with `error`.
Item {
    id: page
    objectName: "homeNewProjectPage"

    property bool revealed: true
    property url folder: Home.defaultFolder
    /// The name the default is made from ("Untitled Project", or the first media file's).
    property string baseName: qsTr("Untitled Project")
    property bool nameEdited: false
    /// A Home.presets id, "match" (Match my media) or "custom".
    property string presetId: "1080p23976"
    property int frameWidth: 1920
    property int frameHeight: 1080
    property int rateNum: 24000
    property int rateDen: 1001
    /// Home.createProject's Advanced keys (as the user chose them; `effectiveAdvanced` is what the project gets).
    property var advanced: page.copy(Home.advancedDefaults)
    /// The Advanced audio tracks were changed on this page (else they follow a recording's streams).
    property bool audioTracksEdited: false
    /// Why the backend refused (Main sets it); cleared by any edit.
    property string error: ""
    property bool advancedOpen: Preferences.homeAdvancedOpen
    /// "Start with media": [{ url, name, sizeText, kind }] (empty: a plain new project).
    property var files: []
    property bool addClips: true
    // The arrows moved the chosen tile: the newly chosen tile takes the focus.
    property bool moveFocusToSelection: false

    readonly property bool mediaMode: page.files.length > 0
    readonly property url firstVideo: {
        for (const f of page.files) {
            if (f.kind === "video")
                return f.url
        }
        return ""
    }
    readonly property var recording: GamingStart.recording
    readonly property bool recordingReady: page.recording.ok === true && !GamingStart.inspecting
                                           && page.firstVideo.toString() !== ""
    readonly property bool matchAvailable: page.mediaMode && page.firstVideo.toString() !== ""
                                           && (GamingStart.inspecting || page.recordingReady)
    readonly property bool matching: page.presetId === "match"
    readonly property var rate: Home.rates.find(r => r.rateNum * page.rateDen === page.rateNum * r.rateDen) ?? null
    readonly property bool canDropFrame: page.rate !== null && page.rate.canDropFrame === true
    readonly property string rateText: Home.rateLabel(page.rateNum, page.rateDen)
    /// The chosen tile's name ("1080p", "Vertical 9:16", "Match my media", "Custom"), beside the frame's shape.
    readonly property string formatName: page.matching ? qsTr("Match my media")
                                                       : (page.presetById(page.presetId)?.name ?? qsTr("Custom"))
    readonly property var groups: {
        const out = []
        if (page.matchAvailable) {
            const r = page.recording
            out.push({ key: "media", label: qsTr("Your media"), tiles: [{
                id: "match", name: qsTr("Match my media"),
                detail: page.recordingReady ? qsTr("%1 · %2 fps").arg(r.sizeText).arg(r.fps.text) : qsTr("Reading…"),
                shape: !page.recordingReady || r.width > r.height ? "landscape" : r.width < r.height ? "portrait" : "square"
            }] })
        }
        const names = { landscape: qsTr("Landscape"), vertical: qsTr("Vertical & square"), custom: qsTr("Custom") }
        for (const key of ["landscape", "vertical", "custom"]) {
            out.push({ key: key, label: names[key], tiles: Home.presets.filter(p => p.group === key).map(p => ({
                id: p.id, name: p.name, shape: p.custom ? "custom" : p.shape,
                detail: p.custom ? qsTr("Any size") : qsTr("%1 fps").arg(p.rateLabel)
            })) })
        }
        return out
    }
    readonly property var tileOrder: page.groups.reduce((ids, g) => ids.concat(g.tiles.map(t => t.id)), [])
    /// A recording with several audio streams: each included stream gets its own audio track.
    readonly property bool streamTracks: page.mediaMode && media.showStreams
    /// The Advanced values the project gets: with a recording's streams, the audio tracks are one per included stream
    /// (or what the user typed, never fewer than the streams need - the backend adds those).
    readonly property var effectiveAdvanced: {
        const a = page.copy(page.advanced)
        if (page.streamTracks) {
            const needed = Math.max(1, media.includedCount)
            a.audioTracks = page.audioTracksEdited ? Math.max(a.audioTracks ?? 3, needed) : needed
        }
        return a
    }
    readonly property var currentSettings: page.settingsFor(false)
    readonly property var summarySettings: page.settingsFor(true)
    readonly property string liveError: {
        const e = Home.validateProject(nameField.text, page.folder)
        return e !== "" ? e : Home.validateSettings(page.summarySettings)
    }
    readonly property bool advancedCustomized: {
        const d = Home.advancedDefaults
        for (const key of Object.keys(d)) {
            if (!page.sameValue(key, page.advanced[key], d[key]))
                return true
        }
        return false
    }

    signal createRequested(string name, url folder, var settings)
    signal createFromMediaRequested(string name, url folder, var settings, var files, var options)
    signal cancelRequested()

    function copy(value: var): var {
        return JSON.parse(JSON.stringify(value))
    }
    function sameValue(key: string, a: var, b: var): bool {
        if (key === "pixelAspect")
            return (a?.num ?? 1) * (b?.den ?? 1) === (b?.num ?? 1) * (a?.den ?? 1)
        if (key === "colorManagement")
            return (a?.mode ?? a) === (b?.mode ?? b)
        return a === b
    }
    function presetById(id: string): var {
        return Home.presets.find(p => p.id === id) ?? null
    }
    /// The non-custom preset with this size and rate ("" when none).
    function presetFor(w: int, h: int, num: int, den: int): string {
        const p = Home.presets.find(p => !p.custom && p.width === w && p.height === h && p.rateNum * den === num * p.rateDen)
        return p ? p.id : ""
    }
    /// Chooses a tile: a preset sets the size and rate, Match my media takes the first video's, Custom keeps them.
    function selectPreset(id: string) {
        page.error = ""
        if (id === "match") {
            page.presetId = "match"
            page.takeRecordingFormat()
            return
        }
        const p = page.presetById(id)
        if (!p)
            return
        page.presetId = id
        if (p.custom) {
            page.setAdvancedOpen(true)
            return
        }
        page.frameWidth = p.width
        page.frameHeight = p.height
        page.rateNum = p.rateNum
        page.rateDen = p.rateDen
    }
    /// The arrows on the tiles: the previous or next tile.
    function stepPreset(direction: int) {
        const order = page.tileOrder
        const at = order.indexOf(page.presetId)
        const next = Math.max(0, Math.min(order.length - 1, (at < 0 ? 0 : at) + direction))
        if (order[next] === page.presetId)
            return
        page.moveFocusToSelection = true
        page.selectPreset(order[next])
    }
    /// Match my media: the first video's size and rate (once it has been read).
    function takeRecordingFormat() {
        if (!page.recordingReady)
            return
        page.frameWidth = page.recording.width
        page.frameHeight = page.recording.height
        page.rateNum = page.recording.fps.num
        page.rateDen = page.recording.fps.den
    }
    /// Advanced: a new size or rate; the tile follows (the preset with those values, else Custom).
    function setFormat(w: int, h: int, num: int, den: int) {
        page.error = ""
        page.frameWidth = w
        page.frameHeight = h
        page.rateNum = num
        page.rateDen = den
        const id = page.presetFor(w, h, num, den)
        if (id !== "")
            page.presetId = id
        else if (page.recordingReady && page.mediaMode && w === page.recording.width && h === page.recording.height
                 && num * page.recording.fps.den === page.recording.fps.num * den)
            page.presetId = "match"
        else
            page.presetId = "custom"
    }
    function setAdvanced(key: string, value: var) {
        page.error = ""
        if (key === "audioTracks")
            page.audioTracksEdited = true
        const next = page.copy(page.advanced)
        next[key] = value
        page.advanced = next
    }
    function setAdvancedOpen(open: bool) {
        page.advancedOpen = open
        Preferences.homeAdvancedOpen = open
    }
    /// "Reset to defaults": the Advanced values as a new project has them, the chosen preset's size and rate.
    function resetAdvanced() {
        page.advanced = page.copy(Home.advancedDefaults)
        page.audioTracksEdited = false
        if (page.matching)
            page.takeRecordingFormat()
        else if (page.presetId !== "custom")
            page.selectPreset(page.presetId)
    }
    /// The settings to create with: the size and rate shown (Match my media: the first video's, as its tile says), the
    /// Advanced values with the tracks a recording's streams need. `forSummary`: also while the video is still being
    /// read (Create waits for it; until then Match my media is asked for as `match`).
    function settingsFor(forSummary: bool): var {
        const s = page.copy(page.effectiveAdvanced)
        s.dropFrame = page.advanced.dropFrame === true && page.canDropFrame
        if (page.matching && !page.recordingReady && !forSummary) {
            s.match = true
        } else {
            s.width = page.frameWidth
            s.height = page.frameHeight
            s.rateNum = page.rateNum
            s.rateDen = page.rateDen
        }
        return s
    }
    /// Remembers the choices for the next new project (Match my media keeps the last preset).
    function remember() {
        const saved = page.savedChoices()
        const keep = page.matching ? saved : { presetId: page.presetId, width: page.frameWidth, height: page.frameHeight,
                                               rateNum: page.rateNum, rateDen: page.rateDen }
        Preferences.homeNewProjectChoices = JSON.stringify(Object.assign({}, keep, { advanced: page.advanced }))
    }
    function savedChoices(): var {
        try {
            const value = JSON.parse(Preferences.homeNewProjectChoices)
            return value !== null && typeof value === "object" ? value : {}
        } catch (e) {
            return {}
        }
    }
    /// The remembered choices (a stale or broken entry falls back to the defaults).
    function restore() {
        const saved = page.savedChoices()
        page.advanced = Object.assign(page.copy(Home.advancedDefaults), saved.advanced ?? {})
        page.selectPreset(Home.presets[0].id)
        const id = String(saved.presetId ?? "")
        const p = page.presetById(id)
        if (p && p.custom && saved.width > 0 && saved.height > 0 && saved.rateNum > 0 && saved.rateDen > 0) {
            page.presetId = "custom"
            page.frameWidth = saved.width
            page.frameHeight = saved.height
            page.rateNum = saved.rateNum
            page.rateDen = saved.rateDen
        } else if (p) {
            page.selectPreset(id)
            if (p.rateChoice && saved.rateNum > 0 && saved.rateDen > 0)
                page.setFormat(p.width, p.height, saved.rateNum, saved.rateDen)
        }
        if (Home.validateSettings(page.settingsFor(true)) !== "") {
            page.advanced = page.copy(Home.advancedDefaults)
            page.selectPreset(Home.presets[0].id)
        }
    }
    function setDefaultName() {
        nameField.text = Home.uniqueProjectName(page.baseName, page.folder)
        page.nameEdited = false
    }
    /// A plain new project: the remembered format, a free name, the default folder.
    function reset() {
        page.error = ""
        page.files = []
        page.addClips = true
        page.audioTracksEdited = false
        GamingStart.clear()
        page.folder = Home.defaultFolder
        page.baseName = qsTr("Untitled Project")
        page.restore()
        page.setDefaultName()
    }
    /// "Start with media" from the all-in-one Import (Home.classifyImport's result): the files, Match my media chosen
    /// when there is a video (GamingStart reads it), the name from the first file.
    function startWithMedia(classified: var) {
        page.reset()
        const list = []
        for (const f of classified.media ?? [])
            list.push({ url: f.url, name: f.name, sizeText: f.sizeText ?? "", kind: f.kind ?? "file" })
        for (const f of classified.captions ?? [])
            list.push({ url: f.url, name: f.name, sizeText: f.sizeText ?? "", kind: "captions" })
        for (const f of classified.lottie ?? [])
            list.push({ url: f.url, name: f.name, sizeText: f.sizeText ?? "", kind: "lottie" })
        for (const f of classified.layered ?? [])
            list.push({ url: f.url, name: f.name, sizeText: f.sizeText ?? "", kind: "layered" })
        page.files = list
        if (String(classified.suggestedName ?? "") !== "")
            page.baseName = classified.suggestedName
        page.setDefaultName()
        if (page.firstVideo.toString() !== "") {
            page.presetId = "match"
            GamingStart.inspect(page.firstVideo)
        }
    }
    function removeFile(index: int) {
        const before = page.firstVideo.toString()
        const list = page.files.slice()
        list.splice(index, 1)
        page.files = list
        const after = page.firstVideo.toString()
        if (after === before)
            return
        if (after === "") {
            GamingStart.clear()
            if (page.matching)
                page.restore()
        } else {
            GamingStart.inspect(page.firstVideo)
        }
    }
    function focusName() {
        nameField.forceActiveFocus(Qt.TabFocusReason)
        nameField.selectAll()
    }
    function create() {
        const name = nameField.text.trim()
        page.error = Home.validateProject(name, page.folder)
        if (page.error !== "") {
            page.focusName()
            return
        }
        page.error = Home.validateSettings(page.summarySettings)
        if (page.error !== "") {
            page.setAdvancedOpen(true)
            return
        }
        if (page.matching && !page.recordingReady && GamingStart.inspecting) {
            page.error = qsTr("Still reading the video: try again in a moment.")
            return
        }
        page.remember()
        if (!page.mediaMode) {
            page.createRequested(name, page.folder, page.currentSettings)
            return
        }
        const options = { addToSequence: page.addClips }
        if (media.showStreams)
            options.tracks = media.tracks()
        page.createFromMediaRequested(name, page.folder, page.currentSettings, page.files.map(f => f.url), options)
    }

    Connections {
        target: GamingStart
        function onRecordingChanged() {
            if (page.matching && page.recordingReady)
                page.takeRecordingFormat()
            else if (page.matching && !GamingStart.inspecting && page.recording.ok !== true && page.mediaMode)
                page.restore() // the video could not be read: back to the remembered format
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.gap

        // ---- Left: the media and the format ----
        Flickable {
            id: leftFlick
            objectName: "homeFormatColumn"
            Layout.preferredWidth: Math.round((page.width - Theme.gap) * 0.55)
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: leftColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            ScrollBar.vertical: ThinScrollBar {}

            ColumnLayout {
                id: leftColumn
                width: leftFlick.width - (leftFlick.interactive ? Theme.space(3) : 0)
                // At least the column's height: Format fills it (its tiles at the top), level with Details beside it.
                height: Math.max(implicitHeight, leftFlick.height)
                spacing: Theme.gap

                StartWithMedia {
                    id: media
                    Layout.fillWidth: true
                    visible: page.mediaMode
                    order: 0
                    revealed: page.revealed
                    files: page.files
                    firstVideo: page.firstVideo
                    addClips: page.addClips
                    onRemoveRequested: index => page.removeFile(index)
                    onAddClipsToggled: on => page.addClips = on
                    onCreateRequested: page.create()
                }

                HomeCard {
                    id: formatCard
                    objectName: "homeFormatCard"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    order: 1
                    revealed: page.revealed
                    title: qsTr("Format")
                    hue: "blue"

                    Repeater {
                        model: page.groups
                        delegate: ColumnLayout {
                            id: group
                            required property var modelData
                            // Tiles at least 36 units wide, sharing the card's width (from the card, not from this
                            // layout: no layout loop).
                            readonly property real room: formatCard.width - formatCard.padding * 2
                            readonly property int columns: Math.max(2, Math.floor((room + Theme.space(2)) / (Theme.space(36) + Theme.space(2))))
                            readonly property real tileWidth: Math.floor((room - Theme.space(2) * (columns - 1)) / columns)
                            objectName: "homePresetGroup_" + modelData.key
                            Layout.fillWidth: true
                            Layout.fillHeight: false
                            spacing: Theme.space(1.5)

                            Text {
                                Layout.fillWidth: true
                                text: group.modelData.label
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                                font.weight: Theme.weightLabel
                            }
                            Grid {
                                columns: group.columns
                                columnSpacing: Theme.space(2)
                                rowSpacing: Theme.space(2)

                                Repeater {
                                    model: group.modelData.tiles
                                    delegate: PresetTile {
                                        id: tile
                                        required property var modelData
                                        objectName: "homePreset_" + modelData.id
                                        // Match my media is two tiles wide (its size and rate in full).
                                        width: modelData.id === "match" ? group.tileWidth * 2 + Theme.space(2) : group.tileWidth
                                        name: modelData.name
                                        // Custom, chosen: the size and rate it makes.
                                        detail: tile.modelData.id === "custom" && page.presetId === "custom"
                                                ? qsTr("%1 × %2 · %3").arg(page.frameWidth).arg(page.frameHeight).arg(page.rateText)
                                                : tile.modelData.detail
                                        shape: modelData.shape
                                        selected: page.presetId === modelData.id
                                        onClicked: page.selectPreset(tile.modelData.id)
                                        onStepRequested: direction => page.stepPreset(direction)
                                        onSelectedChanged: {
                                            if (tile.selected && page.moveFocusToSelection) {
                                                page.moveFocusToSelection = false
                                                tile.forceActiveFocus(Qt.TabFocusReason)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // The tiles stay at the top of the island.
                    Item {
                        Layout.fillHeight: true
                    }
                }
            }
        }

        // ---- Right: name, location, Advanced, Create ----
        HomeCard {
            id: details
            objectName: "homeProjectDetails"
            Layout.fillWidth: true
            Layout.fillHeight: true
            order: 2
            revealed: page.revealed
            title: qsTr("Details")
            hue: "violet"

            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: false
                columns: 2
                columnSpacing: Theme.space(4)
                rowSpacing: Theme.space(2)

                Text {
                    Layout.preferredWidth: Theme.space(18)
                    text: qsTr("Name")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                FieldInput {
                    id: nameField
                    objectName: "homeProjectName"
                    Layout.fillWidth: true
                    implicitHeight: Theme.controlHeight
                    placeholderText: qsTr("Project name")
                    font.weight: Font.DemiBold
                    maximumLength: 120
                    Accessible.name: qsTr("Project name")
                    onTextEdited: {
                        page.nameEdited = true
                        page.error = ""
                    }
                    onAccepted: page.create()
                }

                Text {
                    text: qsTr("Location")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(1.5)
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Theme.controlHeight
                        radius: height / 2
                        color: Theme.sunken
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.space(3.5)
                            anchors.rightMargin: Theme.space(4)
                            spacing: Theme.space(2)
                            Icon {
                                name: "folder"
                                size: Theme.iconXs
                                color: Theme.textTertiary
                            }
                            Text {
                                objectName: "homeProjectFolder"
                                Layout.fillWidth: true
                                text: Home.projectPath(nameField.text.trim(), page.folder)
                                elide: Text.ElideMiddle
                                color: Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                        }
                    }
                    PillButton {
                        objectName: "homeBrowseFolder"
                        iconName: "folder-open"
                        text: qsTr("Browse…")
                        tooltip: qsTr("Choose where to save the project")
                        onClicked: folderDialog.open()
                    }
                }
            }

            // The format the project gets: its frame drawn at its shape (16:9, 9:16, 1:1...) beside the format's name and
            // the summary line. With Advanced open, only the summary line stays (room for the rows).
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: false
                spacing: Theme.space(4)

                Item {
                    id: framePreview
                    objectName: "homeFramePreview"
                    visible: !page.advancedOpen
                    implicitWidth: Theme.space(22)
                    implicitHeight: Theme.space(14)
                    readonly property real aspect: page.frameHeight > 0 ? page.frameWidth / page.frameHeight : 16 / 9
                    /// "16:9", "9:16", "1:1"; an uncommon shape as "1.90:1".
                    readonly property string ratioText: {
                        if (page.frameWidth <= 0 || page.frameHeight <= 0)
                            return ""
                        let a = page.frameWidth
                        let b = page.frameHeight
                        while (b !== 0) {
                            const t = b
                            b = a % b
                            a = t
                        }
                        const w = page.frameWidth / a
                        const h = page.frameHeight / a
                        return w <= 32 && h <= 32 ? qsTr("%1:%2").arg(w).arg(h)
                                                  : qsTr("%1:1").arg((page.frameWidth / page.frameHeight).toFixed(2))
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: Math.round(Math.min(parent.width, parent.height * framePreview.aspect))
                        height: Math.round(width / framePreview.aspect)
                        radius: Theme.space(1.5)
                        color: Theme.sunken
                        border.width: Theme.hairline * 1.5
                        border.color: Theme.accentStroke

                        Text {
                            anchors.centerIn: parent
                            text: framePreview.ratioText
                            color: Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textMicro
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    Text {
                        objectName: "homeFormatName"
                        Layout.fillWidth: true
                        visible: !page.advancedOpen
                        text: page.formatName
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: Theme.fontDisplay
                        font.pixelSize: Theme.textTitle
                        font.weight: Theme.weightTitle
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        Icon {
                            visible: page.advancedOpen
                            name: "film"
                            size: Theme.iconXs
                            color: Theme.textTertiary
                        }
                        Text {
                            objectName: "homeSequenceSummary"
                            Layout.fillWidth: true
                            text: page.matching && !page.recordingReady ? qsTr("Matches your media")
                                                                         : Home.settingsSummary(page.summarySettings)
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textSm
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Theme.hairline
                color: Theme.border
            }

            // Advanced settings (closed by default; the open state is remembered)
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: false
                spacing: Theme.space(2)

                AbstractButton {
                    id: advancedToggle
                    objectName: "homeAdvancedToggle"
                    Layout.fillWidth: true
                    implicitHeight: Theme.controlHeight
                    leftPadding: Theme.space(1)
                    rightPadding: Theme.space(2)
                    focusPolicy: Qt.StrongFocus
                    hoverEnabled: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Advanced settings")
                    Accessible.description: page.advancedCustomized ? qsTr("Changed from the defaults") : ""
                    Accessible.checkable: true
                    Accessible.checked: page.advancedOpen
                    Keys.onReturnPressed: advancedToggle.clicked()
                    Keys.onEnterPressed: advancedToggle.clicked()
                    onClicked: page.setAdvancedOpen(!page.advancedOpen)

                    background: Rectangle {
                        radius: height / 2
                        color: advancedToggle.hovered ? Theme.hover : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.durFast } }
                    }
                    contentItem: RowLayout {
                        spacing: Theme.space(2)
                        Icon {
                            name: page.advancedOpen ? "chevron-down" : "chevron-right"
                            size: Theme.iconSm
                            color: Theme.textSecondary
                        }
                        Text {
                            text: qsTr("Advanced settings")
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Theme.weightLabel
                        }
                        Rectangle {
                            objectName: "homeAdvancedChanged"
                            visible: page.advancedCustomized
                            implicitWidth: changedText.implicitWidth + Theme.space(3)
                            implicitHeight: changedText.implicitHeight + Theme.space(1)
                            radius: Theme.radiusPill
                            color: Theme.accentWash
                            Text {
                                id: changedText
                                anchors.centerIn: parent
                                text: qsTr("Changed")
                                color: Theme.accentStroke
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                                font.weight: Theme.weightLabel
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                    FocusRing {
                        targetRadius: advancedToggle.height / 2
                        visible: advancedToggle.visualFocus
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                }
                HomeLink {
                    objectName: "homeAdvancedReset"
                    visible: page.advancedOpen
                    iconName: "rotate-ccw"
                    text: qsTr("Reset to defaults")
                    tooltip: qsTr("Advanced settings as a new project has them")
                    onClicked: page.resetAdvanced()
                }
            }

            Flickable {
                id: advancedFlick
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: Theme.space(24)
                visible: page.advancedOpen
                contentWidth: width
                contentHeight: advancedGrid.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height
                ScrollBar.vertical: ThinScrollBar {}

                AdvancedSettings {
                    id: advancedGrid
                    width: advancedFlick.width - (advancedFlick.interactive ? Theme.space(4) : 0)
                    frameWidth: page.frameWidth
                    frameHeight: page.frameHeight
                    rateNum: page.rateNum
                    rateDen: page.rateDen
                    canDropFrame: page.canDropFrame
                    advanced: page.effectiveAdvanced
                    onSizeEdited: (w, h) => page.setFormat(w, h, page.rateNum, page.rateDen)
                    onRateChosen: (num, den) => page.setFormat(page.frameWidth, page.frameHeight, num, den)
                    onValueEdited: (key, value) => page.setAdvanced(key, value)
                }
            }
            // Advanced closed: the error and the buttons stay at the bottom of the island.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: !page.advancedOpen
            }

            Text {
                objectName: "homeNewProjectError"
                Layout.fillWidth: true
                visible: text !== ""
                text: page.error !== "" ? page.error : page.liveError
                wrapMode: Text.Wrap
                color: Theme.danger
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: false
                spacing: Theme.space(2)
                Item { Layout.fillWidth: true }
                PillButton {
                    objectName: "homeCancelButton"
                    Layout.preferredHeight: Theme.controlHeightLarge
                    variant: "ghost"
                    text: qsTr("Cancel")
                    tooltip: qsTr("Back without creating a project (Esc)")
                    onClicked: page.cancelRequested()
                }
                LimeButton {
                    objectName: "homeCreateButton"
                    Layout.preferredWidth: Math.max(implicitWidth, Theme.space(44))
                    text: qsTr("Create project")
                    tooltip: page.mediaMode ? qsTr("Create the project with the media and open it")
                                            : qsTr("Create the project and open it")
                    onClicked: page.create()
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        currentFolder: page.folder
        onAccepted: {
            page.folder = selectedFolder
            page.error = ""
            if (!page.nameEdited)
                page.setDefaultName()
        }
    }
}
