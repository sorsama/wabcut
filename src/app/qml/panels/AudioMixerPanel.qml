pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// ● AUDIO TRACK MIXER (the Audio workspace, M5g): one channel strip per audio track of the current sequence
// (AudioMixer.strips: role, effect stack, sends, pan, mute / solo, fader and live meter, output), then the submix strips
// (p02 audio: AudioMixer.submixes), scrolling sideways when they do not fit, and the master strip pinned on the right
// (master effects with the limiter preset, master meter, the loudness of the mix against a target). Meters poll at
// ~30 Hz only while a meter is on screen (the mixer or the Audio Meters panel: a viewer count).
// p02 audio: the AUDIO CLIP MIXER is the other tab (as Premiere groups the two): one strip per audio track for the clip
// under the playhead (ClipMixer.strips: the clip's volume and pan, Write Keyframes).
Panel {
    id: root
    objectName: "audioMixerPanel"

    /// 0: Audio Track Mixer, 1: Audio Clip Mixer (window.audioTrackMixer / window.audioClipMixer).
    property int view: 0

    KeymapScope.scope: "audioMixer"
    title: root.view === 1 ? qsTr("Audio Clip Mixer") : qsTr("Audio Track Mixer")

    leading: [
        PillTabs {
            objectName: "mixerViewTabs"
            compact: true
            model: [qsTr("Tracks"), qsTr("Clips")]
            currentIndex: root.view
            onActivated: index => root.view = index
        }
    ]

    // p02 audio: Premiere's Sequence > Add Tracks > Audio Submix Tracks, from the mixer.
    trailing: [
        PillButton {
            id: addSubmix
            objectName: "addSubmixButton"
            visible: root.view === 0
            compact: true
            variant: "ghost"
            iconName: "plus"
            text: qsTr("Submix")
            tooltip: qsTr("Add an audio submix track")
            onClicked: submixMenu.popup(addSubmix, 0, addSubmix.height)
            OptionsMenu {
                id: submixMenu
                objectName: "addSubmixMenu"
                options: AudioMixer.submixFormats.map(f => ({ id: f.id.substring(4), name: f.name }))
                onChosen: id => AudioMixer.addSubmix(id)
            }
        }
    ]

    // The meters run only while a meter can be seen: the mixer counts as one viewer (0.5: the Audio Meters panel is
    // another, AudioMixer.setMeterViewer).
    readonly property bool metering: root.visible && root.enabled
    onMeteringChanged: AudioMixer.setMeterViewer(root, root.metering)
    Component.onCompleted: AudioMixer.setMeterViewer(root, root.metering)

    RowLayout {
        anchors.fill: parent
        spacing: Theme.space(2)
        visible: root.view === 0

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
                // p02 audio: the submix strips after the tracks', as Premiere lists them.
                Repeater {
                    model: AudioMixer.submixes
                    delegate: SubmixStrip {
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

    // p02 audio: the Audio Clip Mixer.
    Flickable {
        id: clipStrips
        objectName: "clipMixerStrips"
        anchors.fill: parent
        visible: root.view === 1
        clip: true
        contentWidth: clipRow.implicitWidth
        contentHeight: height
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick
        ScrollBar.horizontal: ThinScrollBar {}

        Row {
            id: clipRow
            height: clipStrips.height - Theme.space(2)
            spacing: Theme.space(1)
            Repeater {
                model: root.view === 1 ? ClipMixer.strips : []
                delegate: ClipMixerStrip {
                    required property var modelData
                    strip: modelData
                    height: clipRow.height
                }
            }
        }

        Text {
            anchors.centerIn: parent
            visible: ClipMixer.strips.length === 0
            text: qsTr("This sequence has no audio tracks")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
    }
}
