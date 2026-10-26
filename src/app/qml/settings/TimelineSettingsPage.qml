pragma ComponentBehavior: Bound
import QtQuick

// Settings > Timeline & editing (0.1.3): the editing preferences (linked selection, the snap line, trim offsets and
// play-around, the Fit Clip answer) and the display ones the Tracks panel's display popover also shows (thumbnails,
// waveforms, markers, keyframe lanes), plus Effect Controls' keyframe lane and the monitors' safe margins. Every
// value is Preferences', so both places stay in step. 0.2: Premiere's Preferences > Media and > Timeline (media scaling,
// still and transition durations, project locking) open in their own dialog (EditingPreferencesDialog).
// 0.4 (stream A): Editing gets the playback auto-scroll (No scroll, Page, Smooth) and "Snap the playhead"; Tracks gets
// the clip speed, the fx badges and the clip tooltips (the display popover shows them too).
// 0.5 (stream B): Editing gets "Play audio while scrubbing" (Playback.audioScrubbing: Shift+S and the Sequence menu's
// Audio Scrubbing are the same switch).
SettingsPage {
    id: page
    objectName: "timelineSettingsPage"

    title: qsTr("Timeline & editing")
    description: qsTr("How edits behave and what the tracks show.")

    readonly property var filmstripModes: ["off", "head", "headTail", "continuous"]
    /// 0.4: Preferences.timelinePlaybackScroll, in the tabs' order.
    readonly property var scrollModes: ["none", "page", "smooth"]
    // UX rework (stream T): the default transitions' pickers ({ id, name } per built-in transition of a kind).
    function transitionOptions(kind: string): var {
        return Editing.transitionChoices(kind).map(c => ({ id: c.id, name: c.name }))
    }
    readonly property var keyframeLanes: ["off", "volume", "pan"]

    SettingsSection {
        title: qsTr("Editing")

        SettingsRow {
            rowKey: "linkedSelection"
            label: qsTr("Linked selection")
            description: qsTr("Clicking a clip selects its linked audio or video too (Alt-click ignores links).")
            Toggle {
                objectName: "settingsLinkedSelection"
                tooltip: qsTr("Linked selection")
                checked: Preferences.linkedSelection
                onToggled: Preferences.linkedSelection = checked
            }
        }
        SettingsRow {
            rowKey: "snapIndicator"
            label: qsTr("Snap line")
            description: qsTr("Draw a line where a drag snaps to an edge, the playhead or a marker.")
            Toggle {
                objectName: "settingsSnapIndicator"
                tooltip: qsTr("Snap line")
                checked: Preferences.snapIndicator
                onToggled: Preferences.snapIndicator = checked
            }
        }
        // 0.4: Premiere's "Snap Playhead in Timeline When Snap Is Enabled" and Timeline Playback Auto-Scrolling.
        SettingsRow {
            rowKey: "snapPlayhead"
            label: qsTr("Snap the playhead")
            description: qsTr("With Snap on, the playhead you drag in the ruler snaps to cuts, markers and In/Out. Shift snaps it with Snap off.")
            Toggle {
                objectName: "settingsSnapPlayhead"
                tooltip: qsTr("Snap the playhead")
                checked: Preferences.snapPlayhead
                onToggled: Preferences.snapPlayhead = checked
            }
        }
        SettingsRow {
            rowKey: "playbackScroll"
            label: qsTr("Playback auto-scroll")
            description: qsTr("How the timeline follows the playhead during playback: Page turns a screen at a time, Smooth keeps the playhead in the middle, No scroll leaves the view alone.")
            PillTabs {
                objectName: "settingsPlaybackScroll"
                width: Theme.space(80)
                compact: true
                model: [qsTr("No scroll"), qsTr("Page"), qsTr("Smooth")]
                currentIndex: Math.max(0, page.scrollModes.indexOf(Preferences.timelinePlaybackScroll))
                onActivated: index => Preferences.timelinePlaybackScroll = page.scrollModes[index]
                Accessible.name: qsTr("Playback auto-scroll")
            }
        }
        SettingsRow {
            rowKey: "trimLargeOffset"
            label: qsTr("Large trim offset")
            description: qsTr("Frames a Shift trim moves (Trim Backward/Forward Many).")
            NumberField {
                objectName: "settingsTrimLargeOffset"
                width: Theme.numberFieldWidth
                from: 2
                to: 100
                value: Preferences.trimLargeOffset
                suffix: qsTr(" fr")
                Accessible.name: qsTr("Large trim offset in frames")
                onValueModified: value => Preferences.trimLargeOffset = Math.round(value)
            }
        }
        SettingsRow {
            rowKey: "playAroundEdit"
            label: qsTr("Play around the edit")
            description: qsTr("Seconds Play Around (Shift+K) plays before and after the playhead, or an edit point in trim mode.")
            NumberField {
                objectName: "settingsTrimPreroll"
                width: Theme.numberFieldWidth
                from: 0
                to: 10
                stepSize: 0.5
                decimals: 1
                value: Preferences.trimPreroll
                suffix: qsTr(" s")
                Accessible.name: qsTr("Preroll in seconds")
                onValueModified: value => Preferences.trimPreroll = value
            }
            NumberField {
                objectName: "settingsTrimPostroll"
                width: Theme.numberFieldWidth
                from: 0
                to: 10
                stepSize: 0.5
                decimals: 1
                value: Preferences.trimPostroll
                suffix: qsTr(" s")
                Accessible.name: qsTr("Postroll in seconds")
                onValueModified: value => Preferences.trimPostroll = value
            }
        }
        // 0.5 (stream B): Premiere's Play Audio While Scrubbing (Toggle Audio Scrubbing, Shift+S); on by default.
        SettingsRow {
            rowKey: "audioScrubbing"
            label: qsTr("Play audio while scrubbing")
            description: qsTr("While stopped, dragging the playhead and stepping frames play a little of the sound there (Shift+S turns it on and off).")
            Toggle {
                objectName: "settingsAudioScrubbing"
                tooltip: qsTr("Play audio while scrubbing")
                checked: Playback.audioScrubbing
                onToggled: {
                    Playback.audioScrubbing = checked
                    checked = Qt.binding(() => Playback.audioScrubbing) // Shift+S and the menu move it too
                }
            }
        }
        SettingsRow {
            rowKey: "fitClip"
            label: qsTr("Four-point edits that do not fit")
            description: Preferences.fitClipChoice === "" ? qsTr("Wabcut asks every time (the Fit Clip dialog).")
                                                          : qsTr("Wabcut uses the answer you chose to always use.")
            PillButton {
                objectName: "settingsFitClipReset"
                visible: Preferences.fitClipChoice !== ""
                compact: true
                iconName: "rotate-ccw"
                text: qsTr("Ask again")
                onClicked: Preferences.fitClipChoice = ""
            }
        }
    }

    SettingsSection {
        title: qsTr("Tracks")

        SettingsRow {
            rowKey: "filmstrip"
            label: qsTr("Video thumbnails")
            PillTabs {
                objectName: "settingsFilmstripMode"
                width: Theme.space(104)
                compact: true
                model: [qsTr("Off"), qsTr("Head"), qsTr("Head and tail"), qsTr("Continuous")]
                currentIndex: Math.max(0, page.filmstripModes.indexOf(Preferences.filmstripMode))
                onActivated: index => Preferences.filmstripMode = page.filmstripModes[index]
                Accessible.name: qsTr("Video thumbnails")
            }
        }
        SettingsRow {
            rowKey: "rectifiedWaveforms"
            label: qsTr("Rectified waveforms")
            description: qsTr("Draw audio from the bottom of the clip up, like Premiere.")
            Toggle {
                tooltip: qsTr("Rectified waveforms")
                checked: Preferences.rectifiedWaveforms
                onToggled: Preferences.rectifiedWaveforms = checked
            }
        }
        SettingsRow {
            rowKey: "logarithmicWaveforms"
            label: qsTr("Logarithmic waveforms")
            description: qsTr("Quiet passages show more detail.")
            Toggle {
                tooltip: qsTr("Logarithmic waveforms")
                checked: Preferences.logarithmicWaveforms
                onToggled: Preferences.logarithmicWaveforms = checked
            }
        }
        SettingsRow {
            rowKey: "throughEdits"
            label: qsTr("Through edits")
            description: qsTr("Mark cuts that continue the same source frames.")
            Toggle {
                tooltip: qsTr("Through edits")
                checked: Preferences.showThroughEdits
                onToggled: Preferences.showThroughEdits = checked
            }
        }
        SettingsRow {
            rowKey: "duplicateFrames"
            label: qsTr("Duplicate frame markers")
            description: qsTr("A coloured band under clips that reuse the same source frames.")
            Toggle {
                tooltip: qsTr("Duplicate frame markers")
                checked: Preferences.showDuplicateFrames
                onToggled: Preferences.showDuplicateFrames = checked
            }
        }
        SettingsRow {
            rowKey: "clipMarkers"
            label: qsTr("Clip markers")
            Toggle {
                tooltip: qsTr("Clip markers")
                checked: Preferences.showClipMarkers
                onToggled: Preferences.showClipMarkers = checked
            }
        }
        // 0.4: what a clip tells about itself on the timeline.
        SettingsRow {
            rowKey: "clipSpeed"
            label: qsTr("Clip speed")
            description: qsTr("The speed after the name of a clip that does not play at 100 % ([50%], [−100%]).")
            Toggle {
                objectName: "settingsClipSpeed"
                tooltip: qsTr("Clip speed")
                checked: Preferences.timelineShowClipSpeed
                onToggled: Preferences.timelineShowClipSpeed = checked
            }
        }
        SettingsRow {
            rowKey: "fxBadges"
            label: qsTr("fx badges")
            description: qsTr("A badge after the name: violet when the clip has effects, amber when Motion, Opacity, Volume or time remapping are changed or keyframed.")
            Toggle {
                objectName: "settingsFxBadges"
                tooltip: qsTr("fx badges")
                checked: Preferences.timelineShowFxBadges
                onToggled: Preferences.timelineShowFxBadges = checked
            }
        }
        SettingsRow {
            rowKey: "clipTooltips"
            label: qsTr("Clip tooltips")
            description: qsTr("Rest the pointer on a clip for its start, end, duration, speed and source.")
            Toggle {
                objectName: "settingsClipTooltips"
                tooltip: qsTr("Clip tooltips")
                checked: Preferences.showClipTooltips
                onToggled: Preferences.showClipTooltips = checked
            }
        }
        SettingsRow {
            rowKey: "audioTrackLane"
            label: qsTr("Audio track lane")
            description: qsTr("The track fader or pan drawn over audio tracks.")
            PillTabs {
                objectName: "settingsTrackKeyframes"
                width: Theme.space(64)
                compact: true
                model: [qsTr("Off"), qsTr("Volume"), qsTr("Pan")]
                currentIndex: Math.max(0, page.keyframeLanes.indexOf(Preferences.trackKeyframes))
                onActivated: index => Preferences.trackKeyframes = page.keyframeLanes[index]
                Accessible.name: qsTr("Audio track lane")
            }
        }
    }

    SettingsSection {
        title: qsTr("Panels")

        SettingsRow {
            rowKey: "keyframeLane"
            label: qsTr("Keyframe lane in Effect Controls")
            description: qsTr("Effect Controls widens to show each parameter's keyframes beside it.")
            Toggle {
                objectName: "settingsKeyframeLane"
                tooltip: qsTr("Keyframe lane in Effect Controls")
                checked: Preferences.keyframeLane
                onToggled: Preferences.keyframeLane = checked
            }
        }
        SettingsRow {
            rowKey: "safeMargins"
            label: qsTr("Safe margins")
            description: qsTr("Action-safe and title-safe guides over the monitors.")
            Toggle {
                tooltip: qsTr("Safe margins")
                checked: Preferences.safeMargins
                onToggled: Preferences.safeMargins = checked
            }
        }
    }

    SettingsSection {
        title: qsTr("Media and timeline defaults")

        SettingsRow {
            rowKey: "editingDefaults"
            label: qsTr("Media scaling, durations and project locking")
            description: qsTr("How imported media is sized in a sequence of another frame size, how long stills and default transitions last, and whether open projects are locked for other sessions.")
            PillButton {
                objectName: "editingPreferencesButton"
                iconName: "sliders-horizontal"
                text: qsTr("Open…")
                onClicked: page.actionRequested("editing")
            }
        }
        // UX rework (stream T): Premiere's Set as Default Transition, also here (Editing.defaultVideoTransition /
        // defaultAudioTransition; the Effects panel's item menu and the timeline transition menu set them too).
        SettingsRow {
            rowKey: "defaultVideoTransition"
            label: qsTr("Default video transition")
            description: qsTr("What Apply Video Transition (Ctrl+D) and Apply Default Transitions (Shift+D) put on an edit.")
            EditingChoice {
                objectName: "settingsDefaultVideoTransition"
                options: page.transitionOptions("video")
                current: Editing.defaultVideoTransition
                tooltip: qsTr("Default video transition")
                Accessible.name: qsTr("Default video transition: %1").arg(text)
                onChosen: id => Editing.setDefaultTransition(id)
            }
        }
        SettingsRow {
            rowKey: "defaultAudioTransition"
            label: qsTr("Default audio transition")
            description: qsTr("What Apply Audio Transition (Ctrl+Shift+D) and Apply Default Transitions put on an edit.")
            EditingChoice {
                objectName: "settingsDefaultAudioTransition"
                options: page.transitionOptions("audio")
                current: Editing.defaultAudioTransition
                tooltip: qsTr("Default audio transition")
                Accessible.name: qsTr("Default audio transition: %1").arg(text)
                onChosen: id => Editing.setDefaultTransition(id)
            }
        }
    }
}
