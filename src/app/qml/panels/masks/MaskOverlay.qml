pragma ComponentBehavior: Bound
import QtQuick

// Masks on the program monitor (I.6, docs/masks.md): a sibling of the monitor slot that fills the canvas (fractions of
// the canvas are fractions of this item). The Effect Controls target's mask outlines (the selected one solid, the
// others dashed), the selected mask's feather band and handles - centre, size, rotation, path points and their
// tangents, expansion and feather - and the drawing tools: ellipse and rectangle (drag), pen (click for corners, drag
// to pull curves, click the first point to close, Esc cancels). Clicking an outline selects that mask; everything
// else passes through to the monitor.
Item {
    id: root
    objectName: "programMaskOverlay"

    readonly property bool active: Masks.clip !== "" && (Masks.outlines.length > 0 || Masks.tool !== "")
    visible: active

    /// 0.7 (card #56): with Shift the drawn rectangle or ellipse is a square or circle - the far corner moves so both
    /// sides equal the longer one (in monitor pixels, which are canvas pixels), keeping the direction of the drag.
    function constrainedEnd(x0, y0, x1, y1, square) {
        if (!square)
            return Qt.point(x1, y1)
        const dx = x1 - x0, dy = y1 - y0
        const side = Math.max(Math.abs(dx), Math.abs(dy))
        return Qt.point(x0 + (dx < 0 ? -side : side), y0 + (dy < 0 ? -side : side))
    }

    function pointsOf(list) {
        const out = []
        for (let i = 0; i < list.length; ++i)
            out.push(Qt.point(list[i].x * width, list[i].y * height))
        return out
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const line = (points, closed) => {
                if (points.length < 2)
                    return
                ctx.beginPath()
                ctx.moveTo(points[0].x, points[0].y)
                for (let i = 1; i < points.length; ++i)
                    ctx.lineTo(points[i].x, points[i].y)
                if (closed)
                    ctx.closePath()
                ctx.stroke()
            }
            // Feather band of the selected mask.
            ctx.lineWidth = 1
            ctx.strokeStyle = Theme.guide
            ctx.setLineDash([3, 3])
            line(root.pointsOf(Masks.featherOuter), true)
            line(root.pointsOf(Masks.featherInner), true)
            // Outlines: the selected mask solid in the accent, the others dashed.
            for (let i = 0; i < Masks.outlines.length; ++i) {
                const o = Masks.outlines[i]
                ctx.setLineDash(o.selected ? [] : [5, 4])
                ctx.lineWidth = o.selected ? 2 : 1.25
                ctx.strokeStyle = o.selected ? Theme.accentStroke : (o.enabled ? Theme.scrimText : Theme.scrimTextMuted)
                line(root.pointsOf(o.points), true)
            }
            // Tangent lines.
            ctx.setLineDash([])
            ctx.lineWidth = 1
            ctx.strokeStyle = Theme.accentStroke
            for (let i = 0; i < Masks.handles.length; ++i) {
                const h = Masks.handles[i]
                if (h.kind === "in" || h.kind === "out" || h.kind === "rotate" || h.kind === "feather") {
                    ctx.beginPath()
                    ctx.moveTo(h.fromX * width, h.fromY * height)
                    ctx.lineTo(h.x * width, h.y * height)
                    ctx.stroke()
                }
            }
            // The pen's path so far.
            const pen = root.pointsOf(Masks.penPoints)
            ctx.strokeStyle = Theme.accentStroke
            ctx.lineWidth = 1.5
            line(pen, false)
            ctx.fillStyle = Theme.accentFill
            for (let i = 0; i < pen.length; ++i)
                ctx.fillRect(pen[i].x - 3, pen[i].y - 3, 6, 6)
            // A shape being drawn.
            if (draw.dragging) {
                ctx.setLineDash([4, 3])
                ctx.strokeStyle = Theme.accentStroke
                const end = root.constrainedEnd(draw.x0, draw.y0, draw.x1, draw.y1, draw.square)
                const x = Math.min(draw.x0, end.x), y = Math.min(draw.y0, end.y)
                const w = Math.abs(end.x - draw.x0), h = Math.abs(end.y - draw.y0)
                if (Masks.tool === "ellipse") {
                    ctx.beginPath()
                    ctx.ellipse(x, y, w, h)
                    ctx.stroke()
                } else {
                    ctx.strokeRect(x, y, w, h)
                }
            }
        }
        Connections {
            target: Masks
            function onGeometryChanged() { canvas.requestPaint() }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    // Clicking an outline selects its mask (only while no tool is armed; other clicks pass through).
    MouseArea {
        anchors.fill: parent
        enabled: Masks.tool === "" && Masks.outlines.length > 0
        acceptedButtons: Qt.LeftButton
        onPressed: mouse => {
            const key = Masks.maskAt(mouse.x / width, mouse.y / height)
            if (key === "") {
                mouse.accepted = false
                return
            }
            Masks.selected = key
        }
    }

    // Drawing tools.
    MouseArea {
        id: draw
        objectName: "maskDrawArea"
        anchors.fill: parent
        enabled: Masks.tool !== ""
        cursorShape: Qt.CrossCursor
        property bool dragging: false
        property real x0: 0
        property real y0: 0
        property real x1: 0
        property real y1: 0
        property bool penDragged: false
        /// Shift is held: the shape being drawn is a square or circle (the pen ignores it).
        property bool square: false

        onPressed: mouse => {
            root.forceActiveFocus()
            square = (mouse.modifiers & Qt.ShiftModifier) !== 0
            if (Masks.tool === "pen") {
                const pen = Masks.penPoints
                if (pen.length >= 3 && Math.hypot(mouse.x - pen[0].x * width, mouse.y - pen[0].y * height) < 8) {
                    Masks.penClose()
                    return
                }
                Masks.penPoint(mouse.x / width, mouse.y / height)
                penDragged = false
                return
            }
            x0 = x1 = mouse.x
            y0 = y1 = mouse.y
            dragging = true
            canvas.requestPaint()
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            if (Masks.tool === "pen") {
                penDragged = true
                Masks.penTangent(mouse.x / width, mouse.y / height)
                return
            }
            x1 = mouse.x
            y1 = mouse.y
            square = (mouse.modifiers & Qt.ShiftModifier) !== 0
            canvas.requestPaint()
        }
        onReleased: mouse => {
            if (Masks.tool === "pen" || !dragging)
                return
            dragging = false
            square = square || (mouse.modifiers & Qt.ShiftModifier) !== 0
            const end = root.constrainedEnd(x0, y0, mouse.x, mouse.y, square)
            Masks.drawShape(Masks.tool, x0 / width, y0 / height, end.x / width, end.y / height)
            square = false
            canvas.requestPaint()
        }
    }

    // The selected mask's handles.
    Repeater {
        // By count, so a drag's own updates keep the pressed handle alive (the list is rebuilt on every change).
        model: Masks.tool === "" ? Masks.handles.length : 0
        delegate: Rectangle {
            id: handle
            required property int index
            readonly property var modelData: index < Masks.handles.length ? Masks.handles[index] : ({kind: "", index: -1, x: 0, y: 0})
            readonly property bool round: modelData.kind === "in" || modelData.kind === "out" || modelData.kind === "rotate"
                                          || modelData.kind === "feather"
            readonly property real size: modelData.kind === "center" ? 12 : 9
            x: modelData.x * root.width - width / 2
            y: modelData.y * root.height - height / 2
            width: size
            height: size
            radius: round ? size / 2 : Theme.radiusBadge / 2
            color: modelData.kind === "feather" || modelData.kind === "expansion" ? Theme.raised
                   : handleMouse.pressed ? Theme.accentFill : Theme.scrimText
            border.width: 1.5
            border.color: Theme.accentStroke
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Mask handle: %1").arg(modelData.kind)

            MouseArea {
                id: handleMouse
                anchors.fill: parent
                anchors.margins: -4
                cursorShape: handle.modelData.kind === "rotate" ? Qt.PointingHandCursor : Qt.SizeAllCursor
                preventStealing: true
                onPressed: mouse => {
                    const p = mapToItem(root, mouse.x, mouse.y)
                    Masks.beginDrag(handle.modelData.kind, handle.modelData.index, p.x / root.width, p.y / root.height)
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const p = mapToItem(root, mouse.x, mouse.y)
                    Masks.drag(p.x / root.width, p.y / root.height)
                }
                onReleased: Masks.endDrag()
                onCanceled: Masks.endDrag()
            }
        }
    }

    // Shift pressed or let go in the middle of a drag changes the shape at once, without a mouse move.
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Shift && draw.dragging && Masks.tool !== "pen") {
            draw.square = true
            canvas.requestPaint()
        }
    }
    Keys.onReleased: event => {
        if (event.key === Qt.Key_Shift && draw.dragging && Masks.tool !== "pen") {
            draw.square = false
            canvas.requestPaint()
        }
    }

    Keys.onEscapePressed: {
        Masks.penCancel()
        Masks.tool = ""
    }
    Keys.onReturnPressed: {
        if (Masks.tool === "pen")
            Masks.penClose()
    }
}
