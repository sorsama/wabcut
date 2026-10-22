pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The settings drawer of the Export panel (design language v3: a section card): a header row ("● Settings", the
// resolved format, a round chevron) that opens a body of segmented pill rows on top of the selected preset: container,
// codec, encoder (auto = hardware first, with the encoder auto picks shown), quality, resolution, range, and the
// audio's channels and codec (A.9: as the master, mono, stereo, 5.1; AAC or AC-3). Labels sit beside their rows, or
// above them in a narrow slot. The body grows by height over durCollapse and stays mounted. The rows are controlled:
// they show `summary` (ExportController.describe) and report changes through overrideChanged(key, value)
// (ExportController.queueExport keys).
Item {
    id: root

    property var summary: ({})
    property var overrides: ({})
    property bool open: false

    signal overrideChanged(string key, var value)

    readonly property var containers: ["mp4", "mov", "mkv", "wav"]
    readonly property var codecs: ["h264", "hevc", "prores", "dnxhr"]
    readonly property var qualities: [16, 20, 26] // CRF / CQ: high, balanced, small file
    readonly property var sizes: [[0, 0], [1280, 720], [1920, 1080], [3840, 2160]]
    readonly property var ranges: ["whole", "inOut", "workArea", "custom"] // 0.2: Premiere's Range
    readonly property var audioChannels: [0, 1, 2, 6] // 0 = as the master
    readonly property var dynamicRanges: ["auto", "sdr", "pq", "hlg"] // 0.2 (I.9)
    readonly property bool hasVideo: !(summary.audioOnly ?? (summary.container === "wav"))
    readonly property bool isImage: summary.isImage ?? false // 0.2: stills and image sequences
    readonly property bool longGop: hasVideo && (summary.videoCodec === "h264" || summary.videoCodec === "hevc")
    readonly property var credentialModes: ["off", "ai", "always"]
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
        containerTabs.currentIndex = containers.indexOf(summary.container ?? "mp4") // -1: a format preset's (0.2)
        codecTabs.currentIndex = codecs.indexOf(summary.videoCodec ?? "h264")
        encoderTabs.currentIndex = (overrides.encoder ?? "auto") === "auto" ? 0 : 1
        const quality = overrides.quality ?? 16
        qualityTabs.currentIndex = Math.max(0, qualities.indexOf(quality))
        const width = overrides.width ?? 0
        sizeTabs.currentIndex = Math.max(0, sizes.findIndex(s => s[0] === width))
        rangeTabs.currentIndex = Math.max(0, ranges.indexOf(summary.rangeSetting ?? overrides.range ?? "whole"))
        channelTabs.currentIndex = Math.max(0, audioChannels.indexOf(summary.audioChannels ?? 2))
        audioCodecTabs.currentIndex = summary.audioCodec === "ac3" ? 1 : 0
        dynamicRangeTabs.currentIndex = Math.max(0, dynamicRanges.indexOf(summary.dynamicRangeSetting ?? "auto"))
        credentialsTabs.currentIndex = Math.max(0, credentialModes.indexOf(ExportController.contentCredentials))
    }
    onSummaryChanged: sync()
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
                model: [qsTr("MP4"), qsTr("MOV"), qsTr("MKV"), qsTr("WAV")]
                Accessible.name: qsTr("Container")
                onActivated: index => {
                    const container = root.containers[index]
                    root.overrideChanged("container", container)
                    // MP4 carries H.264/HEVC only: leave ProRes/DNxHR for H.264.
                    if (container === "mp4" && !root.longGop && root.hasVideo)
                        root.overrideChanged("videoCodec", "h264")
                }
            }

            RowLabel { text: qsTr("Codec"); visible: root.hasVideo && !root.isImage }
            PillTabs {
                id: codecTabs
                visible: root.hasVideo && !root.isImage
                Layout.fillWidth: true
                compact: true
                model: [qsTr("H.264"), qsTr("HEVC"), qsTr("ProRes"), qsTr("DNxHR")]
                Accessible.name: qsTr("Video codec")
                onActivated: index => {
                    const codec = root.codecs[index]
                    // ProRes and DNxHR masters go into QuickTime.
                    if ((codec === "prores" || codec === "dnxhr") && root.summary.container === "mp4")
                        root.overrideChanged("container", "mov")
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
                visible: root.longGop
                Layout.fillWidth: true
                compact: true
                model: [qsTr("High"), qsTr("Balanced"), qsTr("Small")]
                Accessible.name: qsTr("Quality")
                onActivated: index => root.overrideChanged("quality", root.qualities[index])
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
                onActivated: index => root.overrideChanged("dynamicRange", index === 0 ? undefined : root.dynamicRanges[index])
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
                model: [qsTr("Entire Source"), qsTr("Source In/Out"), qsTr("Work Area"), qsTr("Custom")]
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

            // A.9: the audio's channels (as the master: 5.1 for a 5.1 master when the codec takes it) and codec.
            RowLabel { text: qsTr("Audio channels") }
            PillTabs {
                id: channelTabs
                objectName: "exportAudioChannelsTabs"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("As the master"), qsTr("Mono"), qsTr("Stereo"), qsTr("5.1")]
                Accessible.name: qsTr("Audio channels")
                onActivated: index => root.overrideChanged("audioChannels", root.audioChannels[index])
            }
            RowLabel { text: qsTr("Audio codec"); visible: root.hasVideo }
            PillTabs {
                id: audioCodecTabs
                objectName: "exportAudioCodecTabs"
                visible: root.hasVideo
                Layout.fillWidth: true
                compact: true
                model: [qsTr("AAC"), qsTr("AC-3")]
                Accessible.name: qsTr("Audio codec")
                onActivated: index => root.overrideChanged("audioCodec", index === 0 ? undefined : "ac3")
            }

            // I.4: captions burned into the picture (as the monitor shows them) or as a sidecar file next to it; P.2:
            // Premiere's sidecar formats, and CEA-608 embedded in H.264 / HEVC.
            RowLabel { text: qsTr("Captions"); visible: root.hasVideo }
            CaptionExportChooser {
                visible: root.hasVideo
                Layout.fillWidth: true
                onChosen: value => root.overrideChanged("captions", value)
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
