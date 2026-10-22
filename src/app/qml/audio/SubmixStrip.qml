pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// p02 audio: a submix strip of the Audio Track Mixer (an entry of AudioMixer.submixes; Premiere's submix tracks): its
// name and format, the effect stack (the submix's track effects), pan (a mono submix pans its folded input; on a 5.1
// master the submix plays its channels as they are), Mute / Solo (soloing it plays what feeds it), the fader beside its
// meter, the automation mode and its output (the Mix or another submix). Fader and pan are the submix's Volume and
// Panner (track.setVolume / track.setPan on "S1"), one undo step per drag.
Rectangle {
    id: root

    required property var strip
    readonly property var values: AudioMixer.faderValues[strip.key] ?? ({ gainDb: strip.gainDb, pan: strip.pan })
    readonly property bool surroundMaster: AudioChannels.masterFormat === "5.1"

    objectName: "submixStrip_" + strip.shortName
    implicitWidth: Theme.mixerStripWidth
    radius: Theme.radiusField
    color: Theme.sunken
    border.width: Theme.hairline
    border.color: Theme.accentStroke

    // Mute / Solo buttons.
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
            color: toggle.checked ? toggle.onColor : toggle.hovered ? Theme.hover : Theme.raised
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

        // Name and format
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Rectangle {
                implicitWidth: shortName.implicitWidth + Theme.space(2)
                implicitHeight: Theme.space(5)
                radius: Theme.radiusBadge
                color: Theme.accentFill
                Text {
                    id: shortName
                    anchors.centerIn: parent
                    text: root.strip.shortName
                    color: Theme.accentOnFill
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Font.Bold
                }
            }
            Text {
                Layout.fillWidth: true
                text: root.strip.name
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Text {
                Layout.fillWidth: true
                text: qsTr("%1 submix").arg(root.strip.formatName)
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            IconButton {
                objectName: "removeSubmix"
                iconName: "trash-2"
                variant: "ghost"
                compact: true
                tooltip: qsTr("Delete %1 (its tracks go to the Mix)").arg(root.strip.name)
                onClicked: AudioMixer.removeSubmix(root.strip.key)
            }
        }

        // Effects
        EffectSlots {
            Layout.fillWidth: true
            target: root.strip.key
            effects: root.strip.effects
        }

        // Pan
        PanKnob {
            objectName: "submixPan"
            Layout.alignment: Qt.AlignHCenter
            enabled: !root.surroundMaster
            pan: root.values.pan ?? 0
            label: qsTr("Pan of %1").arg(root.strip.shortName)
            note: root.surroundMaster ? qsTr("A submix plays its channels to the 5.1 master as they are") : ""
            onMoved: value => AudioMixer.setPan(root.strip.key, value)
            onReleased: AudioMixer.endFaderGesture()
            onHeldChanged: AudioMixer.touch(root.strip.key, "pan", held)
        }

        // Mute / Solo
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1) / 2
            StripToggle {
                objectName: "submixMute"
                //: Mute (one letter on the mixer strip)
                text: qsTr("M")
                tip: qsTr("Mute %1").arg(root.strip.shortName)
                onColor: Theme.warning
                checked: root.strip.muted
                onToggled: AudioMixer.setMuted(root.strip.key, checked)
            }
            StripToggle {
                objectName: "submixSolo"
                //: Solo (one letter on the mixer strip)
                text: qsTr("S")
                tip: qsTr("Solo %1 and what feeds it").arg(root.strip.shortName)
                onColor: Theme.success
                checked: root.strip.solo
                onToggled: AudioMixer.setSolo(root.strip.key, checked)
            }
        }

        // Fader and meter
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Theme.space(24)
            spacing: Theme.space(1)
            Item { Layout.fillWidth: true }
            Fader {
                objectName: "submixFader"
                Layout.fillHeight: true
                gainDb: root.values.gainDb ?? 0
                label: qsTr("Volume of %1").arg(root.strip.shortName)
                onMoved: value => AudioMixer.setGainDb(root.strip.key, value)
                onReleased: AudioMixer.endFaderGesture()
                onHeldChanged: AudioMixer.touch(root.strip.key, "volume", held)
            }
            MeterBar {
                objectName: "submixMeter"
                Layout.fillHeight: true
                Layout.preferredWidth: Theme.meterBarWidth
                Layout.topMargin: Theme.space(2)
                Layout.bottomMargin: Theme.space(2)
                source: AudioMixer
                channel: root.strip.key
                trackColor: Theme.raised
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
            text: qsTr("%1 dB").arg(Number(root.values.gainDb ?? 0).toFixed(1))
            color: Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
        AutomationButton {
            Layout.alignment: Qt.AlignHCenter
            target: root.strip.key
            mode: root.strip.automation ?? "read"
        }
        OutputMenu {
            Layout.fillWidth: true
            target: root.strip.key
            output: root.strip.output ?? "mix"
            outputName: root.strip.outputName ?? ""
        }
    }
}
