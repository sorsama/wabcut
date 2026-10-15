pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// ● CHANNEL VOLUME (A.9, M5n) in Effect Controls: Premiere's Channel Volume for the audio clip the panel edits
// (AudioChannels.volume): one row per clip channel (L / R, the six 5.1 channels, Mono) with the stopwatch, a dB slider
// and number, and the key at the playhead. A drag is one undo step; on an animated channel it moves the key at the
// playhead (adding one there). A muted channel (Modify > Audio Channels) says so. The channels themselves are set in
// Modify > Audio Channels… (the button).
Rectangle {
    id: root
    objectName: "channelVolumeSection"

    property bool expanded: true
    readonly property var volume: AudioChannels.volume
    readonly property var channels: volume.channels ?? []

    implicitHeight: column.implicitHeight + Theme.space(3)
    radius: Theme.radiusField
    color: Theme.sunken
    border.width: Theme.hairline
    border.color: Theme.border

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space(1.5)
        spacing: Theme.space(1)

        // Header: collapse, title, the clip's format, Modify > Audio Channels….
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)

            IconButton {
                objectName: "channelVolumeToggle"
                compact: true
                iconName: root.expanded ? "chevron-down" : "chevron-right"
                tooltip: root.expanded ? qsTr("Collapse Channel Volume") : qsTr("Expand Channel Volume")
                onClicked: root.expanded = !root.expanded
            }
            Text {
                text: qsTr("Channel Volume")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
            Rectangle {
                objectName: "channelVolumeFormat"
                implicitWidth: formatText.implicitWidth + Theme.space(2)
                implicitHeight: Theme.space(4.5)
                radius: Theme.radiusBadge
                color: Theme.raised
                border.width: Theme.hairline
                border.color: Theme.border
                Text {
                    id: formatText
                    anchors.centerIn: parent
                    text: root.volume.format === "5.1" ? qsTr("5.1")
                        : root.volume.format === "mono" ? qsTr("Mono")
                        : root.volume.format === "adaptive" ? qsTr("Adaptive") : qsTr("Stereo")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Font.DemiBold
                }
            }
            Item { Layout.fillWidth: true }
            PillButton {
                objectName: "channelVolumeModify"
                compact: true
                variant: "ghost"
                iconName: "sliders-horizontal"
                text: qsTr("Audio Channels…")
                tooltip: qsTr("Modify > Audio Channels: which source channels this clip plays")
                onClicked: AudioChannels.openDialog([root.volume.clip])
            }
        }

        // A count, not the rows: every edit (each step of a drag) gives `volume` a new list, and a list model would
        // rebuild the rows, destroying the slider being dragged. The rows read their channel by index.
        Repeater {
            id: rows
            model: root.expanded ? root.channels.length : 0

            delegate: RowLayout {
                id: channelRow
                required property int index
                readonly property var modelData: root.channels[index] ?? ({ channel: index + 1, label: "", db: 0 })
                readonly property int channel: index + 1
                objectName: "channelVolumeRow" + channel
                Layout.fillWidth: true
                spacing: Theme.space(1)

                // Stopwatch: animate this channel's volume (keyframes).
                IconButton {
                    objectName: "channelStopwatch"
                    compact: true
                    checkable: true
                    checked: channelRow.modelData.animated === true
                    iconName: "timer"
                    iconSize: Theme.iconSm
                    tooltip: channelRow.modelData.animated ? qsTr("Stop animating %1 (keeps its value here)").arg(channelRow.modelData.label)
                                                           : qsTr("Animate %1").arg(channelRow.modelData.label)
                    onClicked: {
                        checked = Qt.binding(() => channelRow.modelData.animated === true)
                        AudioChannels.setChannelAnimated(channelRow.channel, !(channelRow.modelData.animated === true))
                    }
                }
                PropSlider {
                    objectName: "channelSlider"
                    Layout.fillWidth: true
                    label: channelRow.modelData.muted ? qsTr("%1 (muted)").arg(channelRow.modelData.label) : channelRow.modelData.label
                    unit: qsTr("dB")
                    from: -60
                    to: 24
                    origin: 0
                    defaultValue: 0
                    stepSize: 0.1
                    decimals: 1
                    value: Math.max(-60, channelRow.modelData.db ?? 0)
                    opacity: channelRow.modelData.muted ? Theme.disabledOpacity : 1
                    Accessible.name: qsTr("Channel Volume %1").arg(channelRow.modelData.label)
                    onValueModified: v => AudioChannels.setChannelVolume(channelRow.channel, v)
                    onGestureEnded: AudioChannels.endChannelVolume()
                }
                IconButton {
                    objectName: "channelKey"
                    visible: channelRow.modelData.animated === true
                    compact: true
                    checkable: true
                    checked: channelRow.modelData.keyed === true
                    iconName: "diamond"
                    iconSize: Theme.iconXs
                    tooltip: channelRow.modelData.keyed ? qsTr("Remove the keyframe of %1 at the playhead").arg(channelRow.modelData.label)
                                                        : qsTr("Add a keyframe of %1 at the playhead").arg(channelRow.modelData.label)
                    onClicked: {
                        checked = Qt.binding(() => channelRow.modelData.keyed === true)
                        AudioChannels.toggleChannelKey(channelRow.channel)
                    }
                }
            }
        }
    }
}
