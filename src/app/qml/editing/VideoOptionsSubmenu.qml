pragma ComponentBehavior: Bound
import QtQuick

// Video Options (P.2, E5): Premiere's Clip > Video Options in the clip menu - Frame Hold Options..., Add Frame Hold,
// Insert Frame Hold Segment, Field Options... (0.2), Auto Fit to Frame (Premiere's Scale to Frame Size; a check mark), Set Scale to Fit Frame (Set to Frame Size) and Set Scale to Fill Frame (Fill Frame), on the
// selection (or the clip clicked). Each is one undo step over its typed command (Editing bridge). Its own file,
// importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "videoOptionsSubmenu"

    /// The clip it acts on ("c12").
    property string clipKey: ""
    /// What the clip is (Editing.clipVideoInfo): video, scaleToFrame, nearestNeighbor, frozen.
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
        text: qsTr("Auto Fit to Frame")
        // 0.7: Premiere's "Scale to Frame Size" and "Set to Frame Size" sound alike; these say what each does.
        hint: qsTr("Fits the whole picture into the frame and keeps Motion > Scale at 100 %. Stays fitted if the frame size changes. Premiere: Scale to Frame Size.")
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
    // 0.7 (card #98): pixel art and retro game footage keep hard pixel edges when scaled up.
    ContextMenuItem {
        id: nearestItem
        objectName: "nearestNeighborMenuItem"
        text: qsTr("Nearest Neighbor Scaling")
        iconName: "scaling"
        command: "clip.nearestNeighborScaling"
        checkable: true
        checked: menu.info.nearestNeighbor === true
        enabled: menu.hasVideo
        onTriggered: {
            Editing.nearestNeighborScaling(menu.clipKey, menu.info.nearestNeighbor !== true)
            menu.refresh()
            nearestItem.checked = Qt.binding(() => menu.info.nearestNeighbor === true)
        }
    }
    ContextMenuItem {
        objectName: "setToFrameSizeMenuItem"
        text: qsTr("Set Scale to Fit Frame")
        hint: qsTr("Writes the Motion > Scale that makes the whole picture fit the frame. You can change it afterwards. Premiere: Set to Frame Size.")
        iconName: "crop"
        command: "clip.setToFrameSize"
        enabled: menu.hasVideo
        onTriggered: Editing.setToFrameSize(menu.clipKey)
    }
    ContextMenuItem {
        objectName: "setToFillFrameMenuItem"
        text: qsTr("Set Scale to Fill Frame")
        hint: qsTr("Writes the Motion > Scale that makes the picture cover the whole frame; what overhangs is cropped. Premiere: Fill Frame.")
        iconName: "zoom-in"
        command: "clip.setToFillFrame"
        enabled: menu.hasVideo
        onTriggered: Editing.setToFillFrame(menu.clipKey)
    }
}
