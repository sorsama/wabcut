pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// ● AUDIO METERS (0.5, Premiere's Window > Audio Meters; docked right of the Timeline in the Edit preset): the level of
// the mix (the master's meter, AudioMixer "master": peak, RMS and peak hold per channel, two bars for stereo, six for a
// 5.1 master) filling the panel's height beside a dB scale (0 to -60), the highest peak hold as a readout and the clip
// light, which a click (or Space) clears. Meters poll only while a meter is on screen: this panel and the Audio Track
// Mixer each count as a viewer (AudioMixer.setMeterViewer), so closing one leaves the other running.
// Slim: below a strip's width (a tab and the dock's handle) the panel drops its tab strip and shows the dock's handle
// over its top-right corner on hover, as the monitors do; in a group of tabs or a window of its own it has its strip.
Panel {
    id: root
    objectName: "audioMetersPanel"

    /// The marks of the scale, dBFS (the meter's floor is the last one).
    readonly property var scaleMarks: [0, -6, -12, -18, -24, -36, -48, -60]
    readonly property real floorDb: -60
    /// Wide enough for the tab strip (the tab and the dock's grip, pop-out and close buttons).
    readonly property bool roomForStrip: root.width >= Theme.space(42)
    /// The meter polls while this panel can be seen.
    readonly property bool metering: root.visible && root.enabled
    readonly property bool clipped: meter.clipped
    readonly property var channelNames: meter.channels > 2 ? [qsTr("L"), qsTr("R"), qsTr("C"), qsTr("LFE"), qsTr("Ls"), qsTr("Rs")]
                                                           : [qsTr("L"), qsTr("R")]

    /// Where `db` sits on a meter `height` tall, from its top (0 dBFS) to its bottom (the floor).
    function yOf(db: real, height: real): real {
        const f = Math.max(0, Math.min(1, (db - root.floorDb) / -root.floorDb))
        return (1 - f) * height
    }
    function clearClip() {
        AudioMixer.clearClip("master")
    }
    function focusContent() {
        clipLight.forceActiveFocus(Qt.OtherFocusReason)
    }

    title: root.roomForStrip ? qsTr("Audio Meters") : ""
    Accessible.name: qsTr("Audio Meters")
    padding: root.roomForStrip ? Theme.pad : Theme.space(2)

    onMeteringChanged: AudioMixer.setMeterViewer(root, root.metering)
    Component.onCompleted: AudioMixer.setMeterViewer(root, root.metering)

    ColumnLayout {
        anchors.fill: parent
        // Slim and docked without a strip: the dock's handle shows over the top-right corner on hover; the clip light
        // starts below it (a click on the light never lands on the handle's close button).
        anchors.topMargin: root.dockHost !== null && !root.hasStrip ? Theme.controlHeightSmall + Theme.space(1) : 0
        spacing: Theme.space(1)

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Item {
                Layout.fillWidth: true
            }
            // The clip light, the meter with its scale on the left, and the channels under the bars.
            ColumnLayout {
                id: meterColumn
                Layout.fillHeight: true
                Layout.leftMargin: scale.width + Theme.space(1)
                // Two bars up to 80 px wide together (six up to 144), narrower when the panel is.
                Layout.fillWidth: true
                Layout.preferredWidth: meter.channels > 2 ? Theme.space(36) : Theme.space(20)
                Layout.maximumWidth: Math.max(Theme.space(4), Math.min(meter.channels > 2 ? Theme.space(36) : Theme.space(20),
                                                                       root.width - root.padding * 2 - scale.width - Theme.space(1)))
                spacing: Theme.space(1)

                // Red once the mix reached 0 dBFS, until it is clicked.
                Rectangle {
                    id: clipLight
                    objectName: "audioMetersClipLight"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.space(2)
                    radius: Theme.radiusBadge / 2
                    color: root.clipped ? Theme.danger : Theme.sunken
                    border.width: Theme.hairline
                    border.color: root.clipped ? Theme.danger : Theme.borderStrong
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: root.clipped ? qsTr("The mix clipped: clear the clip light") : qsTr("Clip light: no clipping")
                    Accessible.onPressAction: root.clearClip()
                    Keys.onSpacePressed: root.clearClip()
                    Keys.onReturnPressed: root.clearClip()
                    Keys.onEnterPressed: root.clearClip()

                    FocusRing {
                        targetRadius: clipLight.radius
                        visible: clipLight.activeFocus
                    }
                    TapHandler {
                        onTapped: root.clearClip()
                    }
                    HoverHandler {
                        id: clipHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    Loader {
                        active: clipHover.hovered
                        sourceComponent: Tooltip {
                            parent: clipLight
                            visible: true
                            text: root.clipped ? qsTr("The mix clipped: click to clear") : qsTr("Clip light")
                        }
                    }
                }

                Item {
                    id: meterFrame
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    MeterBar {
                        id: meter
                        objectName: "audioMetersMeter"
                        anchors.fill: parent
                        source: AudioMixer
                        channel: "master"
                        floorDb: root.floorDb
                        idleChannels: 2
                        trackColor: Theme.sunken
                        lowColor: Theme.dark ? Theme.hueMint : Theme.success
                        midColor: Theme.dark ? Theme.hueLime : Theme.warning
                        highColor: Theme.dark ? Theme.hueOrange : Theme.danger
                        peakColor: Theme.textPrimary
                        Accessible.role: Accessible.Indicator
                        Accessible.name: qsTr("Level of the mix")
                        Accessible.description: qsTr("Peak %1 dB").arg(meter.peakText)
                    }
                    Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.width: Theme.hairline
                        border.color: Theme.border
                    }

                    // The dB scale, its marks where the meter draws those levels.
                    Item {
                        id: scale
                        objectName: "audioMetersScale"
                        anchors.right: parent.left
                        anchors.rightMargin: Theme.space(1)
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: Theme.space(6)

                        Repeater {
                            model: root.scaleMarks
                            delegate: Item {
                                id: mark
                                required property real modelData
                                readonly property real at: root.yOf(mark.modelData, scale.height)
                                objectName: "audioMetersMark_" + Math.abs(mark.modelData)
                                width: scale.width
                                height: label.implicitHeight
                                y: Math.max(0, Math.min(scale.height - mark.height, mark.at - mark.height / 2))

                                Text {
                                    id: label
                                    objectName: "audioMetersMarkLabel"
                                    anchors.right: parent.right
                                    anchors.rightMargin: Theme.space(1.5)
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: mark.modelData === 0 ? "0" : qsTr("−%1").arg(Math.abs(mark.modelData))
                                    color: Theme.textSecondary // >= 4.5:1 in the light theme too (small numerals)
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.textMicro
                                }
                                Rectangle {
                                    anchors.right: parent.right
                                    y: Math.round(mark.at - mark.y)
                                    width: Theme.space(1)
                                    height: Theme.hairline
                                    color: Theme.borderStrong
                                }
                            }
                        }
                    }
                }

                // The channels under their bars (hidden when the bars are too narrow for a name).
                Row {
                    id: channelRow
                    objectName: "audioMetersChannels"
                    Layout.fillWidth: true
                    readonly property real cell: meterFrame.width / Math.max(meter.channels, 2)
                    visible: channelRow.cell >= Theme.space(3)
                    Repeater {
                        model: root.channelNames
                        delegate: Text {
                            required property string modelData
                            width: channelRow.cell
                            horizontalAlignment: Text.AlignHCenter
                            text: modelData
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMicro
                        }
                    }
                }
            }
            Item {
                Layout.fillWidth: true
            }
        }

        // The highest peak hold (dBFS), red once the mix clipped.
        Text {
            objectName: "audioMetersPeak"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("%1 dB").arg(meter.peakText)
            elide: Text.ElideRight
            color: root.clipped ? Theme.danger : Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Peak %1 dB").arg(meter.peakText)
        }
    }
}
