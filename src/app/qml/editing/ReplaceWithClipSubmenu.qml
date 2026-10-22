pragma ComponentBehavior: Bound
import QtQuick

// Replace With Clip (P.2, E5): Premiere's Sequence > Replace With Clip in the clip menu - From Source Monitor (its clip
// from its In point), From Source Monitor, Match Frame (its frame at the sequence playhead) and From Bin (the Library's
// selected clip). The selected clips keep their place, length, effects and attributes; one undo step (clip.replace).
// Shift-drag from the Source Monitor or Alt-drag from the Library onto a clip does the same.
ContextMenu {
    id: menu
    objectName: "replaceWithClipSubmenu"

    title: qsTr("Replace With Clip")

    ContextMenuItem {
        objectName: "replaceFromSourceMenuItem"
        text: qsTr("From Source Monitor")
        iconName: "overwrite-edit"
        command: "sequence.replaceFromSourceMonitor"
        enabled: SourceMonitor.loaded
        onTriggered: Editing.replaceWithClip("sourceMonitor")
    }
    ContextMenuItem {
        objectName: "replaceMatchFrameMenuItem"
        text: qsTr("From Source Monitor, Match Frame")
        iconName: "crosshair"
        command: "sequence.replaceFromSourceMonitorMatchFrame"
        enabled: SourceMonitor.loaded
        onTriggered: Editing.replaceWithClip("matchFrame")
    }
    ContextMenuItem {
        objectName: "replaceFromBinMenuItem"
        text: qsTr("From Bin")
        iconName: "library"
        command: "sequence.replaceFromBin"
        enabled: Library.mediaSelected
        onTriggered: Editing.replaceWithClip("bin")
    }
}
