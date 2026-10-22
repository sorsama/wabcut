pragma ComponentBehavior: Bound
import QtQuick

// Video Options (P.2, E5): Premiere's Clip > Video Options in the clip menu - Frame Hold Options..., Add Frame Hold,
// Insert Frame Hold Segment, Field Options... (0.2), Scale to Frame Size (a check mark), Set to Frame Size and Set to Fill Frame, on the
// selection (or the clip clicked). Each is one undo step over its typed command (Editing bridge). Its own file,
// importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "videoOptionsSubmenu"

    /// The clip it acts on ("c12").
    property string clipKey: ""
    /// What the clip is (Editing.clipVideoInfo): video, scaleToFrame, frozen.
    property var info: ({})
    readonly property bool hasVideo: menu.info.video === true

    function refresh() {
        menu.info = Editing.clipVideoInfo(menu.clipKey)
    }

    title: qsTr("Video Options")

    ContextMenuItem {
        objectName: "frameHoldOptionsMenuItem"
        text: qsTr("Frame Hold Options…")
        iconName: "pause"
        command: "clip.frameHoldOptions"
        enabled: menu.hasVideo
        onTriggered: Editing.openFrameHoldOptions(menu.clipKey)
    }
    ContextMenuItem {
        objectName: "addFrameHoldMenuItem"
        text: qsTr("Add Frame Hold")
        iconName: "pause"
        command: "clip.addFrameHold"
        enabled: menu.hasVideo
        onTriggered: Editing.addFrameHold(menu.clipKey, false)
    }
    ContextMenuItem {
        objectName: "insertFrameHoldSegmentMenuItem"
        text: qsTr("Insert Frame Hold Segment")
        iconName: "insert-edit"
        command: "clip.insertFrameHoldSegment"
        enabled: menu.hasVideo
        onTriggered: Editing.addFrameHold(menu.clipKey, true)
    }
    ContextMenuItem {
        objectName: "fieldOptionsMenuItem"
        text: qsTr("Field Options…")
        iconName: "film"
        command: "clip.fieldOptions"
        enabled: menu.hasVideo
        onTriggered: Editing.openFieldOptions(menu.clipKey)
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        id: scaleItem
        objectName: "scaleToFrameSizeMenuItem"
        text: qsTr("Scale to Frame Size")
        iconName: "scaling"
        command: "clip.scaleToFrameSize"
        checkable: true
        checked: menu.info.scaleToFrame === true
        enabled: menu.hasVideo
        onTriggered: {
            Editing.scaleToFrameSize(menu.clipKey, menu.info.scaleToFrame !== true)
            menu.refresh()
            scaleItem.checked = Qt.binding(() => menu.info.scaleToFrame === true)
        }
    }
    ContextMenuItem {
        objectName: "setToFrameSizeMenuItem"
        text: qsTr("Set to Frame Size")
        iconName: "crop"
        command: "clip.setToFrameSize"
        enabled: menu.hasVideo
        onTriggered: Editing.setToFrameSize(menu.clipKey)
    }
    ContextMenuItem {
        objectName: "setToFillFrameMenuItem"
        text: qsTr("Set to Fill Frame")
        iconName: "zoom-in"
        command: "clip.setToFillFrame"
        enabled: menu.hasVideo
        onTriggered: Editing.setToFillFrame(menu.clipKey)
    }
}
