import QtQuick

// The timeline's view tools over the lanes (0.1.1, the marquee 0.2), as in Premiere:
//   H  Hand: drag scrolls the timeline; nothing is edited
//   Z  Zoom: click zooms in at the click, Alt+click zooms out; drag a marquee to zoom to fit that time range
// An overlay, so a press never reaches the view while one of them is on. The wheel is not taken (no onWheel): it still
// scrolls and zooms as usual. With any other tool it is hidden too, not only disabled (0.1.3): Qt Quick's cursor lookup
// does not skip a disabled item, and a MouseArea always has a cursor (the arrow, or the CursorArea's), which would cover
// the lanes' own.
MouseArea {
    id: root

    required property TimelineView view
    /// "hand" or "zoom" (disabled otherwise).
    property string tool: ""

    /// The marquee being dragged (Zoom tool), in this item's coordinates; empty when none.
    readonly property bool marqueeActive: marquee.visible
    property real pressX: 0
    property real pressY: 0
    property real pressScrollX: 0
    property bool dragged: false
    // Alt over the zoom tool: a click zooms out (the cursor says so).
    property bool altHeld: false

    signal pressedByPointer

    /// Zooms the view so the time between x positions `x0` and `x1` (view coordinates) fills its width.
    function zoomToRange(x0, x1) {
        const v = root.view
        const left = Math.min(x0, x1)
        const right = Math.max(x0, x1)
        const pps = v.pixelsPerSecond
        if (right - left < 2 || pps <= 0 || v.width <= 0)
            return
        const t0 = Math.max(0, (left + v.scrollX) / pps)
        const t1 = (right + v.scrollX) / pps
        v.pixelsPerSecond = v.width / Math.max(1e-6, t1 - t0) // clamped by the view (zoom limits)
        v.scrollX = t0 * v.pixelsPerSecond
    }

    enabled: root.tool === "hand" || root.tool === "zoom"
    visible: enabled
    acceptedButtons: Qt.LeftButton
    hoverEnabled: true

    onPressed: mouse => {
        root.pressedByPointer()
        root.pressX = mouse.x
        root.pressY = mouse.y
        root.pressScrollX = root.view.scrollX
        root.dragged = false
    }
    onPositionChanged: mouse => {
        root.altHeld = (mouse.modifiers & Qt.AltModifier) !== 0
        if (!root.pressed)
            return // hovering
        if (root.tool === "hand") {
            root.view.scrollX = root.pressScrollX - (mouse.x - root.pressX)
            return
        }
        if (!root.dragged
            && Math.abs(mouse.x - root.pressX) + Math.abs(mouse.y - root.pressY) < Application.styleHints.startDragDistance)
            return
        root.dragged = true
        marquee.x = Math.min(root.pressX, mouse.x)
        marquee.y = Math.min(root.pressY, mouse.y)
        marquee.width = Math.abs(mouse.x - root.pressX)
        marquee.height = Math.abs(mouse.y - root.pressY)
        marquee.visible = true
    }
    onReleased: mouse => {
        if (root.tool !== "zoom")
            return
        marquee.visible = false
        if (root.dragged) {
            root.zoomToRange(root.pressX, mouse.x)
            return
        }
        const out = (mouse.modifiers & Qt.AltModifier) !== 0
        root.view.zoomAt(root.view.zoomLevel + (out ? -0.1 : 0.1), mouse.x)
    }
    onCanceled: marquee.visible = false

    // 0.1.3: the tools' own cursors (EditCursors, Boxicons): an open hand, a holding hand while it drags, a magnifier
    // with a plus or (Alt) a minus.
    CursorArea {
        objectName: "timelineViewToolCursor"
        anchors.fill: parent
        shape: !root.enabled ? ""
             : root.tool === "hand" ? (root.pressed ? "grab" : "hand")
             : root.altHeld ? "zoomOut" : "zoomIn"
    }

    Rectangle {
        id: marquee
        objectName: "timelineZoomMarquee"
        visible: false
        color: Theme.accentWash
        border.width: Theme.hairline
        border.color: Theme.accentStroke
    }
}
