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
    readonly property var ranges: ["whole", "inOut", "workArea"]
    readonly property var audioChannels: [0, 1, 2, 6] // 0 = as the master
    readonly property bool hasVideo: (summary.container ?? "mp4") !== "wav"
    readonly property bool longGop: hasVideo && (summary.videoCodec === "h264" || summary.videoCodec === "hevc")

    /// Narrow (a side slot): each row's label goes above its control instead of beside it.
    readonly property bool stacked: width < Theme.space(105)

    implicitHeight: header.height + body.height
    clip: true

    // The pill rows are uncontrolled (they move their own indicator on click), so the summary is pushed into
    // them whenever it changes.
    function sync() {
        containerTabs.currentIndex = Math.max(0, containers.indexOf(summary.container ?? "mp4"))
        codecTabs.currentIndex = Math.max(0, codecs.indexOf(summary.videoCodec ?? "h264"))
        encoderTabs.currentIndex = (overrides.encoder ?? "auto") === "auto" ? 0 : 1
        const quality = overrides.quality ?? 16
        qualityTabs.currentIndex = Math.max(0, qualities.indexOf(quality))
        const width = overrides.width ?? 0
        sizeTabs.currentIndex = Math.max(0, sizes.findIndex(s => s[0] === width))
        rangeTabs.currentIndex = Math.max(0, ranges.indexOf(overrides.range ?? "whole"))
        channelTabs.currentIndex = Math.max(0, audioChannels.indexOf(summary.audioChannels ?? 2))
        audioCodecTabs.currentIndex = summary.audioCodec === "ac3" ? 1 : 0
    }
    onSummaryChanged: sync()
    Component.onCompleted: sync()

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

            RowLabel { text: qsTr("Codec"); visible: root.hasVideo }
            PillTabs {
                id: codecTabs
                visible: root.hasVideo
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

            RowLabel { text: qsTr("Range") }
            PillTabs {
                id: rangeTabs
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Whole"), qsTr("In to Out"), qsTr("Work area")]
                Accessible.name: qsTr("Range")
                onActivated: index => root.overrideChanged("range", index === 0 ? undefined : root.ranges[index])
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

            // I.4: captions burned into the picture (as the monitor shows them) or as a sidecar file next to it.
            RowLabel { text: qsTr("Captions"); visible: root.hasVideo }
            PillTabs {
                id: captionTabs
                objectName: "exportCaptionsTabs"
                readonly property var modes: ["off", "burnIn", "srt", "vtt"]
                visible: root.hasVideo
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Off"), qsTr("Burn in"), qsTr("SRT file"), qsTr("VTT file")]
                Accessible.name: qsTr("Captions")
                onActivated: index => root.overrideChanged("captions", index === 0 ? undefined : modes[index])
            }

            RowLabel { text: qsTr("Details") }
            Text {
                Layout.fillWidth: true
                text: root.summary.ok ? qsTr("%1 · %2 · %3 · %4").arg(root.summary.resolution).arg(root.summary.frameRate ?? "")
                                                               .arg(root.summary.duration).arg(root.summary.audio)
                                      : ""
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
        }
    }
}
