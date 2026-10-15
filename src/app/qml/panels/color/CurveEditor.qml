pragma ComponentBehavior: Bound
import QtQuick

// A curve of the grade (I.5): drag a point to move it, click empty space to add one on the curve, double-click a point
// to remove it. RGB curves (`kind: "rgb"`) map input to output with their end points pinned at x = 0 and 1; hue curves
// (`"hue"`, periodic, a hue strip below) and luma / saturation curves (`"neutral"`) are flat at 0.5 = no change. The
// line is the grade's own evaluation (Color.curveSamples: the same monotone cubic the GPU's table is baked from). One
// drag is one undo step.
Item {
    id: root

    property string name: "master"
    property string kind: "rgb"
    property color lineColor: Theme.textPrimary
    property int samples: 160
    readonly property var stored: Color.curves[name]
    readonly property var points: stored !== undefined ? stored : (kind === "rgb" ? [[0, 0], [1, 1]] : [])
    property var dragPoints: []
    property int dragIndex: -1
    property bool inGesture: false

    implicitHeight: 180

    function toX(v) { return plot.x + v * plot.width }
    function toY(v) { return plot.y + (1 - v) * plot.height }
    function fromX(px) { return Math.max(0, Math.min(1, (px - plot.x) / plot.width)) }
    function fromY(py) { return Math.max(0, Math.min(1, 1 - (py - plot.y) / plot.height)) }
    function curveAt(x) {
        const ys = Color.curveSamples(root.name, 257)
        return ys[Math.round(x * 256)]
    }
    function commit(pts) {
        if (!inGesture) {
            inGesture = true
            Color.beginGesture()
        }
        Color.setCurve(root.name, pts)
    }
    function finish() {
        if (inGesture) {
            inGesture = false
            Color.endGesture()
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusField
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border
    }

    Item {
        id: plot
        x: Theme.space(2)
        y: Theme.space(2)
        width: parent.width - Theme.space(4)
        height: parent.height - Theme.space(4) - (root.kind === "hue" ? 10 : 0)
    }

    // Hue strip under a hue curve: the hues themselves (data, not styling).
    Canvas {
        visible: root.kind === "hue"
        x: plot.x
        y: plot.y + plot.height + 4
        width: plot.width
        height: 6
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const strip = ctx.createLinearGradient(0, 0, width, 0)
            for (let i = 0; i <= 12; ++i)
                strip.addColorStop(i / 12, Qt.hsva((i / 12) % 1, 0.75, 1, 1))
            ctx.fillStyle = strip
            ctx.fillRect(0, 0, width, height)
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        property var line: Color.curves !== undefined ? Color.curveSamples(root.name, root.samples) : []
        onLineChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.lineWidth = 1
            ctx.strokeStyle = Theme.border
            for (let i = 1; i < 4; ++i) {
                ctx.beginPath()
                ctx.moveTo(root.toX(i / 4), plot.y)
                ctx.lineTo(root.toX(i / 4), plot.y + plot.height)
                ctx.moveTo(plot.x, root.toY(i / 4))
                ctx.lineTo(plot.x + plot.width, root.toY(i / 4))
                ctx.stroke()
            }
            // Reference: the diagonal (rgb) or the neutral line.
            ctx.strokeStyle = Theme.guide
            ctx.beginPath()
            if (root.kind === "rgb") {
                ctx.moveTo(root.toX(0), root.toY(0))
                ctx.lineTo(root.toX(1), root.toY(1))
            } else {
                ctx.moveTo(root.toX(0), root.toY(0.5))
                ctx.lineTo(root.toX(1), root.toY(0.5))
            }
            ctx.stroke()
            const ys = line
            if (!ys || ys.length < 2)
                return
            ctx.strokeStyle = root.lineColor
            ctx.lineWidth = 2
            ctx.beginPath()
            for (let i = 0; i < ys.length; ++i) {
                const x = root.toX(i / (ys.length - 1))
                const y = root.toY(Math.max(-0.05, Math.min(1.05, ys[i])))
                if (i === 0)
                    ctx.moveTo(x, y)
                else
                    ctx.lineTo(x, y)
            }
            ctx.stroke()
        }
    }

    Repeater {
        model: root.dragIndex >= 0 ? root.dragPoints : root.points
        delegate: Rectangle {
            required property var modelData
            required property int index
            x: root.toX(modelData[0]) - width / 2
            y: root.toY(modelData[1]) - height / 2
            width: 10
            height: 10
            radius: 5
            color: index === root.dragIndex ? Theme.accentStroke : Theme.raised
            border.width: Theme.focusRingWidth
            border.color: root.lineColor
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.CrossCursor
        function nearest(mx, my) {
            for (let i = 0; i < root.points.length; ++i) {
                const p = root.points[i]
                if (Math.hypot(root.toX(p[0]) - mx, root.toY(p[1]) - my) <= 9)
                    return i
            }
            return -1
        }
        onPressed: mouse => {
            let pts = root.points.map(p => [p[0], p[1]])
            let i = nearest(mouse.x, mouse.y)
            if (i < 0) {
                const x = root.fromX(mouse.x)
                const y = root.kind === "rgb" ? root.curveAt(x) : (root.points.length ? root.curveAt(x) : 0.5)
                pts.push([x, y])
                pts.sort((a, b) => a[0] - b[0])
                i = pts.findIndex(p => p[0] === x)
            }
            root.dragPoints = pts
            root.dragIndex = i
            root.commit(pts)
        }
        onPositionChanged: mouse => {
            if (!pressed || root.dragIndex < 0)
                return
            let pts = root.dragPoints.map(p => [p[0], p[1]])
            const i = root.dragIndex
            const pinned = root.kind === "rgb" && (i === 0 || i === pts.length - 1)
            let x = pinned ? pts[i][0] : root.fromX(mouse.x)
            // Stay between the neighbours (x strictly increasing).
            if (i > 0)
                x = Math.max(x, pts[i - 1][0] + 0.01)
            if (i < pts.length - 1)
                x = Math.min(x, pts[i + 1][0] - 0.01)
            pts[i] = [x, root.fromY(mouse.y)]
            root.dragPoints = pts
            root.commit(pts)
        }
        onReleased: {
            root.dragIndex = -1
            root.finish()
        }
        onCanceled: {
            root.dragIndex = -1
            root.finish()
        }
        onDoubleClicked: mouse => {
            const i = nearest(mouse.x, mouse.y)
            if (i < 0)
                return
            if (root.kind === "rgb" && (i === 0 || i === root.points.length - 1))
                return
            let pts = root.points.map(p => [p[0], p[1]])
            pts.splice(i, 1)
            root.commit(pts)
            root.finish()
        }
    }

    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("%1 curve").arg(root.name)
}
