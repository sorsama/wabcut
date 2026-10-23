pragma ComponentBehavior: Bound
import QtQuick

// Sequence (the menu bar, Premiere's order): settings and rendering, match frame, edits and trims, transitions, lift and
// extract, gaps, replace, subsequences, zoom and snapping, selection. Every row is a keymap command (its shortcut shows;
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
}
