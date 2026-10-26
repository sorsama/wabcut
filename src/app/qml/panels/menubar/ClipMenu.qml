pragma ComponentBehavior: Bound
import QtQuick

// Clip (the menu bar, Premiere's order): the clip commands of the keymap, most used first, with Video Options as a
// submenu and the gaming moments at the end. Every row is a keymap command (its shortcut shows; greyed out while it
// cannot run, e.g. with no clip selected).
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
            text: qsTr("Scale to Frame Size")
            command: "clip.scaleToFrameSize"
        }
        BarMenuItem {
            text: qsTr("Set to Frame Size")
            command: "clip.setToFrameSize"
        }
        BarMenuItem {
            text: qsTr("Set to Fill Frame")
            command: "clip.setToFillFrame"
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
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Enable")
        command: "clip.enable"
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
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Reveal in Project")
        command: "clip.revealInProject"
    }
    ContextMenuSeparator {}
    // Gaming (M5o): the moments at the playhead.
    BarMenuItem {
        text: qsTr("Slow-mo at Playhead")
        command: "moments.slowMo"
    }
    BarMenuItem {
        text: qsTr("Punch-in at Playhead")
        command: "moments.punchIn"
    }
}
