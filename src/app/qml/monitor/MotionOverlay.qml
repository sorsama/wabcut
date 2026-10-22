pragma ComponentBehavior: Bound
import QtQuick

// P.2 (E6): Premiere's direct manipulation of Motion on the program monitor (bridge ProgramMotion; docs/monitors.md).
// A sibling of the monitor slot that fills the canvas (fractions of the canvas are fractions of this item), on top of
// the other overlays. The double-click comes from the monitor's pick area (MonitorPanel, 0.1.3 picking): one that does
// not edit a title or a text in place calls engageAt here.
//   * Double-click a clip's picture (or press the direct-manipulation button on Motion in Effect Controls) to show its
//     bounding box: drag inside to move it (Shift: one axis), drag a corner or edge handle to scale it, drag just
//     outside a corner to rotate it (Shift: 45-degree steps), drag the anchor crosshair to move the anchor point with
//     the picture staying put (Alt: the anchor only). An animated Position draws its motion path; drag a keyframe dot
//     to move that keyframe. Moves snap to the frame's edges and centre and to the guides (Ctrl: no snapping).
//   * Crop mode (ProgramMotion.mode "crop": Toggle Crop Direct Manipulation, or Crop's button in Effect Controls): the
//     box is the cropped picture, and its edge and corner handles set the Crop effect's edges.
//   * Esc cancels a drag, else lets go of Motion; a press outside the box lets go and passes through to the monitor.
Item {
    id: root
    objectName: "programMotionOverlay"

    /// Another tool owns the pointer (the Type tool): no double-click engaging.
    property bool suspended: false
    readonly property real grab: 6     // handle grab distance, px
    property string hoverKind: ""

    function snapReach() {
        return width > 0 ? Theme.snapThreshold / width : 0
    }
    function kindAt(x, y) {
        return width > 0 && height > 0 ? ProgramMotion.hitTest(x / width, y / height, grab / width, grab / height) : ""
    }
    function cursorFor(rawKind) {
        const kind = rawKind.startsWith("crop-") ? "scale-" + rawKind.substring(5) : rawKind // crop handles size like scale
        if (kind === "move")
            return Qt.SizeAllCursor
        if (kind === "anchor" || kind.startsWith("key:"))
            return Qt.CrossCursor
        if (kind === "rotate")
            return Qt.OpenHandCursor
        if (kind === "scale-tl" || kind === "scale-br")
            return Qt.SizeFDiagCursor
        if (kind === "scale-tr" || kind === "scale-bl")
            return Qt.SizeBDiagCursor
        if (kind === "scale-t" || kind === "scale-b")
            return Qt.SizeVerCursor
        if (kind === "scale-l" || kind === "scale-r")
            return Qt.SizeHorCursor
        return Qt.ArrowCursor
    }
    /// The double-click (in this item's coordinates): Motion on the clip under the pointer, unless a graphic layer or
    /// another tool takes it.
    function engageAt(x, y) {
        if (root.suspended || width <= 0 || height <= 0)
            return false
        if (GraphicCanvas.active && GraphicCanvas.layerAt(x / width, y / height) !== 0)
            return false // the graphic overlay edits that layer's text
        return ProgramMotion.engageAt(x / width, y / height)
    }

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Motion on the program monitor")
    Accessible.description: ProgramMotion.active ? qsTr("Drag to move, drag a handle to scale, drag outside a corner to rotate")
                                                 : qsTr("Double-click a clip to move, scale or rotate it")

    Canvas {
        id: canvas
        anchors.fill: parent
        visible: ProgramMotion.active
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            // Snapping guides while a move snaps.
            ctx.setLineDash([6, 4])
            ctx.lineWidth = 1
            ctx.strokeStyle = Theme.accentStroke
            for (let i = 0; i < ProgramMotion.guides.length; ++i) {
                const g = ProgramMotion.guides[i]
                ctx.beginPath()
                if (g.orientation === "vertical") {
                    ctx.moveTo(g.position * w, 0)
                    ctx.lineTo(g.position * w, h)
                } else {
                    ctx.moveTo(0, g.position * h)
                    ctx.lineTo(w, g.position * h)
                }
                ctx.stroke()
            }
            ctx.setLineDash([])
            // The motion path: the anchor's track, its keyframes as dots (the one at the playhead filled).
            const path = ProgramMotion.path
            const points = path.points ?? []
            if (points.length >= 4) {
                ctx.lineWidth = 1
                ctx.strokeStyle = Theme.scrimTextMuted
                ctx.setLineDash([2, 3])
                ctx.beginPath()
                ctx.moveTo(points[0] * w, points[1] * h)
                for (let i = 2; i + 1 < points.length; i += 2)
                    ctx.lineTo(points[i] * w, points[i + 1] * h)
                ctx.stroke()
                ctx.setLineDash([])
            }
            const keys = path.keys ?? []
            for (let i = 0; i < keys.length; ++i) {
                const k = keys[i]
                ctx.beginPath()
                ctx.arc(k.x * w, k.y * h, 4, 0, 2 * Math.PI)
                ctx.fillStyle = k.current ? Theme.accentStroke : Theme.scrimText
                ctx.fill()
                ctx.lineWidth = 1
                ctx.strokeStyle = Theme.accentStroke
                ctx.stroke()
            }
            // The bounding box.
            const quad = ProgramMotion.quad
            if (quad.length === 8) {
                ctx.lineWidth = 1.5
                ctx.strokeStyle = Theme.accentStroke
                ctx.beginPath()
                ctx.moveTo(quad[0] * w, quad[1] * h)
                for (let i = 2; i < 8; i += 2)
                    ctx.lineTo(quad[i] * w, quad[i + 1] * h)
                ctx.closePath()
                ctx.stroke()
            }
            // Handles.
            const handles = ProgramMotion.handles
            const size = 8
            for (let i = 0; i < handles.length; ++i) {
                const hd = handles[i]
                ctx.fillStyle = Theme.scrimText
                ctx.fillRect(hd.x * w - size / 2, hd.y * h - size / 2, size, size)
                ctx.lineWidth = 1.5
                ctx.strokeStyle = Theme.accentStroke
                ctx.strokeRect(hd.x * w - size / 2, hd.y * h - size / 2, size, size)
            }
            // The anchor point: a ring with a cross.
            const a = ProgramMotion.anchor
            if (a.x !== undefined) {
                ctx.lineWidth = 1.5
                ctx.strokeStyle = Theme.accentStroke
                ctx.beginPath()
                ctx.arc(a.x * w, a.y * h, 6, 0, 2 * Math.PI)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(a.x * w - 10, a.y * h)
                ctx.lineTo(a.x * w + 10, a.y * h)
                ctx.moveTo(a.x * w, a.y * h - 10)
                ctx.lineTo(a.x * w, a.y * h + 10)
                ctx.stroke()
            }
        }
        Connections {
            target: ProgramMotion
            function onStateChanged() { canvas.requestPaint() }
            function onGuidesChanged() { canvas.requestPaint() }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    // The gestures, while the box is shown (hidden otherwise: a disabled item would still own the cursor).
    MouseArea {
        id: area
        objectName: "programMotionArea"
        anchors.fill: parent
        visible: ProgramMotion.active
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        cursorShape: root.cursorFor(pressed && ProgramMotion.dragging ? dragKind : root.hoverKind)
        property string dragKind: ""

        onPositionChanged: mouse => {
            if (pressed && ProgramMotion.dragging) {
                ProgramMotion.dragTo(mouse.x / width, mouse.y / height, (mouse.modifiers & Qt.ShiftModifier) !== 0,
                                     (mouse.modifiers & Qt.ControlModifier) !== 0, (mouse.modifiers & Qt.AltModifier) !== 0,
                                     root.snapReach())
                return
            }
            root.hoverKind = root.kindAt(mouse.x, mouse.y)
        }
        onExited: root.hoverKind = ""
        onPressed: mouse => {
            const kind = root.kindAt(mouse.x, mouse.y)
            if (kind === "") {
                // Outside the box: let go of Motion, and the press goes on to the monitor.
                ProgramMotion.engaged = false
                mouse.accepted = false
                return
            }
            root.forceActiveFocus()
            dragKind = kind
            ProgramMotion.beginDrag(kind, mouse.x / width, mouse.y / height)
        }
        onReleased: {
            if (ProgramMotion.dragging)
                ProgramMotion.endDrag()
            dragKind = ""
        }
        onCanceled: {
            if (ProgramMotion.dragging)
                ProgramMotion.endDrag()
            dragKind = ""
        }
    }

    Keys.onShortcutOverride: event => {
        event.accepted = ProgramMotion.engaged && event.key === Qt.Key_Escape
    }
    Keys.onPressed: event => {
        if (event.key !== Qt.Key_Escape || !ProgramMotion.engaged)
            return
        if (ProgramMotion.dragging)
            ProgramMotion.cancelDrag()
        else
            ProgramMotion.engaged = false
        event.accepted = true
    }
}
