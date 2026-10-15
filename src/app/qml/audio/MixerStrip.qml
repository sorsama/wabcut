pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// One channel strip of the Audio Track Mixer (M5g), for one audio track (an entry of AudioMixer.strips):
// its name and role (Dialogue / Music / Effects: track.setRole), the effect stack, pan, Mute / Solo / Record arm (A.8:
// RecordArmButton), the fader beside a stereo level meter with its clip light and peak readout, and the gain.
// M5l: fader and pan are A.3's track Volume and Panner (a drag is one undo step; double-click resets); the values follow
// the automation under the playhead (AudioMixer.faderValues), and the strip's automation mode records them while
// playing (Touch / Latch / Write).
// M5n (A.9): while the master is 5.1, a mono or stereo track's pan knob becomes its 5.1 panner (SurroundPanner: the
// puck and the LFE send, track.setSurroundPan); a 5.1 or adaptive track has no panner there.
Rectangle {
    id: root

    required property var strip
    readonly property var values: AudioMixer.faderValues[strip.key] ?? ({ gainDb: strip.gainDb, pan: strip.pan })
    readonly property string trackFormat: AudioChannels.trackFormats[strip.key] ?? "stereo"
    readonly property bool surroundMaster: AudioChannels.masterFormat === "5.1"
    readonly property bool surroundShown: surroundMaster && (trackFormat === "mono" || trackFormat === "stereo")
    readonly property var surroundValues: AudioChannels.surround[strip.key] ?? ({})

    function roleColor(role) {
        return role === "dialogue" ? Theme.info
             : role === "music" ? Theme.keyApplication
             : role === "effects" ? Theme.clipColor(2) : Theme.textTertiary
    }

    // Mute / Solo / Record buttons.
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
            opacity: toggle.enabled ? 1 : Theme.disabledOpacity
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
        HoverHandler {
            enabled: toggle.enabled
            cursorShape: Qt.PointingHandCursor
        }
        Loader {
            active: toggle.hovered && toggle.tip !== ""
            sourceComponent: Tooltip {
                parent: toggle
                visible: true
                text: toggle.tip
            }
        }
    }

    objectName: "mixerStrip_" + strip.shortName
    implicitWidth: Theme.mixerStripWidth
    radius: Theme.radiusField
    color: Theme.raised
    border.width: Theme.hairline
    border.color: Theme.border

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.space(1) + Theme.hairline
        spacing: Theme.space(1)

        // Name
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
            Text {
                Layout.fillWidth: true
                text: root.strip.name
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }

        // Role
        AbstractButton {
            id: roleChip
            objectName: "roleChip"
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeightSmall - Theme.space(1)
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.name: qsTr("Role of %1: %2").arg(root.strip.shortName).arg(root.strip.roleName)
            onClicked: roleMenu.popup(roleChip, 0, roleChip.height)
            background: Rectangle {
                radius: Theme.radiusPill
                color: Qt.alpha(root.roleColor(root.strip.role), roleChip.hovered ? 0.28 : 0.16)
                border.width: Theme.hairline
                border.color: Qt.alpha(root.roleColor(root.strip.role), 0.6)
            }
            contentItem: Text {
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: root.strip.roleName
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
            }
            FocusRing {
                targetRadius: Theme.radiusPill
                visible: roleChip.visualFocus
            }
            HoverHandler {
                enabled: roleChip.enabled
                cursorShape: Qt.PointingHandCursor
            }
            OptionsMenu {
                id: roleMenu
                objectName: "roleMenu"
                options: AudioMixer.roles
                current: root.strip.role
                onChosen: id => AudioMixer.setRole(root.strip.key, id)
            }
        }

        // Effects
        EffectSlots {
            Layout.fillWidth: true
            target: root.strip.key
            effects: root.strip.effects
        }

        // Pan: the knob, or the 5.1 panner while the master is 5.1.
        PanKnob {
            objectName: "panKnob"
            Layout.alignment: Qt.AlignHCenter
            visible: !root.surroundShown
            enabled: AudioMixer.faderAvailable && !root.surroundMaster
            pan: root.values.pan ?? 0
            label: qsTr("Pan of %1").arg(root.strip.shortName)
            note: root.surroundMaster ? qsTr("A %1 track plays its channels to the 5.1 master as they are").arg(root.trackFormat)
                                      : AudioMixer.faderNote
            onMoved: value => AudioMixer.setPan(root.strip.key, value)
            onReleased: AudioMixer.endFaderGesture()
            onHeldChanged: AudioMixer.touch(root.strip.key, "pan", held)
        }
        SurroundPanner {
            objectName: "surroundPanner"
            Layout.fillWidth: true
            visible: root.surroundShown
            label: root.strip.shortName
            panX: root.surroundValues.x ?? 0
            panY: root.surroundValues.y ?? 1
            lfeDb: root.surroundValues.lfeDb ?? -144
            onMoved: (x, y, lfe) => AudioChannels.setSurroundPan(root.strip.key, x, y, lfe)
            onReleased: AudioChannels.endSurroundPan()
        }

        // Mute / Solo / Record
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1) / 2
            StripToggle {
                objectName: "muteToggle"
                //: Mute (one letter on the mixer strip)
                text: qsTr("M")
                tip: qsTr("Mute %1").arg(root.strip.shortName)
                onColor: Theme.warning
                checked: root.strip.muted
                onToggled: AudioMixer.setMuted(root.strip.key, checked)
            }
            StripToggle {
                objectName: "soloToggle"
                //: Solo (one letter on the mixer strip)
                text: qsTr("S")
                tip: qsTr("Solo %1").arg(root.strip.shortName)
                onColor: Theme.success
                checked: root.strip.solo
                onToggled: AudioMixer.setSolo(root.strip.key, checked)
            }
            RecordArmButton {
                objectName: "recordToggle"
                Layout.fillWidth: true
                letter: true
                track: root.strip.key
                trackLabel: root.strip.shortName
                armed: root.strip.recordArmed === true
            }
        }

        // Clip light and peak hold readout
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: meter.peakText
                color: meter.clipped ? Theme.danger : Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
            Rectangle {
                id: clipLight
                objectName: "clipLight"
                implicitWidth: Theme.meterBarWidth
                implicitHeight: Theme.space(2)
                radius: Theme.radiusBadge / 2
                color: meter.clipped ? Theme.danger : Theme.sunken
                border.width: Theme.hairline
                border.color: meter.clipped ? Theme.danger : Theme.border
                Accessible.role: Accessible.Button
                Accessible.name: meter.clipped ? qsTr("Clipped: click to clear") : qsTr("No clipping")
                Accessible.onPressAction: AudioMixer.clearClip(root.strip.key)
                TapHandler {
                    onTapped: AudioMixer.clearClip(root.strip.key)
                }
                HoverHandler {
                    enabled: meter.clipped
                    cursorShape: Qt.PointingHandCursor
                }
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
                objectName: "fader"
                Layout.fillHeight: true
                enabled: AudioMixer.faderAvailable
                gainDb: root.values.gainDb ?? 0
                label: qsTr("Volume of %1").arg(root.strip.shortName)
                note: AudioMixer.faderNote
                onMoved: value => AudioMixer.setGainDb(root.strip.key, value)
                onReleased: AudioMixer.endFaderGesture()
                onHeldChanged: AudioMixer.touch(root.strip.key, "volume", held)
            }
            MeterBar {
                id: meter
                objectName: "meter"
                Layout.fillHeight: true
                Layout.preferredWidth: Theme.meterBarWidth
                Layout.topMargin: Theme.space(2)
                Layout.bottomMargin: Theme.space(2)
                source: AudioMixer
                channel: root.strip.key
                trackColor: Theme.sunken
                lowColor: Theme.hueMint
                midColor: Theme.hueLime
                highColor: Theme.hueOrange
                peakColor: Theme.textPrimary
            }
            Item { Layout.fillWidth: true }
        }

        // Gain readout
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("%1 dB").arg(Number(root.values.gainDb ?? 0).toFixed(1))
            color: Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
        // Automation (M5l)
        AutomationButton {
            Layout.alignment: Qt.AlignHCenter
            target: root.strip.key
            mode: root.strip.automation ?? "read"
        }
    }
}
