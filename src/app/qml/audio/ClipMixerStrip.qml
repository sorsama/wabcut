pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// p02 audio: one strip of the Audio Clip Mixer (an entry of ClipMixer.strips): the clip under the playhead on one audio
// track. Its Volume as the fader, its Panner as the knob (a drag is one undo step; double-click resets), the track's
// meter, mute and solo, and Write Keyframes (the diamond): while playing, the held fader or knob writes keyframes into
// the clip, committed when playback stops. Empty where the track has no clip under the playhead.
Rectangle {
    id: root

    required property var strip
    readonly property bool hasClip: (strip.clip ?? "") !== ""

    objectName: "clipMixerStrip_" + strip.shortName
    implicitWidth: Theme.mixerStripWidth
    radius: Theme.radiusField
    color: Theme.raised
    border.width: Theme.hairline
    border.color: Theme.border

    component StripToggle: AbstractButton {
        id: toggle
        property color onColor: Theme.accentFill
        property string tip: ""
        Layout.fillWidth: true
        implicitHeight: Theme.controlHeightSmall - Theme.space(2)
        checkable: true
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: tip
        background: Rectangle {
            radius: Theme.radiusBadge
            color: toggle.checked ? toggle.onColor : toggle.hovered ? Theme.hover : Theme.sunken
            border.width: Theme.hairline
            border.color: toggle.checked ? toggle.onColor : Theme.border
        }
        contentItem: Text {
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: toggle.text
            color: toggle.checked ? Theme.clipInk : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.Bold
        }
        FocusRing {
            targetRadius: Theme.radiusBadge
            visible: toggle.visualFocus
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.space(1) + Theme.hairline
        spacing: Theme.space(1)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Rectangle {
                implicitWidth: shortName.implicitWidth + Theme.space(2)
                implicitHeight: Theme.space(5)
                radius: Theme.radiusBadge
                color: Theme.clipColor(root.strip.colorIndex)
                Text {
                    id: shortName
                    anchors.centerIn: parent
                    text: root.strip.shortName
                    color: Theme.clipInk
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Font.Bold
                }
            }
            IconButton {
                id: writeKeys
                objectName: "writeKeyframes"
                Layout.alignment: Qt.AlignRight
                iconName: "diamond"
                compact: true
                checkable: true
                checked: root.strip.writeKeyframes === true
                tooltip: qsTr("Write Keyframes: while playing, moving this clip's fader or pan writes keyframes")
                onToggled: ClipMixer.setWriteKeyframes(root.strip.key, checked)
            }
        }
        Text {
            objectName: "clipMixerClipName"
            Layout.fillWidth: true
            text: root.hasClip ? (root.strip.clipName !== "" ? root.strip.clipName : qsTr("Clip")) : qsTr("No clip")
            elide: Text.ElideRight
            color: root.hasClip ? Theme.textSecondary : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }

        PanKnob {
            objectName: "clipPanKnob"
            Layout.alignment: Qt.AlignHCenter
            enabled: root.hasClip
            pan: root.strip.pan ?? 0
            label: qsTr("Pan of the clip on %1").arg(root.strip.shortName)
            onMoved: value => ClipMixer.setPan(root.strip.key, value)
            onReleased: ClipMixer.endGesture()
            onHeldChanged: ClipMixer.touch(root.strip.key, "pan", held)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1) / 2
            StripToggle {
                objectName: "clipMixerMute"
                //: Mute (one letter on the mixer strip)
                text: qsTr("M")
                tip: qsTr("Mute %1").arg(root.strip.shortName)
                onColor: Theme.warning
                checked: root.strip.muted
                onToggled: ClipMixer.setMuted(root.strip.key, checked)
            }
            StripToggle {
                objectName: "clipMixerSolo"
                //: Solo (one letter on the mixer strip)
                text: qsTr("S")
                tip: qsTr("Solo %1").arg(root.strip.shortName)
                onColor: Theme.success
                checked: root.strip.solo
                onToggled: ClipMixer.setSolo(root.strip.key, checked)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Theme.space(24)
            spacing: Theme.space(1)
            Item { Layout.fillWidth: true }
            Fader {
                objectName: "clipFader"
                Layout.fillHeight: true
                enabled: root.hasClip
                gainDb: root.strip.gainDb ?? 0
                label: qsTr("Volume of the clip on %1").arg(root.strip.shortName)
                onMoved: value => ClipMixer.setGainDb(root.strip.key, value)
                onReleased: ClipMixer.endGesture()
                onHeldChanged: ClipMixer.touch(root.strip.key, "volume", held)
            }
            MeterBar {
                objectName: "clipMixerMeter"
                Layout.fillHeight: true
                Layout.preferredWidth: Theme.meterBarWidth
                Layout.topMargin: Theme.space(2)
                Layout.bottomMargin: Theme.space(2)
                source: AudioMixer
                channel: root.strip.key
                trackColor: Theme.sunken
                lowColor: Theme.success
                midColor: Theme.warning
                highColor: Theme.danger
                peakColor: Theme.textPrimary
            }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.hasClip ? qsTr("%1 dB").arg(Number(root.strip.gainDb ?? 0).toFixed(1)) : ""
            color: root.strip.writing === true ? Theme.danger : Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
    }
}
