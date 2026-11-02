pragma ComponentBehavior: Bound
import QtQuick

// Sequence (the menu bar, Premiere's order): settings and rendering, match frame, edits and trims, transitions, lift and
// extract, gaps, replace, subsequences, zoom, snapping and audio scrubbing, selection, captions. Every row is a keymap command (its shortcut shows;
// greyed out while it cannot run). The sequence pill's own menu (switch sequence, Reframe, Rough Cut...) stays as it is.
MenuBarMenu {
    id: root
    objectName: "sequenceMenuBar"
    title: qsTr("Sequence")
    key: "sequence"
    mnemonic: "S"

    BarMenuItem {
        objectName: "sequenceSettingsMenuItem"
        text: qsTr("Sequence Settings…")
        command: "sequence.settings"
        available: ProjectController.sequenceNames.length > 0
    }
    BarMenuItem {
        text: MediaReadiness.rendering ? qsTr("Stop Rendering") : qsTr("Render Effects In to Out")
        command: "sequence.renderInToOut"
    }
    // 0.6 (P1 round, 2A #7 #58): the selected clips' range, and the render files (they ask first, with the size).
    BarMenuItem {
        text: qsTr("Render Selection")
        command: "sequence.renderSelection"
    }
    BarMenuItem {
        text: qsTr("Delete Render Files")
        command: "sequence.deleteRenderFiles"
    }
    BarMenuItem {
        text: qsTr("Delete Render Files In to Out")
        command: "sequence.deleteRenderFilesInToOut"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Match Frame")
        command: "sequence.matchFrame"
    }
    BarMenuItem {
        text: qsTr("Reverse Match Frame")
        command: "sequence.reverseMatchFrame"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Add Edit")
        command: "sequence.addEdit"
    }
    BarMenuItem {
        text: qsTr("Add Edit to All Tracks")
        command: "sequence.addEditToAllTracks"
    }
    BarMenuItem {
        text: qsTr("Trim Edit")
        command: "sequence.trimEdit"
    }
    BarMenuItem {
        text: qsTr("Extend Selected Edit to Playhead")
        command: "sequence.extendEditToPlayhead"
    }
    BarMenuItem {
        text: qsTr("Ripple Trim Previous Edit to Playhead")
        command: "sequence.rippleTrimPreviousToPlayhead"
    }
    BarMenuItem {
        text: qsTr("Ripple Trim Next Edit to Playhead")
        command: "sequence.rippleTrimNextToPlayhead"
    }
    // 0.6 (stream 1A): the non-ripple companions of Q / W (Shift+Q / Shift+W, Ctrl+Alt+Q / Ctrl+Alt+W).
    BarMenuItem {
        text: qsTr("Extend Previous Edit to Playhead")
        command: "sequence.extendPreviousToPlayhead"
    }
    BarMenuItem {
        text: qsTr("Extend Next Edit to Playhead")
        command: "sequence.extendNextToPlayhead"
    }
    BarMenuItem {
        text: qsTr("Trim Previous Edit to Playhead")
        command: "sequence.trimPreviousToPlayhead"
    }
    BarMenuItem {
        text: qsTr("Trim Next Edit to Playhead")
        command: "sequence.trimNextToPlayhead"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Apply Video Transition")
        command: "sequence.applyVideoTransition"
    }
    BarMenuItem {
        text: qsTr("Apply Audio Transition")
        command: "sequence.applyAudioTransition"
    }
    BarMenuItem {
        text: qsTr("Apply Default Transitions to Selection")
        command: "sequence.applyDefaultTransitions"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Lift")
        command: "sequence.lift"
    }
    BarMenuItem {
        text: qsTr("Extract")
        command: "sequence.extract"
    }
    BarMenuItem {
        text: qsTr("Close Gap")
        command: "sequence.closeGap"
    }
    BarMenuItem {
        text: qsTr("Join Through Edits")
        command: "sequence.joinThroughEdits"
    }
    MenuBarMenu {
        objectName: "sequenceMenuReplace"
        title: qsTr("Replace With Clip")
        nested: true
        bar: root.bar

        BarMenuItem {
            text: qsTr("From Source Monitor")
            command: "sequence.replaceFromSourceMonitor"
        }
        BarMenuItem {
            text: qsTr("From Source Monitor, Match Frame")
            command: "sequence.replaceFromSourceMonitorMatchFrame"
        }
        BarMenuItem {
            text: qsTr("From Bin")
            command: "sequence.replaceFromBin"
        }
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Make Subsequence")
        command: "sequence.makeSubsequence"
    }
    BarMenuItem {
        text: qsTr("Make Subsequence from In to Out")
        command: "sequence.makeSubsequenceFromInToOut"
    }
    // 0.6 (P1 round, D2 #9): the In to Out range as a 1080x1920 Short with the Highlights Short options (facecam split,
    // hook title, captions), without a highlight find. Greyed out with "Set In and Out first".
    BarMenuItem {
        objectName: "sequenceMenuMakeShortFromInOut"
        text: qsTr("Make a Short from In to Out")
        command: "sequence.makeShortFromInOut"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Zoom In")
        command: "sequence.zoomIn"
    }
    BarMenuItem {
        text: qsTr("Zoom Out")
        command: "sequence.zoomOut"
    }
    BarMenuItem {
        text: qsTr("Zoom to Sequence")
        command: "sequence.zoomToSequence"
    }
    MenuBarMenu {
        objectName: "sequenceMenuGoToGap"
        title: qsTr("Go to Gap")
        nested: true
        bar: root.bar

        BarMenuItem {
            text: qsTr("Next in Sequence")
            command: "sequence.nextInSequence"
        }
        BarMenuItem {
            text: qsTr("Previous in Sequence")
            command: "sequence.previousInSequence"
        }
    }
    BarMenuItem {
        objectName: "sequenceMenuSnap"
        text: qsTr("Snap in Timeline")
        command: "sequence.snap"
        checkable: true
        checked: Preferences.timelineSnapping
        onTriggered: checked = Qt.binding(() => Preferences.timelineSnapping)
    }
    // 0.5 (stream B): Premiere's Toggle Audio Scrubbing (Shift+S): a dragged playhead and frame steps play the sound there.
    BarMenuItem {
        id: audioScrubbing
        objectName: "sequenceMenuAudioScrubbing"
        text: qsTr("Audio Scrubbing")
        command: "transport.toggleAudioScrubbing"
        checkable: true
        checked: Playback.audioScrubbing
        onTriggered: audioScrubbing.checked = Qt.binding(() => Playback.audioScrubbing)
    }
    // 0.6 (P1 round, D9 #8): Premiere has no beat detection, so this is Wabcut's: the beat grid detected from a clip's
    // music (or set by hand) and what uses it. Detect Beats... opens its BPM range; every row greys out with the reason
    // ("Select a clip with sound", "Detect beats first", "No beat grid to clear").
    ContextMenuSeparator {}
    MenuBarMenu {
        objectName: "sequenceMenuBeatGrid"
        title: qsTr("Beat Grid")
        nested: true
        bar: root.bar

        BarMenuItem {
            text: qsTr("Detect Beats from Selected Clip…")
            command: "sequence.detectBeats"
        }
        BarMenuItem {
            text: qsTr("Snap Selected Clips to Beats")
            command: "edit.snapToBeats"
        }
        BarMenuItem {
            text: qsTr("Cut Selected Clips to Beats…")
            command: "sequence.cutToBeats"
        }
        ContextMenuSeparator {}
        BarMenuItem {
            text: qsTr("Clear Beat Grid")
            command: "sequence.clearBeatGrid"
        }
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Select Clip at Playhead")
        command: "sequence.selectClipAtPlayhead"
    }
    BarMenuItem {
        id: followPlayhead
        objectName: "sequenceMenuSelectionFollowsPlayhead"
        text: qsTr("Selection Follows Playhead")
        command: "sequence.selectionFollowsPlayhead"
        checkable: true
        checked: ProjectController.selectionFollowsPlayhead
        onTriggered: followPlayhead.checked = Qt.binding(() => ProjectController.selectionFollowsPlayhead)
    }
    // 0.6 (P1 round, stream 2E #35, #37): Premiere's caption commands on the shown caption track (Captions), and Correct
    // Transcript from Script… (the sheet). Greyed out with the reason while they cannot run.
    ContextMenuSeparator {}
    MenuBarMenu {
        objectName: "sequenceMenuCaptions"
        title: qsTr("Captions")
        nested: true
        bar: root.bar

        BarMenuItem {
            text: qsTr("Add New Caption Track")
            command: "sequence.addCaptionTrack"
        }
        BarMenuItem {
            text: qsTr("Add Caption at Playhead")
            command: "sequence.addCaptionAtPlayhead"
        }
        ContextMenuSeparator {}
        BarMenuItem {
            text: qsTr("Go to Next Caption Segment")
            command: "sequence.nextCaptionSegment"
        }
        BarMenuItem {
            text: qsTr("Go to Previous Caption Segment")
            command: "sequence.previousCaptionSegment"
        }
        ContextMenuSeparator {}
        BarMenuItem {
            text: qsTr("Correct Transcript from Script…")
            command: "transcript.correctFromScript"
        }
    }
    // 0.6 (P1 round, D2 #34): the gaming sequence actions, each a keymap command (bindable in Keyboard Shortcuts, in
    // the palette, greyed out with the reason): the Shorts and square versions of this sequence, the dead time find with
    // its review in the Highlights panel, the highlight find, and Auto Reframe Sequence… (Premiere's command: the
    // reframe dialog). Moments' clip presets are in Clip > Moments.
    ContextMenuSeparator {}
    MenuBarMenu {
        objectName: "sequenceMenuGaming"
        title: qsTr("Gaming")
        nested: true
        bar: root.bar

        BarMenuItem {
            visible: Moments.gamingEdits.version === true
            text: qsTr("Make Shorts Version")
            command: "moments.shortsVersion"
        }
        BarMenuItem {
            visible: Moments.gamingEdits.version === true
            text: qsTr("Make Square Version")
            command: "moments.squareVersion"
        }
        ContextMenuSeparator {
            visible: Moments.gamingEdits.deadTime === true || Highlights.available
        }
        BarMenuItem {
            visible: Moments.gamingEdits.deadTime === true
            text: qsTr("Find Dead Time…")
            command: "moments.findDeadTime"
        }
        BarMenuItem {
            visible: Highlights.available
            text: qsTr("Find Highlights…")
            command: "moments.findHighlights"
        }
        ContextMenuSeparator {}
        BarMenuItem {
            objectName: "sequenceMenuAutoReframe"
            text: qsTr("Auto Reframe Sequence…")
            command: "sequence.autoReframe"
        }
    }
    // 0.6 (P1 round, 3A #62): the creator autopilot - auto B-roll, the publishing kit, pacing match (the sheet). Hidden
    // while AI is turned off in Settings, like the other AI tools.
    ContextMenuSeparator {
        visible: Preferences.aiEnabled
    }
    BarMenuItem {
        objectName: "sequenceMenuCreatorAutopilot"
        visible: Preferences.aiEnabled
        text: qsTr("Creator Autopilot…")
        command: "sequence.creatorAutopilot"
    }
}
