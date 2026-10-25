pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// ● EXPORT (the Export workspace's panel; design language v3), driven by ExportController (docs/export.md):
//   * Where it goes: filter chips (All, each gaming destination in its hue, General) over a grid of preset tiles - the
//     gaming presets first (M5o, GamingExport: YouTube, Shorts / TikTok / Reels, Twitch, Discord size targets, GIF /
//     WebM), then the built-in ones - each with its size and export-time estimate; the chosen tile is the accent one.
//   * What it makes: the chosen preset's card (format, the size and time estimate, what to know), the settings drawer
//     and where the file goes.
//   * Pinned at the bottom: the chosen preset with its estimate and the Export button, then the export queue (live
//     progress pills), so both stay in view while the tiles scroll.
// Wide slots (760 px and up) put the tiles on the left and the rest in a column on the right; narrow ones stack them.
// Export queues a job on the current sequence's snapshot; the queue renders in the background, so editing continues.
// UX rework (stream L): the last choices come back (ExportController, per user): the preset or gaming preset picked
// last, the range picked last (else In to Out when the sequence has marks; the Export button then says so), and the
// folder exported to last (the default file name and Browse… start there). "Reset to defaults" forgets them.
Panel {
    id: root

    title: qsTr("Export")

    property string presetId: "match-source"
    // Drawer choices on top of the preset (ExportController.queueExport override keys).
    property var overrides: ({})
    // "" = ExportController's default file name for the sequence and container.
    property string outputPath: ""
    // M5o: a gaming preset ("" = a built-in one): a built-in base preset and its overrides, worked out for the
    // sequence and the range (a Discord size target sets the bitrate from the range's length).
    property string gamingPreset: ""
    /// The destination the tiles show: "all", a GamingExport group id or "general".
    property string filter: "all"
    /// The range was picked in the drawer since the panel opened: the suggested range (the remembered one, or In to
    /// Out for a marked sequence) no longer replaces it.
    property bool rangeChosen: false
    /// Anything remembered from earlier exports (for "Reset to defaults").
    readonly property bool remembers: ExportController.lastPreset !== "match-source" || ExportController.lastGamingPreset !== ""
                                      || ExportController.lastFolder !== "" || ExportController.lastRange !== ""
    /// The range this export covers ("whole", "inOut", "workArea", "custom").
    readonly property string range: overrides.range ?? "whole"
    /// The exporter's presets the gaming tiles already offer (I.9): left out of the built-in ones.
    readonly property var gamingDuplicates: ["youtube-1080p60", "youtube-1440p60", "youtube-4k60", "shorts-1080x1920-60",
                                             "twitch-1080p60", "discord-10mb", "discord-25mb", "discord-50mb",
                                             "gif-480p15", "webm-vp9-720p"]
    readonly property var gamingPlan: gamingPreset !== "" && ProjectController.revision >= 0
                                      ? GamingExport.plan(gamingPreset, ProjectController.sequenceId,
                                                          overrides.range ?? "whole")
                                      : ({})
    readonly property string effectivePresetId: gamingPlan.ok === true ? gamingPlan.presetId : presetId

    readonly property var effectiveOverrides: Object.assign({}, gamingPlan.ok === true ? gamingPlan.overrides : {},
                                                            overrides, outputPath !== "" ? { output: outputPath } : {})
    // Re-evaluated on project edits (marks, sequence), new encoder probes and queue changes (default names).
    readonly property var summary: ProjectController.revision >= 0 && ExportController.jobs.count >= 0
                                   && ExportController.autoEncoders
                                   ? ExportController.describe(ProjectController.sequenceId, effectivePresetId,
                                                               effectiveOverrides)
                                   : ({})
    // M5o: how big the file gets and how long the export takes (estimates).
    readonly property var estimate: ProjectController.revision >= 0 && ExportController.autoEncoders
                                    ? GamingExport.estimate(ProjectController.sequenceId, effectivePresetId,
                                                            effectiveOverrides)
                                    : ({})
    readonly property var estimateNotes: (gamingPlan.notes ?? []).concat(estimate.notes ?? [])
    readonly property string errorText: gamingPreset !== "" && gamingPlan.ok === false ? gamingPlan.error
                                        : summary.ok === false ? summary.error : ExportController.lastError
    /// The built-in presets shown as tiles (I.9's gaming presets are the gaming tiles, with their estimates: not twice).
    /// 0.2: the user's presets and the format presets (alpha, audio, stills) are the section below them.
    readonly property var builtInPresets: ExportController.presets.filter(p => root.gamingDuplicates.indexOf(p.id) < 0
                                                                           && (p.category === "general" || p.category === "hdr"))
    /// The chosen preset as the summary shows it: { name, description, icon, hue }.
    readonly property var chosen: {
        if (root.gamingPreset !== "") {
            const p = GamingExport.presets.find(g => g.id === root.gamingPreset)
            if (p)
                return { name: p.name, description: p.description, icon: p.icon, hue: gaming.hueOf(p.group) }
        }
        const b = ExportController.presets.find(e => e.id === root.presetId)
        return b ? { name: b.name, description: b.description, icon: b.icon, hue: gaming.hueOf("") }
                 : { name: "", description: "", icon: "film", hue: gaming.hueOf("") }
    }
    /// The filter chips: every destination in its hue.
    readonly property var filters: [{ id: "all", name: qsTr("All") }]
        .concat(GamingExport.groups.map(g => ({
            id: g.id,
            //: Export filter chip for the 9:16 presets (Shorts, TikTok, Reels)
            name: g.id === "vertical" ? qsTr("Shorts · TikTok · Reels") : g.name,
            hue: gaming.hueOf(g.id)
        })))
        .concat([{ id: "general", name: qsTr("General"), hue: gaming.hueOf("") }])
    readonly property bool showsBuiltIn: filter === "all" || filter === "general"

    /// "0:42", "1:05", "1:02:10".
    function clock(seconds) {
        const total = Math.max(1, Math.round(seconds))
        const h = Math.floor(total / 3600)
        const m = Math.floor(total / 60) % 60
        const s = total % 60
        const two = n => (n < 10 ? "0" : "") + n
        return h > 0 ? h + ":" + two(m) + ":" + two(s) : m + ":" + two(s)
    }
    function setOverride(key, value) {
        const next = Object.assign({}, overrides)
        if (value === undefined)
            delete next[key]
        else
            next[key] = value
        overrides = next
    }
    // The range (and a custom range's In and Out) outlive a preset change: the range is the sequence's, not the
    // preset's.
    function rangeOverrides() {
        const kept = {}
        for (const key of ["range", "rangeIn", "rangeOut"]) {
            if (overrides[key] !== undefined)
                kept[key] = overrides[key]
        }
        return kept
    }
    function choosePreset(id) {
        gamingPreset = ""
        presetId = id
        overrides = rangeOverrides()
        ExportController.lastPreset = id
        ExportController.lastGamingPreset = ""
    }
    // M5o: a gaming preset keeps the range the drawer chose (its size target depends on it).
    function chooseGamingPreset(id) {
        gamingPreset = id
        overrides = rangeOverrides()
        ExportController.lastGamingPreset = id
    }
    /// The drawer's range picker: this export's range, remembered for the next one ("custom" is not).
    function chooseRange(value) {
        rangeChosen = true
        setOverride("range", value)
        ExportController.lastRange = value === undefined ? "whole" : value
    }
    /// The range to start from (until one is picked here): the remembered one when the sequence has it, else In to Out
    /// when it has marks, else the whole sequence (ExportController.suggestedRange).
    function applySuggestedRange() {
        if (rangeChosen)
            return
        const range = ExportController.suggestedRange(ProjectController.sequenceId)
        const next = range === "" || range === "whole" ? undefined : range
        if (overrides.range !== next)
            setOverride("range", next)
    }
    /// Reset to defaults: forgets the remembered preset, range and folder, and starts over from Match Source.
    function resetChoices() {
        ExportController.forgetExportChoices()
        gamingPreset = ""
        presetId = "match-source"
        outputPath = ""
        overrides = ({})
        rangeChosen = false
        applySuggestedRange()
    }
    function startExport() {
        if (gamingPreset !== "" && gamingPlan.ok !== true)
            return
        if (ExportController.queueExport(ProjectController.sequenceId, effectivePresetId, effectiveOverrides) !== "")
            outputPath = "" // the next export gets a fresh default name
    }
    // 0.2, Premiere's Send to Media Encoder (Alt+Shift+M): the export waits in the queue for Start Queue.
    function sendToQueue() {
        if (gamingPreset !== "" && gamingPlan.ok !== true)
            return
        if (ExportController.sendToQueue(ProjectController.sequenceId, [effectivePresetId], effectiveOverrides, true).length > 0)
            outputPath = ""
        queueDialog.openOn("queue")
    }

    // The auto encoders are probed (in the background) the first time the workspace is shown. The suggested range
    // follows the marks set since (until a range is picked here).
    onVisibleChanged: {
        if (!visible)
            return
        ExportController.probeEncoders()
        applySuggestedRange()
    }
    // The last choices (UX rework): set once, so a pick here is never overwritten.
    Component.onCompleted: {
        presetId = ExportController.lastPreset
        gamingPreset = ExportController.lastGamingPreset
        applySuggestedRange()
    }
    Connections {
        target: ProjectController
        function onSequenceChanged() {
            root.applySuggestedRange()
        }
    }

    Binding {
        target: ExportController
        property: "project"
        value: ProjectController
    }

    // A glyph in a circle of the destination's hue (black glyph), as on the tiles.
    component HueGlyph: Rectangle {
        id: glyph
        property string iconName: ""
        property color hue: Theme.accentStroke
        implicitWidth: Theme.space(9)
        implicitHeight: Theme.space(9)
        radius: height / 2
        color: hue

        Icon {
            anchors.centerIn: parent
            name: glyph.iconName
            size: Theme.iconMd
            color: Theme.clipInk
        }
    }
    // A caps label over a value, e.g. "FILE SIZE / ≈ 115 MB".
    component Stat: ColumnLayout {
        id: stat
        property string label: ""
        property string value: ""
        spacing: Theme.space(0.5)

        Text {
            Layout.fillWidth: true
            text: stat.label
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMicro
            font.weight: Theme.weightTitle
            font.capitalization: Font.AllUppercase
            font.letterSpacing: Theme.microTracking * 1.5
        }
        Text {
            Layout.fillWidth: true
            text: stat.value
            elide: Text.ElideRight
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textTitle
            font.weight: Theme.weightTitle
        }
    }

    Item {
        id: page
        objectName: "exportPage"
        anchors.fill: parent

        readonly property bool wide: width >= Theme.space(190)
        readonly property real gap: Theme.space(3)
        readonly property real sideWidth: wide ? Math.min(Theme.space(100), Math.round(width * 0.42)) : width
        readonly property real gridWidth: wide ? width - sideWidth - gap : width
        /// Tiles per row: as many as fit at 180 px or more.
        readonly property int tileColumns: Math.max(1, Math.floor((gridWidth + Theme.space(2)) / (Theme.space(45) + Theme.space(2))))

        // ---- Where it goes: the filter and the tiles (all of the panel's width when narrow, its left when wide) ----
        Flickable {
            id: flick
            objectName: "exportPresetScroll"
            x: 0
            y: 0
            width: page.gridWidth
            height: page.wide ? page.height : page.height - dock.height - page.gap
            contentWidth: width
            contentHeight: column.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}

            ColumnLayout {
                id: column
                width: flick.width
                spacing: Theme.space(3)

                ExportFilterChips {
                    Layout.fillWidth: true
                    model: root.filters
                    current: root.filter
                    onActivated: id => root.filter = id
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: gaming.shownCount > 0
                    spacing: Theme.space(2)

                    ExportGroupLabel {
                        Layout.fillWidth: true
                        text: qsTr("For gaming")
                        hue: root.filter === "all" ? Theme.accentStroke : gaming.hueOf(root.filter)
                        count: gaming.shownCount
                    }
                    GamingPresets {
                        id: gaming
                        Layout.fillWidth: true
                        columns: page.tileColumns
                        selected: root.gamingPreset
                        sequenceId: ProjectController.sequenceId
                        range: root.overrides.range ?? "whole"
                        filter: root.filter
                        onChosen: id => root.chooseGamingPreset(id)
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.space(1)
                    visible: root.showsBuiltIn
                    spacing: Theme.space(2)

                    ExportGroupLabel {
                        Layout.fillWidth: true
                        text: qsTr("General")
                        hue: gaming.hueOf("")
                        count: root.builtInPresets.length
                    }
                    GridLayout {
                        objectName: "exportBuiltInPresets"
                        Layout.fillWidth: true
                        columns: page.tileColumns
                        columnSpacing: Theme.space(2)
                        rowSpacing: Theme.space(2)

                        Repeater {
                            model: root.builtInPresets

                            delegate: PresetCard {
                                id: builtIn
                                required property var modelData
                                // The estimate for the drawer's range (re-read on edits and encoder probes).
                                readonly property var estimateData: ProjectController.revision >= 0 && ExportController.autoEncoders
                                                                    ? GamingExport.estimate(ProjectController.sequenceId, modelData.id,
                                                                                            root.overrides.range !== undefined
                                                                                            ? { range: root.overrides.range } : {})
                                                                    : ({})
                                objectName: "exportPreset_" + modelData.id
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredWidth: 1 // equal columns
                                text: modelData.name
                                description: modelData.description
                                iconName: modelData.icon
                                hue: gaming.hueOf("")
                                selected: root.gamingPreset === "" && root.presetId === modelData.id
                                estimateIsError: builtIn.estimateData.ok === false
                                sizeText: builtIn.estimateData.ok === true ? builtIn.estimateData.sizeText : ""
                                encodeSeconds: builtIn.estimateData.ok === true ? builtIn.estimateData.encodeSeconds : -1
                                estimate: builtIn.estimateData.ok === true
                                          ? qsTr("≈ %1 · %2").arg(builtIn.estimateData.sizeText).arg(builtIn.estimateData.encodeText)
                                          : ""
                                onClicked: root.choosePreset(modelData.id)
                            }
                        }
                    }
                }

                // 0.2: My presets (Save / Import Preset) and Premiere's other formats.
                ExportFormatPresets {
                    objectName: "exportFormatPresets"
                    Layout.fillWidth: true
                    visible: root.showsBuiltIn
                    selected: root.gamingPreset === "" ? root.presetId : ""
                    onChosen: id => root.choosePreset(id)
                    onManageRequested: presetManager.openWith(root.effectivePresetId, root.effectiveOverrides)
                }
            }
        }

        // ---- What it makes (wide slots: the right column; narrow ones: under the tiles) ----
        Flickable {
            id: sideFlick
            visible: page.wide
            x: page.width - page.sideWidth
            y: 0
            width: page.sideWidth
            height: page.height - dock.height - page.gap
            contentWidth: width
            contentHeight: sideColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}

            ColumnLayout {
                id: sideColumn
                width: sideFlick.width
                spacing: Theme.space(3)
            }
        }

        ColumnLayout {
            id: details
            objectName: "exportDetails"
            parent: page.wide ? sideColumn : column
            Layout.fillWidth: true
            spacing: Theme.space(3)

            // The chosen preset: what it makes, how big and how long (M5o's estimate), what to know.
            Rectangle {
                objectName: "exportChosenCard"
                Layout.fillWidth: true
                implicitHeight: chosenColumn.implicitHeight + Theme.space(8)
                radius: Theme.radiusCard
                color: Theme.section

                ColumnLayout {
                    id: chosenColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.space(4)
                    spacing: Theme.space(3)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(3)

                        HueGlyph {
                            iconName: root.chosen.icon
                            hue: root.chosen.hue
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space(0.5)

                            Text {
                                objectName: "exportChosenName"
                                Layout.fillWidth: true
                                text: root.chosen.name
                                elide: Text.ElideRight
                                color: Theme.textPrimary
                                font.family: Theme.fontDisplay
                                font.pixelSize: Theme.textTitle
                                font.weight: Theme.weightTitle
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.summary.format ?? root.chosen.description
                                elide: Text.ElideRight
                                color: Theme.textSecondary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textXs
                            }
                        }
                    }

                    // M5o: the estimate - the file size and the export time - and what to know about this export.
                    ColumnLayout {
                        objectName: "exportEstimate"
                        Layout.fillWidth: true
                        visible: root.estimate.ok === true
                        spacing: Theme.space(2)
                        Accessible.role: Accessible.StaticText
                        Accessible.name: estimateLine.text

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space(2)

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                implicitHeight: sizeStat.implicitHeight + Theme.space(5)
                                radius: Theme.radiusField
                                color: Theme.raised

                                Stat {
                                    id: sizeStat
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: Theme.space(3)
                                    anchors.rightMargin: Theme.space(3)
                                    label: qsTr("File size")
                                    value: root.estimate.ok === true ? qsTr("≈ %1").arg(root.estimate.sizeText) : ""
                                }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                implicitHeight: timeStat.implicitHeight + Theme.space(5)
                                radius: Theme.radiusField
                                color: Theme.raised

                                Stat {
                                    id: timeStat
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: Theme.space(3)
                                    anchors.rightMargin: Theme.space(3)
                                    label: qsTr("Export time")
                                    value: root.estimate.ok === true ? root.estimate.encodeText : ""
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space(2)
                            Icon {
                                Layout.alignment: Qt.AlignTop
                                name: "gauge"
                                size: Theme.iconSm
                                color: Theme.textTertiary
                            }
                            Text {
                                id: estimateLine
                                objectName: "exportEstimateLine"
                                Layout.fillWidth: true
                                text: root.estimate.ok === true
                                      ? qsTr("About %1 · %2 to export with %3")
                                            .arg(root.estimate.sizeText).arg(root.estimate.encodeText)
                                            .arg(root.estimate.encoderText)
                                      : ""
                                wrapMode: Text.WordWrap
                                color: Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                                font.weight: Theme.weightBody
                            }
                        }
                        Repeater {
                            objectName: "exportEstimateNotes"
                            model: root.estimateNotes
                            delegate: RowLayout {
                                id: note
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Theme.space(2)

                                Rectangle {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.topMargin: Theme.space(1)
                                    Layout.leftMargin: (Theme.iconSm - Theme.space(1.5)) / 2
                                    Layout.rightMargin: (Theme.iconSm - Theme.space(1.5)) / 2
                                    Layout.preferredWidth: Theme.space(1.5)
                                    Layout.preferredHeight: Theme.space(1.5)
                                    radius: Theme.space(0.75)
                                    color: Theme.textTertiary
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: note.modelData
                                    wrapMode: Text.WordWrap
                                    color: Theme.textTertiary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textXs
                                }
                            }
                        }
                    }
                }
            }

            ExportSettingsDrawer {
                objectName: "exportSettingsDrawer"
                Layout.fillWidth: true
                summary: root.summary
                overrides: root.overrides
                onOverrideChanged: (key, value) => {
                    if (key === "range")
                        root.chooseRange(value)
                    else
                        root.setOverride(key, value)
                }
            }

            // 0.2: Effects (overlays, Lumetri Look/LUT, Video Limiter, Loudness Normalization), Metadata, render options.
            ExportEffectsSection {
                objectName: "exportEffectsSection"
                Layout.fillWidth: true
                summary: root.summary
                overrides: root.overrides
                onOverrideChanged: (key, value) => root.setOverride(key, value)
            }

            // Where the file goes.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: saveColumn.implicitHeight + Theme.space(7)
                radius: Theme.radiusCard
                color: Theme.section

                ColumnLayout {
                    id: saveColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.space(4)
                    anchors.rightMargin: Theme.space(3)
                    spacing: Theme.space(2)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        SliceMark {
                            Layout.alignment: Qt.AlignVCenter
                            color: Theme.hueBlue
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Save to")
                            color: Theme.textPrimary
                            font.family: Theme.fontDisplay
                            font.pixelSize: Theme.textMd
                            font.weight: Theme.weightLabel
                        }
                        // UX rework: forget the remembered preset, range and folder.
                        PillButton {
                            objectName: "exportResetChoices"
                            compact: true
                            variant: "ghost"
                            visible: root.remembers
                            iconName: "rotate-ccw"
                            text: qsTr("Reset to defaults")
                            tooltip: qsTr("Forget the last preset, range and folder: Match Source, next to the project")
                            onClicked: {
                                // It hides once nothing is remembered: the keyboard focus goes on to Browse… first
                                // (with its ring when the keyboard brought it here).
                                if (activeFocus)
                                    exportBrowse.forceActiveFocus(visualFocus ? Qt.TabFocusReason : Qt.OtherFocusReason)
                                root.resetChoices()
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)

                        TextField {
                            id: outputField
                            objectName: "exportOutputField"
                            Layout.fillWidth: true
                            implicitHeight: Theme.controlHeight
                            leftPadding: Theme.space(4)
                            rightPadding: Theme.space(3)
                            text: root.summary.output ?? ""
                            color: Theme.textPrimary
                            selectionColor: Theme.accentFill
                            selectedTextColor: Theme.accentOnFill
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                            placeholderText: qsTr("Output file")
                            placeholderTextColor: Theme.textTertiary
                            Accessible.name: qsTr("Output file")
                            onEditingFinished: root.outputPath = text
                            background: Rectangle {
                                radius: height / 2
                                color: Theme.raised
                                border.width: outputField.activeFocus ? Theme.focusRingWidth : 0
                                border.color: Theme.focus
                            }
                        }
                        PillButton {
                            id: exportBrowse
                            objectName: "exportBrowse"
                            iconName: "folder-open"
                            text: qsTr("Browse…")
                            tooltip: qsTr("Choose where to save the export")
                            onClicked: saveDialog.open()
                        }
                    }
                }
            }
        }

        // ---- Pinned at the bottom: the chosen preset and Export, then the queue ----
        ColumnLayout {
            id: dock
            x: page.wide ? page.width - page.sideWidth : 0
            width: page.sideWidth
            anchors.bottom: parent.bottom
            spacing: Theme.space(2)

            Text {
                objectName: "exportError"
                Layout.fillWidth: true
                visible: root.errorText !== ""
                text: root.errorText
                wrapMode: Text.WordWrap
                color: Theme.danger
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            Rectangle {
                objectName: "exportFooter"
                Layout.fillWidth: true
                implicitHeight: footerGrid.implicitHeight + Theme.space(4)
                radius: Theme.radiusCard
                color: Theme.section

                GridLayout {
                    id: footerGrid
                    // The chosen preset beside the button; the button alone when the card above shows it (wide) and
                    // under it where the slot is narrow.
                    readonly property bool summaryShown: !page.wide
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: summaryShown ? Theme.space(3) : Theme.space(2)
                    anchors.rightMargin: Theme.space(2)
                    columns: summaryShown && width >= Theme.space(75) ? 2 : 1
                    columnSpacing: Theme.space(3)
                    rowSpacing: Theme.space(2)

                    RowLayout {
                        Layout.fillWidth: true
                        visible: footerGrid.summaryShown
                        spacing: Theme.space(2.5)

                        Rectangle {
                            Layout.preferredWidth: Theme.space(7)
                            Layout.preferredHeight: Theme.space(7)
                            radius: height / 2
                            color: root.chosen.hue

                            Icon {
                                anchors.centerIn: parent
                                name: root.chosen.icon
                                size: Theme.iconSm
                                color: Theme.clipInk
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Text {
                                Layout.fillWidth: true
                                text: root.chosen.name
                                elide: Text.ElideRight
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Theme.weightTitle
                            }
                            Text {
                                objectName: "exportFooterEstimate"
                                Layout.fillWidth: true
                                //: Export panel footer: the estimated file size and export time, e.g. "≈ 104 MB · 1:28 to export"
                                text: root.estimate.ok === true
                                      ? qsTr("≈ %1 · %2 to export").arg(root.estimate.sizeText)
                                                                   .arg(root.clock(root.estimate.encodeSeconds ?? 0))
                                      : (root.summary.format ?? "")
                                elide: Text.ElideRight
                                color: Theme.textSecondary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textXs
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: footerGrid.columns === 1
                        spacing: Theme.space(2)

                        PillButton {
                            objectName: "exportButton"
                            Layout.fillWidth: footerGrid.columns === 1
                            Layout.preferredWidth: footerGrid.columns === 1 ? -1 : Math.max(implicitWidth, Theme.space(34))
                            implicitHeight: Theme.controlHeightLarge
                            variant: "primary"
                            enabled: root.summary.ok === true
                            // The range when it is not the whole sequence (UX rework: In to Out is picked for a
                            // marked sequence, so the button says so).
                            text: ExportController.busy ? qsTr("Add to queue")
                                  : root.range === "inOut" ? qsTr("Export In to Out")
                                  : root.range === "workArea" ? qsTr("Export Work Area")
                                  : root.range === "custom" ? qsTr("Export Range") : qsTr("Export")
                            iconName: "download"
                            tooltip: qsTr("Export media")
                            command: "file.exportMedia"
                            onClicked: root.startExport()
                        }
                        // 0.2: Premiere's Send to Media Encoder: into the queue, waiting for Start Queue.
                        PillButton {
                            objectName: "exportSendToQueue"
                            implicitHeight: Theme.controlHeightLarge
                            variant: "secondary"
                            enabled: root.summary.ok === true
                            text: qsTr("Send to Queue")
                            iconName: "list"
                            tooltip: qsTr("Send to the export queue (Media Encoder) without starting")
                            command: "file.sendToMediaEncoder"
                            onClicked: root.sendToQueue()
                        }
                    }
                }
            }

            // ---- Queue: progress stays in view while the next export is set up ----
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.space(1)
                visible: ExportController.jobs.count > 0

                SectionHeader {
                    Layout.fillWidth: true
                    text: qsTr("Queue")
                }
                PillButton {
                    compact: true
                    variant: "ghost"
                    visible: ExportController.jobs.count > ExportController.jobs.activeCount
                    text: qsTr("Clear")
                    tooltip: qsTr("Remove finished exports from the list")
                    onClicked: ExportController.clearFinished()
                }
                PillButton {
                    objectName: "exportOpenQueue"
                    compact: true
                    variant: "ghost"
                    iconName: "list"
                    text: qsTr("Queue…")
                    tooltip: qsTr("The export queue, watch folders and the CPU limit")
                    onClicked: queueDialog.openOn("queue")
                }
            }

            ListView {
                id: queue
                objectName: "exportQueue"
                Layout.fillWidth: true
                // Up to three jobs in view (fewer in a short slot, so the tiles keep room); more scroll.
                Layout.preferredHeight: Math.min(contentHeight, Theme.space(45), Math.round(page.height * 0.3))
                visible: count > 0
                clip: true
                spacing: Theme.space(2)
                boundsBehavior: Flickable.StopAtBounds
                model: ExportController.jobs
                ScrollBar.vertical: ThinScrollBar {}

                delegate: ExportJobPill {
                    id: job
                    width: queue.width
                    objectName: "exportJob_" + job.jobId
                    onCancelRequested: ExportController.cancel(job.jobId)
                    onRevealRequested: ExportController.reveal(job.jobId)
                    // The notice follows the last Open: gone after it works again (a drive plugged back in).
                    onOpenRequested: job.notice = ExportController.openOutput(job.jobId)
                                     ? "" : qsTr("The file was moved or deleted.")
                    onPauseRequested: ExportController.pause(job.jobId)
                    onResumeRequested: ExportController.resume(job.jobId)
                    onStartRequested: ExportController.start(job.jobId)
                }
            }
        }
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Export to")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Video, audio and image files (*.mp4 *.mov *.mkv *.webm *.gif *.wav *.mp3 *.m4a *.aif *.png *.tif *.dpx *.jpg *.tga *.exr)")]
        // Where the file would go now: the folder exported to last, else next to the project (UX rework).
        currentFolder: ExportController.folderUrlOf(root.summary.output ?? "")
        onAccepted: root.outputPath = selectedFile.toString()
    }

    // 0.2: Wabcut's Media Encoder (the queue, watch folders, the CPU limit) and the preset manager.
    ExportQueueDialog {
        id: queueDialog
    }
    ExportPresetManager {
        id: presetManager
        onPresetSaved: id => root.choosePreset(id)
    }
}
