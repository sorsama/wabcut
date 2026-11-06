pragma ComponentBehavior: Bound
import QtQuick

// Over the program picture (0.7, Match to a colour chart, docs/color.md): the chart's outline, the middle half of every
// patch that is read (a warning colour where a patch cannot be read), the numbers of the corner patches so the chart's
// orientation can be checked, and four corner handles to drag onto the outer corners of the 24 patches. Fills the
// picture's frame exactly, so every coordinate is a fraction of the canvas. The handles also move with the arrow keys
// (Shift: ten times as far). Only drawn while Color.chart.active; the panel's Color Chart section owns the rest.
Item {
    id: root
    objectName: "colorChartOverlay"

    readonly property bool shown: Color.chart.active ?? false
    readonly property var corners: Color.chart.corners ?? []
    readonly property var quads: Color.chart.quads ?? []
    readonly property var patches: Color.chart.patches ?? []

    visible: shown

    function moveCorner(index: int, x: real, y: real, final: bool): void {
        if (root.corners.length !== 4)
            return
        const next = []
        for (let k = 0; k < 4; ++k) {
            if (k === index)
                next.push([Math.max(-0.2, Math.min(1.2, x / root.width)), Math.max(-0.2, Math.min(1.2, y / root.height))])
            else
                next.push([root.corners[k][0], root.corners[k][1]])
        }
        Color.setChartCorners(next, final)
    }

    Canvas {
        id: canvas
        objectName: "colorChartCanvas"
        anchors.fill: parent
        renderTarget: Canvas.Image
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (!root.shown || root.corners.length !== 4)
                return
            const w = width, h = height
            const outline = (pts, close) => {
                ctx.beginPath()
                for (let i = 0; i < pts.length; ++i) {
                    if (i === 0)
                        ctx.moveTo(pts[i][0] * w, pts[i][1] * h)
                    else
                        ctx.lineTo(pts[i][0] * w, pts[i][1] * h)
                }
                if (close)
                    ctx.closePath()
            }
            // The patches that are read, dark under light so they show on any picture.
            for (let i = 0; i < root.quads.length; ++i) {
                const unreadable = root.patches.length === root.quads.length && !root.patches[i].used
                outline(root.quads[i], true)
                ctx.lineWidth = 3
                ctx.strokeStyle = Theme.scrim
                ctx.stroke()
                ctx.lineWidth = 1.2
                ctx.strokeStyle = unreadable ? Theme.warning : Theme.scrimText
                ctx.stroke()
            }
            // The chart.
            outline(root.corners, true)
            ctx.lineWidth = 3.5
            ctx.strokeStyle = Theme.scrim
            ctx.stroke()
            ctx.lineWidth = 2
            ctx.strokeStyle = Theme.accentStroke
            ctx.stroke()
            // The corner patches' numbers: 1 (dark skin) top-left, 6 top-right, 19 (white) bottom-left, 24 (black) bottom-right.
            ctx.font = "bold " + Theme.textSm + "px sans-serif"
            ctx.textAlign = "center"
            ctx.textBaseline = "middle"
            for (const pair of [[0, "1"], [5, "6"], [18, "19"], [23, "24"]]) {
                const q = root.quads[pair[0]]
                if (!q)
                    continue
                const cx = (q[0][0] + q[2][0]) / 2 * w
                const cy = (q[0][1] + q[2][1]) / 2 * h
                ctx.lineWidth = 3
                ctx.strokeStyle = Theme.scrim
                ctx.strokeText(pair[1], cx, cy)
                ctx.fillStyle = Theme.scrimText
                ctx.fillText(pair[1], cx, cy)
            }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        Connections {
            target: Color
            function onChartChanged(): void { canvas.requestPaint() }
        }
        Connections {
            target: Theme
            function onDarkChanged(): void { canvas.requestPaint() }
        }
    }

    Repeater {
        model: root.shown && root.corners.length === 4 ? 4 : 0

        delegate: Rectangle {
            id: handle

            required property int index
            readonly property var point: root.corners[index] ?? [0.5, 0.5]

            objectName: "chartCorner" + index
            x: point[0] * root.width - width / 2
            y: point[1] * root.height - height / 2
            width: 18
            height: 18
            radius: width / 2
            color: index === 0 ? Theme.accentFill : Theme.scrim
            border.width: 2
            border.color: index === 0 ? Theme.scrim : Theme.accentStroke
            activeFocusOnTab: true
            Accessible.role: Accessible.Slider
            Accessible.name: [qsTr("Chart corner above Dark skin (top left)"), qsTr("Chart corner above Bluish green (top right)"),
                              qsTr("Chart corner below Black (bottom right)"), qsTr("Chart corner below White (bottom left)")][index]

            // The ring shows for keyboard focus only (an Item has no visualFocus: a key press says the keyboard is in use).
            property bool viaKeyboard: false
            onActiveFocusChanged: if (!activeFocus) viaKeyboard = false

            FocusRing {
                visible: handle.activeFocus && handle.viaKeyboard
                targetRadius: handle.radius
            }

            Keys.onPressed: event => {
                handle.viaKeyboard = true
                const step = (event.modifiers & Qt.ShiftModifier) ? 0.01 : 0.001
                let dx = 0, dy = 0
                if (event.key === Qt.Key_Left)
                    dx = -step
                else if (event.key === Qt.Key_Right)
                    dx = step
                else if (event.key === Qt.Key_Up)
                    dy = -step
                else if (event.key === Qt.Key_Down)
                    dy = step
                else
                    return
                event.accepted = true
                root.moveCorner(handle.index, (handle.point[0] + dx) * root.width, (handle.point[1] + dy) * root.height, true)
            }

            MouseArea {
                anchors.fill: parent
                anchors.margins: -8
                cursorShape: Qt.SizeAllCursor
                onPressed: {
                    handle.viaKeyboard = false
                    handle.forceActiveFocus(Qt.MouseFocusReason)
                }
                onPositionChanged: mouse => {
                    if (pressed) {
                        const p = mapToItem(root, mouse.x, mouse.y)
                        root.moveCorner(handle.index, p.x, p.y, false)
                    }
                }
                onReleased: mouse => {
                    const p = mapToItem(root, mouse.x, mouse.y)
                    root.moveCorner(handle.index, p.x, p.y, true)
                }
            }
        }
    }
}
