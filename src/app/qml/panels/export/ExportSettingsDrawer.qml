pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The settings drawer of the Export panel (design language v3: a section card): a header row ("● Settings", the
// resolved format, a round chevron) that opens a body of segmented pill rows on top of the selected preset: container,
// codec, encoder (auto = hardware first, with the encoder auto picks shown), quality, resolution, range, and the
// audio's channels and codec (A.9: as the master, mono, stereo, 5.1; AAC or AC-3). Labels sit beside their rows, or
// above them in a narrow slot. The body grows by height over durCollapse and stays mounted. The rows are controlled:
// they show `summary` (ExportController.describe) and report changes through overrideChanged(key, value)
// (ExportController.queueExport keys). 0.6 (#73): MXF (OP1a or OP-Atom for Avid) with its codecs, PCM tracks and reel
// name, and the Embedded captions rows: 608 / 708, the 608 channel, the 708 service, where they go, how they appear.
Item {
    id: root

    property var summary: ({})
    property var overrides: ({})
    property bool open: false

    signal overrideChanged(string key, var value)

    readonly property var containers: ["mp4", "mov", "mkv", "wav", "mxf"]
    /// 0.6 (#73): MXF OP1a or OP-Atom (summary.container "mxf" / "mxf-opatom").
    readonly property bool isMxf: summary.mxf ?? false
    readonly property bool isOpAtom: (summary.container ?? "") === "mxf-opatom"
    readonly property var codecs: isMxf ? ["dnxhr", "xdcam-hd422", "avc-intra-100"] : ["h264", "hevc", "prores", "dnxhr"]
    readonly property var qualities: [16, 20, 26] // CRF / CQ: high, balanced, small file
    readonly property var sizes: [[0, 0], [1280, 720], [1920, 1080], [3840, 2160]]
    readonly property var ranges: ["whole", "inOut", "workArea", "custom"] // 0.2: Premiere's Range
    readonly property var audioChannels: isMxf ? [0, 2, 4, 8] : [0, 1, 2, 6, 8] // 0 = as the master; MXF: tracks
    readonly property var dynamicRanges: ["auto", "sdr", "pq", "hlg"] // 0.2 (I.9)
    /// 0.7: dynamic HDR metadata on top of a PQ master (override `dynamicMetadata`); the Dolby Vision ones are listed so the
    /// build can say why it cannot write them.
    readonly property var dynamicMetadataKinds: ["none", "hdr10plus", "dolbyVision81", "dolbyVision84"]
    /// 0.6 (#72): the SDR output colour spaces (override `outputSpace`; Auto = the preset's, else the sequence's Color
    /// Management choice). Rec.2100 PQ / HLG are the Dynamic range row's.
    readonly property var outputSpaces: ["auto", "rec709", "rec2020", "p3-d65"]
    readonly property bool hasVideo: !(summary.audioOnly ?? (summary.container === "wav"))
    readonly property bool isImage: summary.isImage ?? false // 0.2: stills and image sequences
    readonly property bool longGop: hasVideo && (summary.videoCodec === "h264" || summary.videoCodec === "hevc")
    /// P1 (#28): the codecs a size limit works with (H.264, HEVC, AV1, VP9), and the chips (MB; 0 = off).
    readonly property bool sizeLimitAvailable: hasVideo && !isImage && (longGop || summary.videoCodec === "av1"
                                                                       || summary.videoCodec === "vp9")
    readonly property var sizeLimits: [0, 8, 25, 100, 500]
    /// The size limit this export has (overrides.targetMB; 0 = off).
    readonly property real sizeLimit: Number(overrides.targetMB ?? 0)
    readonly property var credentialModes: ["off", "ai", "always"]
    /// 0.6 (#52): "By track role" stems are on (overrides.stems "role").
    readonly property bool stemsOn: (overrides.stems ?? "off") === "role"
    /// 0.6 (#73): Embedded captions (also while the format refuses them: the rows stay to fix it).
    readonly property bool embedding: hasVideo && (summary.captionMode ?? overrides.captions ?? "") === "embed"
    readonly property var captionStandards: ["608+708", "608", "708"]
    readonly property string captionStandard: overrides.captionStandard ?? summary.captionStandard ?? "608+708"
    readonly property var captionCarriages: ["auto", "video", "track", "both"]
    readonly property var captionDisplays: ["pop-on", "roll-up-3", "paint-on"]
    // AI-15: Content Credentials go into MP4 and MOV files, where this build can sign.
    readonly property bool signable: ExportController.contentCredentialsAvailable
                                     && ((summary.container ?? "mp4") === "mp4" || summary.container === "mov")

    /// Narrow (a side slot): each row's label goes above its control instead of beside it.
    readonly property bool stacked: width < Theme.space(105)

    implicitHeight: header.height + body.height
    clip: true

    // The pill rows are uncontrolled (they move their own indicator on click), so the summary is pushed into
    // them whenever it changes.
    function sync() {
        const container = summary.container ?? "mp4"
        containerTabs.currentIndex = containers.indexOf(container === "mxf-opatom" ? "mxf" : container) // -1: a format preset's (0.2)
        patternTabs.currentIndex = isOpAtom ? 1 : 0
        codecTabs.currentIndex = codecs.indexOf(summary.videoCodec ?? "h264")
        encoderTabs.currentIndex = (overrides.encoder ?? "auto") === "auto" ? 0 : 1
        const quality = overrides.quality ?? 16
        qualityTabs.currentIndex = Math.max(0, qualities.indexOf(quality))
        const width = overrides.width ?? 0
        sizeTabs.currentIndex = Math.max(0, sizes.findIndex(s => s[0] === width))
        rangeTabs.currentIndex = Math.max(0, ranges.indexOf(summary.rangeSetting ?? overrides.range ?? "whole"))
        channelTabs.currentIndex = Math.max(0, audioChannels.indexOf(summary.audioChannels ?? 2))
        audioCodecTabs.currentIndex = summary.audioCodec === "ac3" ? 1 : 0
        pcmTabs.currentIndex = summary.audioCodec === "pcm_s16le" ? 1 : 0
        layoutTabs.currentIndex = (summary.mxfAudioLayout ?? "mono") === "multichannel" ? 1 : 0
        captionStandardTabs.currentIndex = Math.max(0, captionStandards.indexOf(captionStandard))
        cc608Tabs.currentIndex = Math.max(0, Number(overrides.cc608Channel ?? summary.cc608Channel ?? 1) - 1)
        carriageTabs.currentIndex = Math.max(0, captionCarriages.indexOf(overrides.captionCarriage ?? summary.captionCarriage ?? "auto"))
        const display = Number(summary.captionDisplay ?? 0)
        captionDisplayTabs.currentIndex = display === 0 ? 0 : display === 4 ? 2 : 1
        dynamicRangeTabs.currentIndex = Math.max(0, dynamicRanges.indexOf(summary.dynamicRangeSetting ?? "auto"))
        dynamicMetadataTabs.currentIndex = Math.max(0, dynamicMetadataKinds.indexOf(summary.dynamicMetadataSetting ?? "none"))
        outputSpaceTabs.currentIndex = Math.max(0, outputSpaces.indexOf(overrides.outputSpace ?? "auto"))
        credentialsTabs.currentIndex = Math.max(0, credentialModes.indexOf(ExportController.contentCredentials))
        stemsTabs.currentIndex = stemsOn ? 1 : 0
        syncSizeLimit()
    }
    // (Read from `overrides` itself: `sizeLimit` may not have followed it yet in this handler.)
    onOverridesChanged: {
        syncSizeLimit()
        stemsTabs.currentIndex = (overrides.stems ?? "off") === "role" ? 1 : 0
    }
    function syncSizeLimit() {
        sizeLimitTabs.currentIndex = sizeLimits.indexOf(Number(overrides.targetMB ?? 0)) // -1: a custom size (the field)
    }
    /// P1 (#28): a size limit typed into the custom field (MB, 1-4000); remembered for the field.
    function applyCustomSizeLimit(text: string): bool {
        const value = Number(text.replace(",", ".").replace(/\s*MB\s*$/i, ""))
        if (text.trim() === "" || !isFinite(value) || value < 1 || value > 4000)
            return false
        const megabytes = Math.round(value * 10) / 10
        ExportController.lastTargetMB = megabytes
        root.overrideChanged("targetMB", megabytes)
        return true
    }
    onSummaryChanged: sync()
    // 0.6 (#73): the codec and channel pills change their choices with the format.
    onCodecsChanged: Qt.callLater(root.sync)
    onAudioChannelsChanged: Qt.callLater(root.sync)
    Component.onCompleted: sync()

    ContentCredentialsSettings {
        id: credentialsSettings
    }

    // The card: a section on the panel (design language v3), header and body together.
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusCard
        color: Theme.section
    }

    // ---- Header ----
    Rectangle {
        id: header
        width: root.width
        height: Theme.controlHeightLarge + Theme.space(2)
        radius: Theme.radiusCard
        color: headerArea.containsMouse ? Theme.hover : "transparent"
        activeFocusOnTab: true

        Accessible.role: Accessible.Button
        Accessible.name: root.open ? qsTr("Hide export settings") : qsTr("Show export settings")
        Keys.onSpacePressed: root.open = !root.open
        Keys.onReturnPressed: root.open = !root.open

        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.space(4)
            anchors.rightMargin: Theme.space(2.5)
            spacing: Theme.space(2)

            SliceMark {
                Layout.alignment: Qt.AlignVCenter
                color: Theme.accentStroke
            }
            Text {
                text: qsTr("Settings")
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.textMd
                font.weight: Theme.weightLabel
            }
            // The resolved format, e.g. "MP4 · H.264 · 1920 × 1080 · 60 fps".
            Text {
                Layout.fillWidth: true
                text: root.summary.format ?? ""
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignRight
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
            Rectangle {
                Layout.preferredWidth: Theme.space(7)
                Layout.preferredHeight: Theme.space(7)
                radius: height / 2
                color: root.open ? Theme.well : Theme.raised

                Icon {
                    anchors.centerIn: parent
                    name: "chevron-down"
                    size: Theme.iconSm
                    color: Theme.textPrimary
                    rotation: root.open ? 180 : 0
                    Behavior on rotation { NumberAnimation { duration: Theme.durState; easing.type: Easing.OutCubic } }
                }
            }
        }

        MouseArea {
            id: headerArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.open = !root.open
        }

        FocusRing {
            targetRadius: header.radius
            visible: header.activeFocus
        }
    }

    // ---- Body ----
    Item {
        id: body
        anchors.top: header.bottom
        width: root.width
        height: root.open ? rows.implicitHeight + Theme.space(5) : 0
        enabled: root.open
        opacity: root.open ? 1 : 0
        Behavior on height { NumberAnimation { duration: Theme.durCollapse; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft } }
        Behavior on opacity { NumberAnimation { duration: Theme.durState } }

        // A hairline under the header.
        Rectangle {
            x: Theme.space(4)
            width: parent.width - Theme.space(8)
            height: Theme.hairline
            color: Theme.border
        }

        GridLayout {
            id: rows
            x: Theme.space(4)
            y: Theme.space(3)
            width: parent.width - Theme.space(8)
            columns: root.stacked ? 1 : 2
            columnSpacing: Theme.space(3)
            rowSpacing: root.stacked ? Theme.space(1.5) : Theme.space(2)

            component RowLabel: Text {
                Layout.preferredWidth: root.stacked ? -1 : Theme.propLabelWidth + Theme.space(4)
                Layout.fillWidth: root.stacked
                Layout.topMargin: root.stacked ? Theme.space(1.5) : 0
                color: root.stacked ? Theme.textTertiary : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: root.stacked ? Theme.textXs : Theme.textSm
                font.weight: root.stacked ? Theme.weightLabel : Theme.weightBody
                elide: Text.ElideRight
            }

            RowLabel { text: qsTr("Format") }
            PillTabs {
                id: containerTabs
                Layout.fillWidth: true
                compact: true
                model: [qsTr("MP4"), qsTr("MOV"), qsTr("MKV"), qsTr("WAV"), qsTr("MXF")]
                Accessible.name: qsTr("Container")
                onActivated: index => {
                    const container = root.containers[index]
                    root.overrideChanged("container", container)
                    // MP4 carries H.264/HEVC only: leave ProRes/DNxHR for H.264.
                    if (container === "mp4" && !root.longGop && root.hasVideo)
                        root.overrideChanged("videoCodec", "h264")
                }
            }

            // 0.6 (#73): MXF's operational pattern - OP1a, one file for broadcast delivery, or OP-Atom, Avid MediaFiles
            // (a file for the picture and one per audio channel).
            RowLabel { text: qsTr("MXF"); visible: root.isMxf }
            PillTabs {
                id: patternTabs
                objectName: "exportMxfPatternTabs"
                visible: root.isMxf
                Layout.fillWidth: true
                compact: true
                model: [qsTr("OP1a"), qsTr("OP-Atom (Avid)")]
                Accessible.name: qsTr("MXF operational pattern")
                onActivated: index => root.overrideChanged("container", index === 0 ? "mxf" : "mxf-opatom")
            }

            RowLabel { text: qsTr("Codec"); visible: root.hasVideo && !root.isImage && !root.isOpAtom }
            PillTabs {
                id: codecTabs
                objectName: "exportCodecTabs"
                visible: root.hasVideo && !root.isImage && !root.isOpAtom
                Layout.fillWidth: true
                compact: true
                model: root.isMxf ? [qsTr("DNxHD / DNxHR"), qsTr("XDCAM HD422"), qsTr("AVC-Intra 100")]
                                  : [qsTr("H.264"), qsTr("HEVC"), qsTr("ProRes"), qsTr("DNxHR")]
                Accessible.name: qsTr("Video codec")
                onActivated: index => {
                    const codec = root.codecs[index]
                    // ProRes and DNxHR masters go into QuickTime.
                    if ((codec === "prores" || codec === "dnxhr") && root.summary.container === "mp4")
                        root.overrideChanged("container", "mov")
                    // 0.6 (#73): XDCAM HD422 and AVC-Intra 100 are HD rasters: 1080p unless a size is chosen.
                    if ((codec === "xdcam-hd422" || codec === "avc-intra-100") && root.overrides.width === undefined) {
                        root.overrideChanged("width", 1920)
                        root.overrideChanged("height", 1080)
                    }
                    root.overrideChanged("videoCodec", codec)
                }
            }

            RowLabel { text: qsTr("Encoder"); visible: root.hasVideo }
            RowLayout {
                visible: root.hasVideo
                Layout.fillWidth: true
                spacing: Theme.space(2)

                PillTabs {
                    id: encoderTabs
                    Layout.preferredWidth: Theme.space(40)
                    compact: true
                    model: [qsTr("Auto"), qsTr("Software")]
                    Accessible.name: qsTr("Encoder")
                    onActivated: index => root.overrideChanged("encoder", index === 0 ? undefined : "software")
                }
                // The encoder this export uses: a dot and its name, elided in a narrow slot.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.controlHeightSmall
                    radius: height / 2
                    color: Theme.raised
                    Accessible.role: Accessible.StaticText
                    Accessible.name: encoderName.text
                    Accessible.description: qsTr("The encoder this export uses: hardware first, software when none works")

                    Rectangle {
                        id: encoderDot
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.space(3)
                        anchors.verticalCenter: parent.verticalCenter
                        width: Theme.dotSize
                        height: Theme.dotSize
                        radius: width / 2
                        color: encoderTabs.currentIndex === 0 ? Theme.success : Theme.textTertiary
                    }
                    Text {
                        id: encoderName
                        anchors.left: encoderDot.right
                        anchors.leftMargin: Theme.space(2)
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.space(3)
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.summary.encoder ?? ""
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                    HoverHandler {
                        id: encoderHover
                    }
                    Tooltip {
                        visible: encoderHover.hovered
                        text: qsTr("The encoder this export uses: hardware first, software when none works")
                    }
                }
            }

            RowLabel { text: qsTr("Quality"); visible: root.longGop }
            PillTabs {
                id: qualityTabs
                objectName: "exportQualityTabs"
                visible: root.longGop
                // P1 (#28): a size limit decides the bitrate: the quality choice waits until it is off.
                enabled: root.sizeLimit === 0
                opacity: enabled ? 1 : 0.45
                Layout.fillWidth: true
                compact: true
                model: [qsTr("High"), qsTr("Balanced"), qsTr("Small")]
                Accessible.name: qsTr("Quality")
                Accessible.description: enabled ? "" : qsTr("Set by the size limit")
                onActivated: index => root.overrideChanged("quality", root.qualities[index])
            }

            // P1 (#28): a size limit for any export (WhatsApp, email, X, Nitro): the whole file at most that many MB
            // (decimal, as upload limits are stated); two passes when the first ends over. Off = the preset's own.
            RowLabel { text: qsTr("Size limit"); visible: root.sizeLimitAvailable }
            RowLayout {
                visible: root.sizeLimitAvailable
                Layout.fillWidth: true
                spacing: Theme.space(2)

                PillTabs {
                    id: sizeLimitTabs
                    objectName: "exportSizeLimitTabs"
                    Layout.fillWidth: true
                    compact: true
                    currentIndex: 0
                    model: [qsTr("Off"), qsTr("8 MB"), qsTr("25 MB"), qsTr("100 MB"), qsTr("500 MB")]
                    Accessible.name: qsTr("Size limit")
                    onActivated: index => root.overrideChanged("targetMB", index === 0 ? undefined : root.sizeLimits[index])
                }
                FieldInput {
                    id: sizeLimitField
                    objectName: "exportSizeLimitCustom"
                    Layout.preferredWidth: Theme.space(24)
                    // A custom limit shows here; otherwise the last one typed, to use again with Enter.
                    text: root.sizeLimit > 0 && root.sizeLimits.indexOf(root.sizeLimit) < 0 ? String(root.sizeLimit)
                          : ExportController.lastTargetMB > 0 ? String(ExportController.lastTargetMB) : ""
                    placeholderText: qsTr("MB")
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                    Accessible.name: qsTr("Custom size limit in MB")
                    onAccepted: root.applyCustomSizeLimit(text)
                    HoverHandler {
                        id: sizeFieldHover
                    }
                    Tooltip {
                        visible: sizeFieldHover.hovered && !sizeLimitField.activeFocus
                        text: qsTr("A size of your own, 1 to 4000 MB: type it and press Enter")
                    }
                }
            }

            RowLabel { text: qsTr("Size"); visible: root.hasVideo }
            PillTabs {
                id: sizeTabs
                visible: root.hasVideo
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Preset"), qsTr("720p"), qsTr("1080p"), qsTr("4K")]
                Accessible.name: qsTr("Frame size")
                onActivated: index => {
                    root.overrideChanged("width", index === 0 ? undefined : root.sizes[index][0])
                    root.overrideChanged("height", index === 0 ? undefined : root.sizes[index][1])
                }
            }

            // 0.2 (I.9): an HDR master (Rec.2100 PQ with HDR10 metadata, or HLG) of an HDR or Wide Gamut sequence, on
            // 10-bit HEVC, AV1 or ProRes; Auto follows the sequence's colour setup.
            RowLabel { text: qsTr("Dynamic range"); visible: root.hasVideo && (root.summary.hdrAvailable ?? false) }
            PillTabs {
                id: dynamicRangeTabs
                objectName: "exportDynamicRangeTabs"
                visible: root.hasVideo && (root.summary.hdrAvailable ?? false)
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Auto"), qsTr("SDR"), qsTr("HDR PQ"), qsTr("HDR HLG")]
                Accessible.name: qsTr("Dynamic range")
                onActivated: index => {
                    // 0.7: dynamic metadata describes an HDR master; an SDR (or the wrong HDR) master has none. Read what was chosen
                    // before the change: the summary follows the overrides at once (and has none while they do not resolve).
                    const kind = root.overrides.dynamicMetadata ?? "none"
                    root.overrideChanged("dynamicRange", index === 0 ? undefined : root.dynamicRanges[index])
                    if (kind !== "none" && (index === 1 || (index === 3 && kind !== "dolbyVision84") || (index === 2 && kind === "dolbyVision84")))
                        root.overrideChanged("dynamicMetadata", undefined)
                }
            }

            // 0.7: dynamic HDR metadata. HDR10+ is measured from the picture in a first pass and written shot by shot into a
            // PQ HEVC file (MP4, MOV, MKV). Dolby Vision is licensed by Dolby: a build without a licensed writer lists it
            // greyed out and says why under the row.
            RowLabel { text: qsTr("HDR metadata"); visible: dynamicMetadataColumn.visible }
            ColumnLayout {
                id: dynamicMetadataColumn
                objectName: "exportDynamicMetadata"
                visible: root.hasVideo && (root.summary.hdrAvailable ?? false) && root.longGop
                Layout.fillWidth: true
                spacing: Theme.space(1.5)

                PillTabs {
                    id: dynamicMetadataTabs
                    objectName: "exportDynamicMetadataTabs"
                    Layout.fillWidth: true
                    compact: true
                    model: [
                        { "label": qsTr("None") },
                        { "label": qsTr("HDR10+"), "enabled": root.summary.hdr10PlusAvailable ?? false,
                          "tooltip": root.summary.hdr10PlusReason ?? "" },
                        { "label": qsTr("Dolby Vision 8.1"), "enabled": (root.summary.dolbyVisionReason ?? "") === "",
                          "tooltip": root.summary.dolbyVisionReason ?? "" },
                        { "label": qsTr("Dolby Vision 8.4"), "enabled": (root.summary.dolbyVisionReason ?? "") === "",
                          "tooltip": root.summary.dolbyVisionReason ?? "" }
                    ]
                    Accessible.name: qsTr("Dynamic metadata")
                    onActivated: index => root.overrideChanged("dynamicMetadata", index === 0 ? undefined : root.dynamicMetadataKinds[index])
                }
                Text {
                    objectName: "exportDynamicMetadataNote"
                    Layout.fillWidth: true
                    text: root.summary.dynamicMetadataNote ?? ""
                    visible: text !== ""
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }

            // 0.6 (#72): the output colour space of an SDR file (Premiere's Output Color Space): Rec.709, Rec.2020 (SDR)
            // or P3-D65; the details line below shows the colour chain it makes.
            RowLabel { text: qsTr("Color space"); visible: root.hasVideo && !root.isImage }
            PillTabs {
                id: outputSpaceTabs
                objectName: "exportOutputSpaceTabs"
                visible: root.hasVideo && !root.isImage
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Auto"), qsTr("Rec.709"), qsTr("Rec.2020"), qsTr("P3-D65")]
                Accessible.name: qsTr("Output color space")
                onActivated: index => root.overrideChanged("outputSpace", index === 0 ? undefined : root.outputSpaces[index])
            }

            // 0.2: Premiere's Include Alpha Channel (ProRes 4444, QuickTime Animation, CineForm, PNG, TIFF, Targa, OpenEXR)
            // and Export As Sequence (stills).
            RowLabel { text: qsTr("Alpha"); visible: root.hasVideo && (root.summary.alphaAvailable ?? false) }
            Toggle {
                objectName: "exportIncludeAlpha"
                visible: root.hasVideo && (root.summary.alphaAvailable ?? false)
                text: qsTr("Include Alpha Channel")
                checked: root.summary.alpha ?? false
                onToggled: root.overrideChanged("alpha", checked)
            }
            RowLabel { text: qsTr("Stills"); visible: root.isImage }
            Toggle {
                objectName: "exportAsSequence"
                visible: root.isImage
                text: qsTr("Export As Sequence")
                checked: root.summary.exportAsSequence ?? true
                onToggled: root.overrideChanged("exportAsSequence", checked)
            }

            RowLabel { text: qsTr("Range") }
            PillTabs {
                id: rangeTabs
                objectName: "exportRangeTabs"
                Layout.fillWidth: true
                compact: true
                // 0.7 (card 024): the Work Area range is there once the sequence has a work area (Alt+[ / Alt+] set it), as in
                // Quick Export's range chips.
                model: [qsTr("Entire Source"), qsTr("Source In/Out"),
                        { label: qsTr("Work Area"), enabled: (root.summary.rangeAvailable ?? {}).workArea === true,
                          tooltip: qsTr("Set a work area with Alt+[ and Alt+]") },
                        qsTr("Custom")]
                Accessible.name: qsTr("Range")
                onActivated: index => {
                    if (root.ranges[index] === "custom" && root.overrides.rangeIn === undefined) {
                        root.overrideChanged("rangeIn", root.summary.rangeIn)
                        root.overrideChanged("rangeOut", root.summary.rangeOut)
                    }
                    root.overrideChanged("range", index === 0 ? undefined : root.ranges[index])
                }
            }
            RowLabel { text: qsTr("In / Out"); visible: rangeTabs.currentIndex === 3 }
            RowLayout {
                visible: rangeTabs.currentIndex === 3
                Layout.fillWidth: true
                spacing: Theme.space(2)
                FieldInput {
                    objectName: "exportRangeIn"
                    Layout.fillWidth: true
                    text: root.overrides.rangeIn ?? root.summary.rangeIn ?? ""
                    placeholderText: qsTr("In")
                    Accessible.name: qsTr("Custom range In")
                    onEditingFinished: root.overrideChanged("rangeIn", text)
                }
                FieldInput {
                    objectName: "exportRangeOut"
                    Layout.fillWidth: true
                    text: root.overrides.rangeOut ?? root.summary.rangeOut ?? ""
                    placeholderText: qsTr("Out")
                    Accessible.name: qsTr("Custom range Out")
                    onEditingFinished: root.overrideChanged("rangeOut", text)
                }
            }
            // P1 (#26, 2C): each marker range or each selected clip as its own file (the Export Ranges dialog).
            RowLabel { text: qsTr("Separate files") }
            Row {
                objectName: "exportRangeBatches"
                spacing: Theme.space(2)
                PillButton {
                    objectName: "exportRangeEachMarker"
                    variant: "ghost"
                    compact: true
                    text: qsTr("Each marker…")
                    tooltip: qsTr("One file per marker with a duration, named after it")
                    enabled: ProjectController.markers.length >= 0 && ExportController.markerRanges("", -1).length > 0
                    disabledReason: qsTr("No marker has a duration")
                    onClicked: ExportController.rangesExportRequested("markers")
                }
                PillButton {
                    objectName: "exportRangeEachClip"
                    variant: "ghost"
                    compact: true
                    text: qsTr("Each selected clip…")
                    tooltip: qsTr("One file per selected clip (linked video and audio as one), named after it")
                    enabled: ProjectController.selectedClips.length > 0 && ExportController.selectedClipRanges().length > 0
                    disabledReason: qsTr("Select clips in the timeline first")
                    onClicked: ExportController.rangesExportRequested("clips")
                }
            }

            // A.9: the audio's channels (as the master: 5.1 for a 5.1 master when the codec takes it) and codec.
            RowLabel { text: qsTr("Audio channels") }
            PillTabs {
                id: channelTabs
                objectName: "exportAudioChannelsTabs"
                Layout.fillWidth: true
                compact: true
                model: root.isMxf ? [qsTr("As the master"), qsTr("2 tracks"), qsTr("4 tracks"), qsTr("8 tracks")]
                                  : [qsTr("As the master"), qsTr("Mono"), qsTr("Stereo"), qsTr("5.1"), qsTr("7.1")]
                Accessible.name: qsTr("Audio channels")
                onActivated: index => root.overrideChanged("audioChannels", root.audioChannels[index])
            }
            // 0.6 (#73): MXF audio - 24 or 16-bit PCM, one mono track per channel or one multichannel track (OP1a), and
            // the reel (tape) name Avid shows in its Tape column.
            RowLabel { text: qsTr("Audio"); visible: root.isMxf }
            PillTabs {
                id: pcmTabs
                objectName: "exportMxfPcmTabs"
                visible: root.isMxf
                Layout.fillWidth: true
                compact: true
                model: [qsTr("PCM 24-bit"), qsTr("PCM 16-bit")]
                Accessible.name: qsTr("MXF audio sample size")
                onActivated: index => root.overrideChanged("audioCodec", index === 0 ? "pcm24" : "pcm16")
            }
            RowLabel { text: qsTr("Audio tracks"); visible: root.isMxf && !root.isOpAtom }
            PillTabs {
                id: layoutTabs
                objectName: "exportMxfLayoutTabs"
                visible: root.isMxf && !root.isOpAtom
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Mono tracks"), qsTr("One multichannel track")]
                Accessible.name: qsTr("MXF audio track layout")
                onActivated: index => root.overrideChanged("mxfAudioLayout", index === 0 ? "mono" : "multichannel")
            }
            RowLabel { text: qsTr("Reel"); visible: root.isMxf }
            FieldInput {
                objectName: "exportMxfReel"
                visible: root.isMxf
                Layout.fillWidth: true
                text: root.overrides.reelName ?? root.summary.reelName ?? ""
                placeholderText: qsTr("Tape or reel name (optional)")
                maximumLength: 64
                Accessible.name: qsTr("Reel name")
                onEditingFinished: root.overrideChanged("reelName", text.trim() === "" ? undefined : text.trim())
            }

            RowLabel { text: qsTr("Audio codec"); visible: root.hasVideo && !root.isMxf }
            PillTabs {
                id: audioCodecTabs
                objectName: "exportAudioCodecTabs"
                visible: root.hasVideo && !root.isMxf
                Layout.fillWidth: true
                compact: true
                model: [qsTr("AAC"), qsTr("AC-3")]
                Accessible.name: qsTr("Audio codec")
                onActivated: index => root.overrideChanged("audioCodec", index === 0 ? undefined : "ac3")
            }
            // 0.6 (#52): dialogue / music / effects stems with the master, by the audio tracks' roles: one 24-bit WAV per
            // role next to the export ("_DX", "_MX", "_FX", "_OTHER"), queued after it. The line says which.
            RowLabel { text: qsTr("Audio stems"); visible: !root.isImage }
            PillTabs {
                id: stemsTabs
                objectName: "exportStemsTabs"
                visible: !root.isImage
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Off"), qsTr("By track role")]
                Accessible.name: qsTr("Audio stems")
                Accessible.description: qsTr("Also write the dialogue, music and effects as WAV files next to the export")
                onActivated: index => root.overrideChanged("stems", index === 0 ? undefined : "role")
            }
            Item {
                visible: root.stemsOn && !root.stacked
                implicitWidth: 1
                implicitHeight: 1
            }
            Text {
                objectName: "exportStemsPlan"
                visible: root.stemsOn
                Layout.fillWidth: true
                text: root.summary.stemsText ?? ""
                wrapMode: Text.WordWrap
                color: (root.summary.stems ?? []).length > 0 ? Theme.textSecondary : Theme.warningText
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            Item {
                visible: root.stemsOn && (root.summary.stems ?? []).length > 0 && !root.stacked
                implicitWidth: 1
                implicitHeight: 1
            }
            Toggle {
                objectName: "exportStemsWithoutMaster"
                visible: root.stemsOn && (root.summary.stems ?? []).length > 0
                text: qsTr("Stems without master effects")
                checked: root.overrides.stemsMasterEffects === false
                onToggled: root.overrideChanged("stemsMasterEffects", checked ? false : undefined)
            }

            // I.4: captions burned into the picture (as the monitor shows them) or as a sidecar file next to it; P.2:
            // Premiere's sidecar formats, and CEA-608 embedded in H.264 / HEVC.
            RowLabel { text: qsTr("Captions"); visible: root.hasVideo }
            CaptionExportChooser {
                visible: root.hasVideo
                Layout.fillWidth: true
                mode: root.summary.captionMode ?? root.overrides.captions ?? "none"
                onChosen: value => {
                    // 0.6 (#73): a format that can carry only 608 (QuickTime's c608 track) starts with 608 alone.
                    if (value === "embed" && root.overrides.captionStandard === undefined && !(root.summary.mxf ?? false)
                            && !(root.summary.captionInVideoAvailable ?? false))
                        root.overrideChanged("captionStandard", "608")
                    root.overrideChanged("captions", value)
                }
            }
            // 0.6 (#73): Embedded captions - CEA-608 (CC1-CC4) and / or CEA-708 (services 1-63), in the picture stream
            // and / or a caption track (MXF: SMPTE 436M; QuickTime: c608), pop-on, roll-up or paint-on.
            RowLabel { text: qsTr("Standard"); visible: root.embedding }
            PillTabs {
                id: captionStandardTabs
                objectName: "exportCaptionStandardTabs"
                visible: root.embedding
                Layout.fillWidth: true
                compact: true
                model: [qsTr("608 + 708"), qsTr("608"), qsTr("708")]
                Accessible.name: qsTr("Closed caption standard")
                onActivated: index => root.overrideChanged("captionStandard", root.captionStandards[index])
            }
            RowLabel { text: qsTr("608 channel"); visible: root.embedding && root.captionStandard !== "708" }
            PillTabs {
                id: cc608Tabs
                objectName: "exportCc608Tabs"
                visible: root.embedding && root.captionStandard !== "708"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("CC1"), qsTr("CC2"), qsTr("CC3"), qsTr("CC4")]
                Accessible.name: qsTr("608 caption channel")
                onActivated: index => root.overrideChanged("cc608Channel", index + 1)
            }
            RowLabel { text: qsTr("708 service"); visible: root.embedding && root.captionStandard !== "608" }
            RowLayout {
                visible: root.embedding && root.captionStandard !== "608"
                Layout.fillWidth: true
                spacing: Theme.space(2)
                FieldInput {
                    id: serviceField
                    objectName: "exportCc708Service"
                    Layout.preferredWidth: Theme.space(16)
                    text: String(root.overrides.cc708Service ?? root.summary.cc708Service ?? 1)
                    inputMethodHints: Qt.ImhDigitsOnly
                    Accessible.name: qsTr("708 caption service number")
                    onEditingFinished: {
                        const n = Number(text)
                        if (Number.isInteger(n) && n >= 1 && n <= 63)
                            root.overrideChanged("cc708Service", n)
                        else
                            text = String(root.overrides.cc708Service ?? root.summary.cc708Service ?? 1)
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("1 to 63 (1 is the primary language)")
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
            }
            RowLabel { text: qsTr("Carry in"); visible: root.embedding }
            PillTabs {
                id: carriageTabs
                objectName: "exportCaptionCarriageTabs"
                visible: root.embedding
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Auto"), qsTr("Video stream"), qsTr("Caption track"), qsTr("Both")]
                Accessible.name: qsTr("Where the captions go")
                Accessible.description: qsTr("Video stream: inside the H.264, HEVC or XDCAM picture. Caption track: an SMPTE 436M "
                                             + "track in MXF, a c608 track in QuickTime.")
                onActivated: index => root.overrideChanged("captionCarriage", index === 0 ? undefined : root.captionCarriages[index])
            }
            RowLabel { text: qsTr("Display"); visible: root.embedding }
            PillTabs {
                id: captionDisplayTabs
                objectName: "exportCaptionDisplayTabs"
                visible: root.embedding
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Pop-on"), qsTr("Roll-up"), qsTr("Paint-on")]
                Accessible.name: qsTr("How the captions appear")
                onActivated: index => root.overrideChanged("captionDisplay", index === 0 ? undefined : root.captionDisplays[index])
            }
            // What this export embeds, or why the format cannot.
            Item {
                visible: root.embedding
                implicitWidth: 1
                implicitHeight: 1
            }
            Text {
                objectName: "exportEmbeddedCaptionsPlan"
                visible: root.embedding
                Layout.fillWidth: true
                text: root.summary.ok ? (root.summary.embeddedCaptions ?? "") : (root.summary.error ?? "")
                wrapMode: Text.WordWrap
                color: root.summary.ok ? Theme.textSecondary : Theme.dangerText
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            // 0.2: a soft subtitle track inside the file (3GPP Timed Text in MP4 / MOV, SubRip in MKV).
            RowLabel { text: qsTr("Subtitles"); visible: root.summary.subtitlesAvailable ?? false }
            Toggle {
                objectName: "exportSubtitleTrack"
                visible: root.summary.subtitlesAvailable ?? false
                text: qsTr("Subtitle track in the file")
                checked: root.summary.subtitles ?? false
                onToggled: root.overrideChanged("subtitles", checked)
            }

            // AI-15, 0.2 (Premiere's Export Settings > Content Credentials): signed C2PA Content Credentials - who
            // produced the export, what was made with AI, the sources and the redactions, verifiable. "AI media" (the
            // default) attaches them to exports that contain AI-generated media; "Always" to every export.
            RowLabel { text: qsTr("Content Credentials"); visible: root.signable }
            RowLayout {
                visible: root.signable
                Layout.fillWidth: true
                spacing: Theme.space(2)
                PillTabs {
                    id: credentialsTabs
                    objectName: "exportCredentialsTabs"
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("Off"), qsTr("AI media"), qsTr("Always")]
                    Accessible.name: qsTr("Attach Content Credentials")
                    onActivated: index => ExportController.contentCredentials = root.credentialModes[index]
                }
                IconButton {
                    objectName: "exportCredentialsSettings"
                    iconName: "shield-check"
                    compact: true
                    tooltip: qsTr("Content Credentials settings: signing certificate, AI training")
                    onClicked: credentialsSettings.open()
                }
            }
            RowLabel { text: qsTr("Producer"); visible: root.signable && credentialsTabs.currentIndex > 0 }
            FieldInput {
                objectName: "exportCredentialsProducer"
                visible: root.signable && credentialsTabs.currentIndex > 0
                Layout.fillWidth: true
                text: ExportController.credentialsProducer
                placeholderText: qsTr("Your name or studio (optional)")
                Accessible.name: qsTr("Producer")
                onEditingFinished: ExportController.credentialsProducer = text
            }
            RowLabel { text: qsTr("Thumbnail"); visible: root.signable && credentialsTabs.currentIndex > 0 }
            Toggle {
                objectName: "exportCredentialsThumbnail"
                visible: root.signable && credentialsTabs.currentIndex > 0
                text: qsTr("Include a thumbnail of the export")
                checked: ExportController.credentialsThumbnail
                onToggled: ExportController.credentialsThumbnail = checked
            }

            RowLabel { text: qsTr("Details") }
            Text {
                Layout.fillWidth: true
                text: root.summary.ok ? qsTr("%1 · %2 · %3 · %4").arg(root.summary.resolution).arg(root.summary.frameRate ?? "")
                                                               .arg(root.summary.duration).arg(root.summary.audio)
                                        + (root.summary.dynamicRange ? " · " + root.summary.dynamicRange : "")
                                      : ""
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
        }
    }
}
