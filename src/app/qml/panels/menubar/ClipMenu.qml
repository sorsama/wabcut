pragma ComponentBehavior: Bound
import QtQuick

// Clip (the menu bar, Premiere's order): the clip commands of the keymap, most used first, with Video Options as a
// submenu and the gaming moments at the end. Every row is a keymap command (its shortcut shows; greyed out while it
// cannot run, e.g. with no clip selected); the gaming Moments submenu's three speeds without a command run the same
// preset without a shortcut.
MenuBarMenu {
    id: root
    objectName: "clipMenu"
    title: qsTr("Clip")
    key: "clip"
    mnemonic: "C"

    BarMenuItem {
        text: qsTr("Make Subclip…")
        command: "clip.makeSubclip"
    }
    BarMenuItem {
        text: qsTr("Speed/Duration…")
        command: "clip.speedDuration"
    }
    // 0.5 (stream C): Premiere's Scene Edit Detection… (the dialog; greyed out with the reason without a video clip).
    BarMenuItem {
        text: qsTr("Scene Edit Detection…")
        command: "clip.sceneEditDetection"
    }
    BarMenuItem {
        text: qsTr("Audio Gain…")
        command: "clip.audioGain"
    }
    BarMenuItem {
        text: qsTr("Audio Channels…")
        command: "clip.audioChannels"
    }
    // 0.7 (card 047): the spectrogram of the selected audio clip and the tools that repair a cough, a squeak or a hum in it.
    BarMenuItem {
        objectName: "clipMenuSpectralRepair"
        text: qsTr("Spectral Repair…")
        command: "clip.spectralRepair"
    }
    // 0.7 (card #83): pin a graphic, title or censor bar to something that moves in the shot (Effect Controls' Track Motion).
    BarMenuItem {
        objectName: "clipMenuTrackMotion"
        text: qsTr("Track Motion…")
        command: "clip.trackMotion"
    }
    // 0.7 (card 069): bake a selected nested sequence into a file the clip plays (smooth playback), and the way back.
    BarMenuItem {
        objectName: "clipMenuRenderAndReplace"
        text: qsTr("Render and Replace…")
        command: "clip.renderAndReplace"
    }
    BarMenuItem {
        objectName: "clipMenuRestoreUnrendered"
        text: qsTr("Restore Unrendered")
        command: "clip.restoreUnrendered"
    }
    // 0.6 (P1 round, D9 #8): Detect Beats… on a selected clip with sound (its dialog has the optional BPM range); the
    // row greys out with "Select a clip with sound" otherwise.
    BarMenuItem {
        text: qsTr("Detect Beats…")
        command: "sequence.detectBeats"
    }
    ContextMenuSeparator {}
    MenuBarMenu {
        objectName: "clipMenuVideoOptions"
        title: qsTr("Video Options")
        nested: true
        bar: root.bar

        BarMenuItem {
            text: qsTr("Frame Hold Options…")
            command: "clip.frameHoldOptions"
        }
        BarMenuItem {
            text: qsTr("Add Frame Hold")
            command: "clip.addFrameHold"
        }
        BarMenuItem {
            text: qsTr("Insert Frame Hold Segment")
            command: "clip.insertFrameHoldSegment"
        }
        BarMenuItem {
            text: qsTr("Field Options…")
            command: "clip.fieldOptions"
        }
        ContextMenuSeparator {}
        BarMenuItem {
            text: qsTr("Auto Fit to Frame")
            hint: qsTr("Fits the whole picture into the frame and keeps Motion > Scale at 100 %. Stays fitted if the frame size changes. Premiere: Scale to Frame Size.")
            command: "clip.scaleToFrameSize"
        }
        BarMenuItem {
            text: qsTr("Nearest Neighbor Scaling")
            command: "clip.nearestNeighborScaling"
        }
        BarMenuItem {
            text: qsTr("Set Scale to Fit Frame")
            hint: qsTr("Writes the Motion > Scale that makes the whole picture fit the frame. You can change it afterwards. Premiere: Set to Frame Size.")
            command: "clip.setToFrameSize"
        }
        BarMenuItem {
            text: qsTr("Set Scale to Fill Frame")
            hint: qsTr("Writes the Motion > Scale that makes the picture cover the whole frame; what overhangs is cropped. Premiere: Fill Frame.")
            command: "clip.setToFillFrame"
        }
    }
    // 0.6 (card #74): Final Cut's auditions, a take stack at a cut (the Take Selector panel lists the takes).
    MenuBarMenu {
        objectName: "clipMenuAuditions"
        title: qsTr("Auditions")
        nested: true
        bar: root.bar

        BarMenuItem {
            text: qsTr("New Audition")
            command: "audition.create"
        }
        BarMenuItem {
            text: qsTr("Add Selected Clips to Audition")
            command: "audition.addSelected"
        }
        BarMenuItem {
            text: qsTr("Add Source Monitor Clip to Audition")
            command: "audition.addSource"
        }
        BarMenuItem {
            text: qsTr("Duplicate Take")
            command: "audition.duplicateTake"
        }
        ContextMenuSeparator {}
        BarMenuItem {
            text: qsTr("Next Take")
            command: "audition.nextTake"
        }
        BarMenuItem {
            text: qsTr("Previous Take")
            command: "audition.previousTake"
        }
        ContextMenuSeparator {}
        BarMenuItem {
            text: qsTr("Flatten Audition")
            command: "audition.flatten"
        }
        ContextMenuSeparator {}
        BarMenuItem {
            text: qsTr("Take Selector")
            command: "audition.showTakeSelector"
        }
        BarMenuItem {
            id: takesOverlay
            text: qsTr("Show Takes in Program Monitor")
            command: "audition.toggleOverlay"
            checkable: true
            checked: Auditions.overlayShown
            onTriggered: takesOverlay.checked = Qt.binding(() => Auditions.overlayShown)
        }
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Insert")
        command: "clip.insert"
    }
    BarMenuItem {
        text: qsTr("Overwrite")
        command: "clip.overwrite"
    }
    // 0.7 (card 109): the Program Monitor edit overlay's other zones as commands, for the Source Monitor's clip.
    BarMenuItem {
        objectName: "clipMenuOverlay"
        text: qsTr("Overlay")
        command: "clip.overlayEdit"
    }
    BarMenuItem {
        objectName: "clipMenuInsertBefore"
        text: qsTr("Insert Before Clip at Playhead")
        command: "clip.insertBefore"
    }
    BarMenuItem {
        objectName: "clipMenuInsertAfter"
        text: qsTr("Insert After Clip at Playhead")
        command: "clip.insertAfter"
    }
    BarMenuItem {
        objectName: "clipMenuAppend"
        text: qsTr("Append at End")
        command: "clip.appendEdit"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Enable")
        command: "clip.enable"
    }
    // 0.7 (card 064): locks the selected clips (not their track); the item reads Unlock Clip when they are all locked.
    BarMenuItem {
        objectName: "clipMenuLock"
        text: ProjectController.selectionLocked ? qsTr("Unlock Clip") : qsTr("Lock Clip")
        command: "clip.lock"
    }
    BarMenuItem {
        text: qsTr("Link")
        command: "clip.link"
    }
    BarMenuItem {
        text: qsTr("Group")
        command: "clip.group"
    }
    BarMenuItem {
        text: qsTr("Ungroup")
        command: "clip.ungroup"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Merge Clips…")
        command: "clip.mergeClips"
    }
    BarMenuItem {
        text: qsTr("Interpret Footage…")
        command: "clip.interpretFootage"
    }
    BarMenuItem {
        text: qsTr("Source Settings…")
        command: "clip.rawSourceSettings"
    }
    BarMenuItem {
        text: qsTr("Modify Timecode…")
        command: "clip.modifyTimecode"
    }
    BarMenuItem {
        text: qsTr("Replace Footage…")
        command: "clip.replaceFootage"
    }
    BarMenuItem {
        text: qsTr("Make Offline")
        command: "clip.makeOffline"
    }
    // 0.7 (card 143): read a file that was changed by another program again, in place.
    BarMenuItem {
        objectName: "reloadFootageMenuItem"
        text: qsTr("Reload Footage")
        command: "clip.reloadFootage"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Reveal in Project")
        command: "clip.revealInProject"
    }
    ContextMenuSeparator {}
    // Gaming (M5o): every Moments preset on the clip under the playhead, in one submenu (0.6, D2 #34: each preset is
    // also a keymap command, so a key assigned in Keyboard Shortcuts shows beside it and a row greys out with the reason
    // while the clip cannot take the preset). The speeds without a command of their own (50 %, 200 %, 400 %) run the
    // same Moments preset without a shortcut, as the Tracks header's Moments popover does.
    MenuBarMenu {
        objectName: "clipMenuMoments"
        title: qsTr("Moments")
        nested: true
        bar: root.bar

        BarMenuItem {
            text: qsTr("Slow-mo 25 %")
            command: "moments.slowMo"
        }
        BarMenuItem {
            text: qsTr("Slow-mo 50 %")
            runsCommand: false
            available: Moments.target.speed === true
            disabledReason: Moments.target.speedReason || Moments.target.reason || ""
            onTriggered: Moments.apply("slowMo50")
        }
        ContextMenuSeparator {}
        BarMenuItem {
            text: qsTr("Speed Ramp In")
            command: "moments.rampIn"
        }
        BarMenuItem {
            text: qsTr("Freeze and Replay")
            command: "moments.freezeReplay"
        }
        BarMenuItem {
            text: qsTr("Fast Forward 200 %")
            runsCommand: false
            available: Moments.target.speed === true
            disabledReason: Moments.target.speedReason || Moments.target.reason || ""
            onTriggered: Moments.apply("fastForward200")
        }
        BarMenuItem {
            text: qsTr("Fast Forward 300 %")
            command: "moments.fastForward"
        }
        BarMenuItem {
            text: qsTr("Fast Forward 400 %")
            runsCommand: false
            available: Moments.target.speed === true
            disabledReason: Moments.target.speedReason || Moments.target.reason || ""
            onTriggered: Moments.apply("fastForward400")
        }
        ContextMenuSeparator {}
        BarMenuItem {
            text: qsTr("Punch In")
            command: "moments.punchIn"
        }
        BarMenuItem {
            text: qsTr("Punch In and Hold")
            command: "moments.punchInHold"
        }
        BarMenuItem {
            text: qsTr("Shake")
            command: "moments.shake"
        }
        ContextMenuSeparator {
            visible: Moments.gamingEdits.replay === true || Moments.gamingEdits.version === true
        }
        BarMenuItem {
            visible: Moments.gamingEdits.replay === true
            text: qsTr("Instant Replay")
            command: "moments.instantReplay"
        }
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
    }
}
