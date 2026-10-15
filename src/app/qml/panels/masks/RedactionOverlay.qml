import QtQuick

// The Source Monitor's redaction tool (I.6, docs/masks.md): over the source picture (fractions of this item are
// fractions of the source frame). It outlines the redactions active at the source position (the picture shows the
// redaction itself), and while the tool is armed (Masks.redactTool) a drag marks a region of the source over its
// In..Out - or the whole media - in every sequence that uses it.
Item {
    id: root
    objectName: "sourceRedactionOverlay"

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.lineWidth = 1.5
            ctx.strokeStyle = Theme.danger
            ctx.setLineDash([5, 3])
            for (let i = 0; i < Masks.activeRegions.length; ++i) {
                const points = Masks.activeRegions[i].points
                if (points.length < 2)
                    continue
                ctx.beginPath()
                ctx.moveTo(points[0].x * width, points[0].y * height)
                for (let k = 1; k < points.length; ++k)
                    ctx.lineTo(points[k].x * width, points[k].y * height)
                ctx.closePath()
                ctx.stroke()
            }
            if (mark.dragging) {
                ctx.setLineDash([])
                ctx.fillStyle = Theme.scrim
                const x = Math.min(mark.x0, mark.x1), y = Math.min(mark.y0, mark.y1)
                const w = Math.abs(mark.x1 - mark.x0), h = Math.abs(mark.y1 - mark.y0)
                ctx.fillRect(x, y, w, h)
                ctx.strokeRect(x, y, w, h)
            }
        }
        Connections {
            target: Masks
            function onRedactionsChanged() { canvas.requestPaint() }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    MouseArea {
        id: mark
        objectName: "sourceRedactionArea"
        anchors.fill: parent
        enabled: Masks.redactTool
        cursorShape: Qt.CrossCursor
        preventStealing: true
        property bool dragging: false
        property real x0: 0
        property real y0: 0
        property real x1: 0
        property real y1: 0
        onPressed: mouse => {
            x0 = x1 = mouse.x
            y0 = y1 = mouse.y
            dragging = true
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            x1 = Math.max(0, Math.min(width, mouse.x))
            y1 = Math.max(0, Math.min(height, mouse.y))
            canvas.requestPaint()
        }
        onReleased: {
            dragging = false
            if (Math.abs(x1 - x0) > 3 && Math.abs(y1 - y0) > 3) {
                if (ObjectMasks.trackRedactions) // AI-14: the object under the box, tracked
                    ObjectMasks.redactObject(x0 / width, y0 / height, x1 / width, y1 / height)
                else
                    Masks.addRedaction(x0 / width, y0 / height, x1 / width, y1 / height)
            }
            canvas.requestPaint()
        }
    }
}
