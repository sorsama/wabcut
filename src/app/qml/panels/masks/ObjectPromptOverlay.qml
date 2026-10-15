pragma ComponentBehavior: Bound
import QtQuick

// The AI Object Mask on the program monitor (AI-14, docs/masks.md "Object masks"): a sibling of the monitor slot that
// fills the canvas (fractions of the canvas are fractions of this item), above the mask overlay. It shows the matte
// preview (the object tinted, or the matte in grey, placed where the clip's frame lies on the canvas) and the object
// mask's prompt on this frame (green + on the object, red - off it, the box). Armed (ObjectMasks.armed), a click adds a
// point - Alt-click or the minus toggle a negative one - and a drag draws a box; Esc disarms. Otherwise everything
// passes through to the monitor.
Item {
    id: root
    objectName: "programObjectOverlay"

    visible: ObjectMasks.armed || ObjectMasks.previewSource !== "" || ObjectMasks.points.length > 0

    // The matte preview, mapped onto the frame's quad on the canvas.
    Image {
        id: preview
        objectName: "objectMattePreview"
        visible: ObjectMasks.previewSource !== "" && ObjectMasks.previewQuad.length === 6
        source: ObjectMasks.previewSource
        width: 1
        height: 1
        smooth: true
        cache: false
        fillMode: Image.Stretch
        transformOrigin: Item.TopLeft
        transform: Matrix4x4 {
            readonly property var q: ObjectMasks.previewQuad
            matrix: q.length === 6 ? Qt.matrix4x4((q[2] - q[0]) * root.width, (q[4] - q[0]) * root.width, 0, q[0] * root.width,
                                                  (q[3] - q[1]) * root.height, (q[5] - q[1]) * root.height, 0, q[1] * root.height,
                                                  0, 0, 1, 0,
                                                  0, 0, 0, 1)
                                   : Qt.matrix4x4()
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const b = ObjectMasks.box
            if (b.x0 !== undefined) {
                ctx.lineWidth = 1.5
                ctx.strokeStyle = Theme.accentStroke
                ctx.setLineDash([6, 3])
                ctx.strokeRect(b.x0 * width, b.y0 * height, (b.x1 - b.x0) * width, (b.y1 - b.y0) * height)
                ctx.setLineDash([])
            }
            if (drag.dragging) {
                ctx.lineWidth = 1.5
                ctx.strokeStyle = Theme.accentStroke
                ctx.fillStyle = Theme.accentWash
                const x = Math.min(drag.x0, drag.x1), y = Math.min(drag.y0, drag.y1)
                ctx.fillRect(x, y, Math.abs(drag.x1 - drag.x0), Math.abs(drag.y1 - drag.y0))
                ctx.strokeRect(x, y, Math.abs(drag.x1 - drag.x0), Math.abs(drag.y1 - drag.y0))
            }
            const r = Theme.space(1.5)
            for (let i = 0; i < ObjectMasks.points.length; ++i) {
                const p = ObjectMasks.points[i]
                const x = p.x * width, y = p.y * height
                ctx.beginPath()
                ctx.arc(x, y, r, 0, 2 * Math.PI)
                ctx.fillStyle = p.positive ? Theme.success : Theme.danger
                ctx.fill()
                ctx.lineWidth = 1.5
                ctx.strokeStyle = Theme.scrimText
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(x - r * 0.55, y)
                ctx.lineTo(x + r * 0.55, y)
                if (p.positive) {
                    ctx.moveTo(x, y - r * 0.55)
                    ctx.lineTo(x, y + r * 0.55)
                }
                ctx.stroke()
            }
        }
        Connections {
            target: ObjectMasks
            function onGeometryChanged() { canvas.requestPaint() }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    MouseArea {
        id: drag
        objectName: "objectPromptArea"
        anchors.fill: parent
        enabled: ObjectMasks.armed && !ObjectMasks.busy
        cursorShape: Qt.CrossCursor
        preventStealing: true
        acceptedButtons: Qt.LeftButton
        property bool dragging: false
        property real x0: 0
        property real y0: 0
        property real x1: 0
        property real y1: 0
        onPressed: mouse => {
            root.forceActiveFocus()
            x0 = x1 = mouse.x
            y0 = y1 = mouse.y
            dragging = false
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            x1 = Math.max(0, Math.min(width, mouse.x))
            y1 = Math.max(0, Math.min(height, mouse.y))
            dragging = Math.abs(x1 - x0) > 4 || Math.abs(y1 - y0) > 4
            canvas.requestPaint()
        }
        onReleased: mouse => {
            if (dragging)
                ObjectMasks.drawBox(x0 / width, y0 / height, x1 / width, y1 / height)
            else
                ObjectMasks.click(mouse.x / width, mouse.y / height, (mouse.modifiers & Qt.AltModifier) ? !ObjectMasks.positive : ObjectMasks.positive)
            dragging = false
            canvas.requestPaint()
        }
    }

    Keys.onEscapePressed: ObjectMasks.armed = false
}
