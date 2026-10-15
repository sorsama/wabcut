pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// ● AUDIO TRACK MIXER (the Audio workspace, M5g): one channel strip per audio track of the current sequence
// (AudioMixer.strips: role, effect stack, pan, mute / solo, fader and live meter), scrolling sideways when they do
// not fit, and the master strip pinned on the right (master effects with the limiter preset, master meter, the
// loudness of the mix against a target). Meters poll at ~30 Hz only while the mixer is on screen.
Panel {
    id: root
    objectName: "audioMixerPanel"

    KeymapScope.scope: "audioMixer"
    title: qsTr("Audio Track Mixer")

    // The meters run only while the mixer can be seen.
    Binding {
        target: AudioMixer
        property: "metersActive"
        value: root.visible && root.enabled
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        Flickable {
            id: strips
            objectName: "mixerStrips"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: stripRow.implicitWidth
            contentHeight: height
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.HorizontalFlick
            ScrollBar.horizontal: ThinScrollBar {}

            Row {
                id: stripRow
                height: strips.height - Theme.space(2)
                spacing: Theme.space(1)

                Repeater {
                    model: AudioMixer.strips
                    delegate: MixerStrip {
                        required property var modelData
                        strip: modelData
                        height: stripRow.height
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: AudioMixer.strips.length === 0
                text: qsTr("This sequence has no audio tracks")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
        }

        MasterStrip {
            Layout.fillHeight: true
            Layout.bottomMargin: Theme.space(2)
            Layout.preferredWidth: Theme.mixerMasterWidth
        }
    }
}
