pragma ComponentBehavior: Bound
import QtQuick

// Timeline display settings (M5i; the Tracks header's sliders button): Premiere's thumbnail modes, waveform styles,
// through edits, duplicate frame markers, clip markers, the snap indicator, linked selection, and the track heights
// (expand all, minimise all, presets). Choices apply at once and persist through Preferences. 0.2: the video and audio
// fade handles, sequence label colours on the tabs and audio time units (kept by Monitors, shared with the Program
// Monitor's menu).
Popover {
    id: root
    objectName: "timelineDisplayPopover"

    required property TimelineView view

    title: qsTr("Timeline display")
    transformOrigin: Item.TopLeft

    readonly property var filmstripModes: ["off", "head", "headTail", "continuous"]
    readonly property var presets: {
        try {
            const list = JSON.parse(Preferences.trackHeightPresets)
            return Array.isArray(list) ? list : []
        } catch (e) {
            return []
        }
    }

    function savePreset(name) {
        const trimmed = name.trim()
        if (trimmed === "")
            return
        const preset = root.view.trackHeightPreset()
        const list = root.presets.filter(p => p.name !== trimmed)
        list.push({ name: trimmed, video: preset.video, audio: preset.audio })
        Preferences.trackHeightPresets = JSON.stringify(list)
    }
    function removePreset(name) {
        Preferences.trackHeightPresets = JSON.stringify(root.presets.filter(p => p.name !== name))
    }

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    Column {
        width: Theme.space(80)
        spacing: Theme.space(2)

        GroupLabel { text: qsTr("Video thumbnails") }
        PillTabs {
            objectName: "filmstripModeTabs"
            width: parent.width
            compact: true
            model: [{ label: qsTr("Off") }, { label: qsTr("Head") }, { label: qsTr("Head and tail") },
                    { label: qsTr("Continuous") }]
            currentIndex: Math.max(0, root.filmstripModes.indexOf(Preferences.filmstripMode))
            onActivated: index => Preferences.filmstripMode = root.filmstripModes[index]
            Accessible.name: qsTr("Video thumbnails")
        }

        GroupLabel { text: qsTr("Audio waveforms") }
        Toggle {
            objectName: "rectifiedWaveformsToggle"
            text: qsTr("Rectified")
            tooltip: qsTr("Draw waveforms from the bottom of the clip")
            checked: Preferences.rectifiedWaveforms
            onToggled: Preferences.rectifiedWaveforms = checked
        }
        Toggle {
            objectName: "logarithmicWaveformsToggle"
            text: qsTr("Logarithmic scaling")
            tooltip: qsTr("Quiet passages show more detail")
            checked: Preferences.logarithmicWaveforms
            onToggled: Preferences.logarithmicWaveforms = checked
        }
        Toggle {
            objectName: "volumeWaveformsToggle"
            text: qsTr("Use clip volume")
            tooltip: qsTr("Show audio waveforms using clip volume: scaled by each clip's Audio Gain and Volume keyframes")
            checked: Preferences.volumeWaveforms
            onToggled: Preferences.volumeWaveforms = checked
        }

        GroupLabel { text: qsTr("Audio track keyframes") }
        PillTabs {
            objectName: "trackKeyframesTabs"
            width: parent.width
            compact: true
            readonly property var modes: ["off", "volume", "pan"]
            model: [{ label: qsTr("Off") }, { label: qsTr("Track volume") }, { label: qsTr("Track pan") }]
            currentIndex: Math.max(0, modes.indexOf(Preferences.trackKeyframes))
            onActivated: index => Preferences.trackKeyframes = modes[index]
            Accessible.name: qsTr("Audio track keyframes")
        }

        GroupLabel { text: qsTr("Show") }
        Toggle {
            objectName: "throughEditsToggle"
            text: qsTr("Through edits")
            tooltip: qsTr("Mark cuts between clips that continue the same source")
            checked: Preferences.showThroughEdits
            onToggled: Preferences.showThroughEdits = checked
        }
        Toggle {
            objectName: "duplicateFramesToggle"
            text: qsTr("Duplicate frame markers")
            tooltip: qsTr("A coloured band under video that reuses frames another clip uses")
            checked: Preferences.showDuplicateFrames
            onToggled: Preferences.showDuplicateFrames = checked
        }
        Toggle {
            objectName: "clipMarkersToggle"
            text: qsTr("Clip markers")
            checked: Preferences.showClipMarkers
            onToggled: Preferences.showClipMarkers = checked
        }
        Toggle {
            objectName: "snapIndicatorToggle"
            text: qsTr("Snap indicator")
            tooltip: qsTr("A line where a drag snaps")
            checked: Preferences.snapIndicator
            onToggled: Preferences.snapIndicator = checked
        }
        Toggle {
            objectName: "linkedSelectionToggle"
            text: qsTr("Linked selection")
            tooltip: qsTr("Clicking a clip selects its linked clips; Alt-click selects one")
            checked: Preferences.linkedSelection
            onToggled: Preferences.linkedSelection = checked
        }
        // 0.2: Premiere's Show Video / Audio Fade Handles, Show Sequence Label Color and Show Audio Time Units.
        Toggle {
            objectName: "videoFadeHandlesToggle"
            text: qsTr("Video fade handles")
            tooltip: qsTr("Handles in the top corners of video clips: drag one to fade in or out")
            checked: Preferences.showVideoFadeHandles
            onToggled: Preferences.showVideoFadeHandles = checked
        }
        Toggle {
            objectName: "audioFadeHandlesToggle"
            text: qsTr("Audio fade handles")
            tooltip: qsTr("Handles in the top corners of audio clips: drag one to fade in or out")
            checked: Preferences.showAudioFadeHandles
            onToggled: Preferences.showAudioFadeHandles = checked
        }
        Toggle {
            objectName: "sequenceLabelColorToggle"
            text: qsTr("Sequence label colour")
            tooltip: qsTr("A swatch of each sequence's label colour on its tab; right-click a tab to change it")
            checked: Preferences.showSequenceLabelColor
            onToggled: Preferences.showSequenceLabelColor = checked
        }
        Toggle {
            objectName: "audioTimeUnitsToggle"
            text: qsTr("Audio time units")
            tooltip: qsTr("Count time in audio samples: the playhead and audio clips move between frames")
            checked: Monitors.audioTimeUnits
            onToggled: Monitors.audioTimeUnits = checked
        }

        GroupLabel { text: qsTr("Track height") }
        Row {
            spacing: Theme.space(2)
            PillButton {
                objectName: "expandAllTracksButton"
                compact: true
                iconName: "unfold-vertical"
                text: qsTr("Expand all")
                command: "timeline.expandAllTracks"
                onClicked: root.view.changeTrackHeights("all", 1)
            }
            PillButton {
                objectName: "minimizeAllTracksButton"
                compact: true
                iconName: "fold-vertical"
                text: qsTr("Minimize all")
                command: "timeline.minimizeAllTracks"
                onClicked: root.view.minimizeAllTracks()
            }
            PillButton {
                compact: true
                text: qsTr("Reset")
                onClicked: root.view.resetTrackHeights()
            }
        }
        Repeater {
            model: root.presets
            delegate: Row {
                id: presetRow
                required property var modelData
                spacing: Theme.space(1)
                PillButton {
                    objectName: "trackHeightPreset_" + presetRow.modelData.name
                    compact: true
                    text: presetRow.modelData.name
                    tooltip: qsTr("Apply the track height preset")
                    onClicked: root.view.applyTrackHeightPreset(presetRow.modelData)
                }
                IconButton {
                    compact: true
                    iconName: "trash-2"
                    tooltip: qsTr("Delete the preset %1").arg(presetRow.modelData.name)
                    onClicked: root.removePreset(presetRow.modelData.name)
                }
            }
        }
        Row {
            spacing: Theme.space(2)
            FieldInput {
                id: presetName
                objectName: "trackHeightPresetName"
                width: Theme.space(44)
                placeholderText: qsTr("Preset name")
                Accessible.name: qsTr("Track height preset name")
                onAccepted: {
                    root.savePreset(text)
                    text = ""
                }
            }
            PillButton {
                objectName: "saveTrackHeightPreset"
                compact: true
                text: qsTr("Save preset")
                enabled: presetName.text.trim() !== ""
                onClicked: {
                    root.savePreset(presetName.text)
                    presetName.text = ""
                }
            }
        }
    }
}
