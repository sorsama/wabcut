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
// 0.7 (card 032): the panel menu (the "⋯" button, Premiere's Audio Track Mixer menu): Show/Hide Tracks… (a checklist; a
// hidden strip stays in the mix), Meter Input(s) Only and Loop, with their keys (Ctrl+Alt+T, Ctrl+Shift+I, Ctrl+L).
Panel {
    id: root
    objectName: "audioMixerPanel"

    /// 0: Audio Track Mixer, 1: Audio Clip Mixer (window.audioTrackMixer / window.audioClipMixer).
    property int view: 0

    KeymapScope.scope: "audioMixer"
    title: root.view === 1 ? qsTr("Audio Clip Mixer") : qsTr("Audio Track Mixer")

    /// 0.7 (card 032): Main.qml's audioMixer.showHideTracks (Ctrl+Alt+T) opens the checklist.
    function openShowHide() {
        root.view = 0
        showHideLoader.active = true
        const popover = showHideLoader.item as ShowHideTracksPopover
        if (popover)
            popover.open()
    }

    leading: [
        PillTabs {
            objectName: "mixerViewTabs"
            compact: true
            model: [qsTr("Tracks"), qsTr("Clips")]
            Accessible.name: qsTr("Mixer view") // P1 #61
            currentIndex: root.view
            onActivated: index => root.view = index
        }
    ]

    // p02 audio: Premiere's Sequence > Add Tracks > Audio Submix Tracks, from the mixer.
    trailing: [
        PillButton {
            id: menuButton
            objectName: "mixerMenuButton"
            visible: root.view === 0
            compact: true
            variant: "ghost"
            iconName: "ellipsis"
            tooltip: AudioMixer.hiddenCount > 0 ? qsTr("Mixer menu: Show/Hide Tracks, Meter Inputs Only, Loop (%n track(s) hidden)", "", AudioMixer.hiddenCount)
                                                : qsTr("Mixer menu: Show/Hide Tracks, Meter Inputs Only, Loop")
            Accessible.name: qsTr("Mixer menu")
            onClicked: panelMenu.popup(menuButton, 0, menuButton.height + Theme.space(1))
            MixerPanelMenu {
                id: panelMenu
                onShowHideRequested: root.openShowHide()
            }
        },
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

                // 0.7 (card 124): the strips of a collapsed track group are folded into one group strip, shown where the
                // group's first strip is (MixerGroupStrip); the other strips of the group take no room.
                Repeater {
                    model: AudioMixer.strips
                    delegate: Item {
                        id: slot
                        required property var modelData
                        readonly property bool folded: (modelData.groupCollapsed ?? false) === true
                        readonly property bool showsGroup: folded && (modelData.groupFirst ?? false) === true
                        // 0.7: Show/Hide Tracks (hidden strips take no room) and folded track groups
                        visible: modelData.hidden !== true && (!folded || showsGroup)
                        width: folded ? (showsGroup ? groupStrip.width : 0) : stripItem.width
                        height: stripRow.height
                        MixerStrip {
                            id: stripItem
                            visible: !slot.folded
                            strip: slot.modelData
                            height: slot.height
                        }
                        Loader {
                            id: groupLoader
                            active: slot.showsGroup
                            height: slot.height
                            sourceComponent: MixerGroupStrip {
                                group: slot.modelData.group ?? ({})
                                height: slot.height
                            }
                        }
                        // The group strip's width, for the slot (a strip's own width otherwise).
                        QtObject {
                            id: groupStrip
                            readonly property real width: (groupLoader.item as Item)?.width ?? Theme.mixerStripWidth
                        }
                    }
                }
                // p02 audio: the submix strips after the tracks', as Premiere lists them.
                Repeater {
                    model: AudioMixer.submixes
                    delegate: SubmixStrip {
                        required property var modelData
                        strip: modelData
                        height: stripRow.height
                        visible: modelData.hidden !== true
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
            // 0.7: every strip hidden - say so and how to bring them back.
            Text {
                objectName: "mixerAllHidden"
                anchors.centerIn: parent
                width: parent.width - Theme.space(8)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: AudioMixer.strips.length > 0 && AudioMixer.hiddenCount >= AudioMixer.strips.length + AudioMixer.submixes.length
                text: qsTr("Every track is hidden. Show/Hide Tracks brings them back")
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

    // 0.7 (card 032): the Show/Hide Tracks checklist, under the menu button's right edge.
    Loader {
        id: showHideLoader
        objectName: "mixerShowHideLoader"
        active: false
        sourceComponent: ShowHideTracksPopover {
            parent: root
            x: root.width - width - Theme.space(3)
            y: Theme.space(10)
        }
    }

    // r07: the settings of a strip's effect (EffectSlots asks with AudioMixer.openEffectSettings). One popup here, over
    // the strips: they are made again whenever the mixer changes, which would close a popup that lived in one.
    Loader {
        id: effectSettingsLoader
        objectName: "mixerEffectSettingsLoader"
        active: false
        sourceComponent: MixerEffectSettings {}
    }
    // 0.7 (card 101): Save effects as preset… from a strip's Presets menu (EffectSlots asks with AudioMixer.openSavePreset).
    Loader {
        id: savePresetLoader
        objectName: "mixerSavePresetLoader"
        active: false
        sourceComponent: SavePresetDialog {}
    }
    Connections {
        target: AudioMixer
        function onSavePresetRequested(target: string, suggestedName: string) {
            savePresetLoader.active = true
            const dialog = savePresetLoader.item as SavePresetDialog
            if (dialog)
                dialog.openForTrack(target, suggestedName)
        }
    }
    Connections {
        target: AudioMixer
        function onEffectSettingsRequested(target: string, effect: string, stripName: string) {
            effectSettingsLoader.active = true
            const popup = effectSettingsLoader.item as MixerEffectSettings
            if (!popup)
                return
            popup.parent = root
            popup.x = Math.max(0, (root.width - popup.implicitWidth) / 2)
            popup.y = Theme.space(12)
            popup.openFor(target, effect, stripName)
        }
    }
}
