pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Shapes

// One pane of the Graph Editor (GraphEditorPanel.qml): the value graph ("value") or the speed graph ("speed"). It draws the
// grid, every shown curve in its colour, the keys (their shapes are Effect Controls' lane shapes) and the Bezier handles of
// the selected ones, the playhead; and it turns the pointer into the engine's gestures (KeyframeGraph): a press on a key or
// handle drags it (Shift holds one axis of a key and levels a handle, Alt breaks a handle's pair), a press on nothing draws a
// selection box, the wheel zooms time (Shift: value, Alt: pans), the middle button pans, a double click on a curve adds a key.
Item {
    id: pane

    required property var graph
    /// "value" or "speed".
    required property string kind
    readonly property bool isSpeed: kind === "speed"

    /// The pane's y window (the speed graph always shows all of its range).
    readonly property real y0: isSpeed ? 0 : graph.valueStart
    readonly property real y1: isSpeed ? 1 : graph.valueEnd
    readonly property real x0: graph.viewStart
    readonly property real x1: graph.viewEnd
    readonly property var activeCurve: {
        const curves = graph.curves
        for (let i = 0; i < curves.length; ++i) {
            if (curves[i].active)
                return curves[i]
        }
        return curves.length > 0 ? curves[0] : null
    }
    /// What a key at x holds when the pointer is on it, for the tooltip.
    property string hoverText: ""

    function xToPx(x: real): real { return (x - pane.x0) / (pane.x1 - pane.x0) * pane.width }
    function yToPx(y: real): real { return (y - pane.y0) / (pane.y1 - pane.y0) * pane.height }
    function pxToX(px: real): real { return pane.x0 + px / Math.max(1, pane.width) * (pane.x1 - pane.x0) }
    function pxToY(py: real): real { return pane.y0 + py / Math.max(1, pane.height) * (pane.y1 - pane.y0) }
    function curveColor(index: int): color {
        const hue = Theme.hue(Theme.hueOrder[index % Theme.hueOrder.length])
        return index < Theme.hueOrder.length ? hue : Qt.lighter(hue, 1.45)
    }
    /// A curve's path in pane pixels.
    function pathOf(curve: var): var {
        const flat = pane.isSpeed ? curve.speedPath : curve.path
        const points = []
        for (let i = 0; i + 1 < flat.length; i += 2)
            points.push(Qt.point(pane.xToPx(flat[i]), pane.yToPx(flat[i + 1])))
        return points
    }
    /// A step (1, 2 or 5 times a power of ten) that puts about `count` ticks in `range`.
    function niceStep(range: real, count: real): real {
        if (!(range > 0))
            return 1
        const raw = range / Math.max(1, count)
        const mag = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10))
        const norm = raw / mag
        return (norm < 1.5 ? 1 : norm < 3.5 ? 2 : norm < 7.5 ? 5 : 10) * mag
    }
    function formatNumber(v: real, step: real): string {
        const digits = step >= 1 ? 0 : Math.min(4, Math.ceil(-Math.log(step) / Math.LN10))
        return v.toFixed(digits)
    }

    // The horizontal grid lines and their labels: the active curve's values (value graph) or the speeds (speed graph).
    readonly property var valueTicks: {
        const c = pane.activeCurve
        const out = []
        if (!c || pane.height < 40)
            return out
        if (pane.isSpeed) {
            const speedStep = pane.niceStep(c.speedMax, 2)
            for (const f of [-0.5, -0.25, 0, 0.25, 0.5])
                out.push({ y: 0.5 - f, label: pane.formatNumber(f * 2 * c.speedMax, speedStep) })
            return out
        }
        const range = c.hi - c.lo
        const top = c.hi - pane.y0 * range
        const bottom = c.hi - pane.y1 * range
        const step = pane.niceStep(top - bottom, pane.height / 46)
        for (let v = Math.ceil(bottom / step) * step; v <= top + step * 1e-6; v += step)
            out.push({ y: (c.hi - v) / range, label: pane.formatNumber(v, step) })
        return out
    }
    // The vertical grid lines: whole frames spaced about 90 px apart.
    readonly property var timeTicks: {
        const out = []
        const length = pane.graph.info.lengthSeconds ?? 0
        const frame = pane.graph.info.frameSeconds ?? 0
        if (!(length > 0) || !(frame > 0) || pane.width < 60)
            return out
        const framesShown = (pane.x1 - pane.x0) * length / frame
        const stepFrames = Math.max(1, Math.round(pane.niceStep(framesShown, pane.width / 90)))
        const stepX = stepFrames * frame / length
        for (let x = Math.ceil(pane.x0 / stepX) * stepX; x <= pane.x1 + 1e-9; x += stepX)
            out.push(x)
        return out
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusClip
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border
    }

    // Grid.
    Repeater {
        model: pane.timeTicks
        delegate: Rectangle {
            required property real modelData
            x: Math.round(pane.xToPx(modelData))
            width: Theme.hairline
            height: pane.height
            color: Theme.border
            opacity: 0.55
        }
    }
    Repeater {
        model: pane.valueTicks
        delegate: Item {
            id: tick
            required property var modelData
            y: Math.round(pane.yToPx(modelData.y))
            width: pane.width
            Rectangle {
                width: parent.width
                height: Theme.hairline
                color: tick.modelData.y === 0.5 && pane.isSpeed ? Theme.borderStrong : Theme.border
                opacity: tick.modelData.y === 0.5 && pane.isSpeed ? 1 : 0.55
            }
            Text {
                x: Theme.space(1.5)
                y: -height - 1
                text: tick.modelData.label + (pane.isSpeed && pane.activeCurve ? " " + pane.activeCurve.unit + qsTr("/s") : pane.activeCurve ? " " + pane.activeCurve.unit : "")
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
        }
    }

    // Curves.
    Repeater {
        model: pane.graph.curves
        delegate: Shape {
            id: curveShape
            required property var modelData
            anchors.fill: parent
            layer.enabled: true
            layer.samples: 4
            ShapePath {
                strokeColor: pane.curveColor(curveShape.modelData.color)
                strokeWidth: curveShape.modelData.active ? 2.5 : 1.5
                fillColor: "transparent"
                joinStyle: ShapePath.RoundJoin
                PathPolyline {
                    path: pane.pathOf(curveShape.modelData)
                }
            }
        }
    }

    // Playhead.
    Rectangle {
        objectName: "graphPlayhead_" + pane.kind
        visible: pane.graph.playhead >= 0
        x: Math.round(pane.xToPx(pane.graph.playhead))
        width: 1
        height: pane.height
        color: Theme.accentStroke
        opacity: 0.8
    }

    // Keys and handles.
    Repeater {
        model: pane.graph.curves
        delegate: Item {
            id: curveItem
            required property var modelData
            anchors.fill: parent
            readonly property color tint: pane.curveColor(modelData.color)
            Repeater {
                model: curveItem.modelData.keys
                delegate: Item {
                    id: key
                    required property var modelData
                    readonly property real kx: pane.xToPx(modelData.x)
                    readonly property real ky: pane.yToPx(pane.isSpeed ? modelData.speedY : modelData.y)
                    readonly property bool hasIn: pane.isSpeed ? modelData.sHasIn : modelData.hasIn
                    readonly property bool hasOut: pane.isSpeed ? modelData.sHasOut : modelData.hasOut
                    readonly property real inX: pane.xToPx(pane.isSpeed ? modelData.sInX : modelData.inX)
                    readonly property real inY: pane.yToPx(pane.isSpeed ? modelData.sInY : modelData.inY)
                    readonly property real outX: pane.xToPx(pane.isSpeed ? modelData.sOutX : modelData.outX)
                    readonly property real outY: pane.yToPx(pane.isSpeed ? modelData.sOutY : modelData.outY)
                    readonly property color handleColor: modelData.broken ? Theme.warning : curveItem.tint
                    visible: kx > -12 && kx < pane.width + 12
                    width: pane.width
                    height: pane.height

                    Loader {
                        active: key.hasIn || key.hasOut
                        anchors.fill: parent
                        sourceComponent: Item {
                            Shape {
                                anchors.fill: parent
                                layer.enabled: true
                                layer.samples: 4
                                ShapePath {
                                    strokeColor: key.handleColor
                                    strokeWidth: 1.5
                                    fillColor: "transparent"
                                    startX: key.kx
                                    startY: key.ky
                                    PathLine { x: key.hasIn ? key.inX : key.kx; y: key.hasIn ? key.inY : key.ky }
                                    PathMove { x: key.kx; y: key.ky }
                                    PathLine { x: key.hasOut ? key.outX : key.kx; y: key.hasOut ? key.outY : key.ky }
                                }
                            }
                            Rectangle {
                                objectName: "graphHandleIn"
                                visible: key.hasIn
                                x: key.inX - width / 2
                                y: key.inY - height / 2
                                width: Theme.space(2.5)
                                height: width
                                radius: width / 2
                                color: Theme.sunken
                                border.width: 2
                                border.color: key.handleColor
                            }
                            Rectangle {
                                objectName: "graphHandleOut"
                                visible: key.hasOut
                                x: key.outX - width / 2
                                y: key.outY - height / 2
                                width: Theme.space(2.5)
                                height: width
                                radius: width / 2
                                color: Theme.sunken
                                border.width: 2
                                border.color: key.handleColor
                            }
                        }
                    }
                    // The key sits in a ring of its curve's colour (filled while selected).
                    Rectangle {
                        x: key.kx - width / 2
                        y: key.ky - height / 2
                        width: Theme.space(4.5)
                        height: width
                        radius: width / 2
                        color: key.modelData.selected ? Qt.alpha(curveItem.tint, 0.35) : "transparent"
                        border.width: key.modelData.selected ? 2 : 1.5
                        border.color: curveItem.tint
                    }
                    KeyframeShape {
                        objectName: "graphKey"
                        x: Math.round(key.kx - width / 2)
                        y: Math.round(key.ky - height / 2)
                        shape: key.modelData.shape
                        selected: key.modelData.selected
                    }
                }
            }
        }
    }

    // The selection box.
    Rectangle {
        id: marquee
        objectName: "graphMarquee"
        visible: false
        color: Theme.accentWash
        border.width: Theme.hairline
        border.color: Theme.accentStroke
    }

    MouseArea {
        id: mouse
        objectName: "graphPane_" + pane.kind
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        cursorShape: pressKind === "key" || pressKind === "handle" ? Qt.ClosedHandCursor
                     : panning ? Qt.SizeAllCursor : pane.hoverText !== "" ? Qt.OpenHandCursor : Qt.CrossCursor

        // The gesture in progress: "key", "handle", "box" or "".
        property string pressKind: ""
        property bool panning: false
        property real pressPx: 0
        property real pressPy: 0
        property real pressX: 0
        property real pressY: 0
        property real lastPx: 0
        property real lastPy: 0
        property string lockAxis: ""
        readonly property real tolX: 9 / Math.max(1, pane.width) * (pane.x1 - pane.x0)
        readonly property real tolY: 9 / Math.max(1, pane.height) * (pane.y1 - pane.y0)

        function mods(m: int): int { return m }

        onPressed: mouse => {
            pane.forceActiveFocus(Qt.MouseFocusReason)
            pressPx = lastPx = mouse.x
            pressPy = lastPy = mouse.y
            lockAxis = ""
            if (mouse.button === Qt.MiddleButton) {
                panning = true
                return
            }
            pressX = pane.pxToX(mouse.x)
            pressY = pane.pxToY(mouse.y)
            const hit = pane.graph.press(pane.kind, pressX, pressY, tolX, tolY, mouse.modifiers)
            pressKind = hit === "none" ? "box" : hit
            if (pressKind === "box") {
                marquee.x = mouse.x
                marquee.y = mouse.y
                marquee.width = 0
                marquee.height = 0
                marquee.visible = true
            }
        }
        onPositionChanged: mouse => {
            if (!mouse.buttons) {
                // Hover: what is under the pointer.
                pane.hoverText = pane.graph.describeAt(pane.kind, pane.pxToX(mouse.x), pane.pxToY(mouse.y), tolX, tolY)
                return
            }
            if (panning) {
                const span = pane.x1 - pane.x0
                pane.graph.panTime(-(mouse.x - lastPx) / Math.max(1, pane.width) * span)
                if (!pane.isSpeed)
                    pane.graph.panValue(-(mouse.y - lastPy) / Math.max(1, pane.height) * (pane.y1 - pane.y0))
                lastPx = mouse.x
                lastPy = mouse.y
                return
            }
            if (pressKind === "box") {
                marquee.x = Math.max(0, Math.min(pressPx, mouse.x))
                marquee.y = Math.max(0, Math.min(pressPy, mouse.y))
                marquee.width = Math.min(pane.width, Math.max(pressPx, mouse.x)) - marquee.x
                marquee.height = Math.min(pane.height, Math.max(pressPy, mouse.y)) - marquee.y
                return
            }
            if (pressKind !== "key" && pressKind !== "handle")
                return
            let px = mouse.x
            let py = mouse.y
            if ((mouse.modifiers & Qt.ShiftModifier) && pressKind === "key") {
                // Shift holds one axis of a key: the one it moved along first.
                if (lockAxis === "" && Math.hypot(px - pressPx, py - pressPy) > 5)
                    lockAxis = Math.abs(px - pressPx) >= Math.abs(py - pressPy) ? "x" : "y"
                if (lockAxis === "x")
                    py = pressPy
                else if (lockAxis === "y")
                    px = pressPx
            } else {
                lockAxis = ""
            }
            pane.graph.dragTo(pane.kind, pane.pxToX(px), pane.pxToY(py), mouse.modifiers)
        }
        onReleased: mouse => {
            if (panning) {
                panning = false
                return
            }
            if (pressKind === "key" || pressKind === "handle") {
                pane.graph.release()
            } else if (pressKind === "box") {
                marquee.visible = false
                if (marquee.width < 4 && marquee.height < 4) {
                    if (!(mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)))
                        pane.graph.clearSelection()
                } else {
                    pane.graph.boxSelect(pane.kind, pane.pxToX(marquee.x), pane.pxToY(marquee.y),
                                         pane.pxToX(marquee.x + marquee.width), pane.pxToY(marquee.y + marquee.height), mouse.modifiers)
                }
            }
            pressKind = ""
        }
        onCanceled: {
            if (pressKind === "key" || pressKind === "handle")
                pane.graph.release()
            marquee.visible = false
            pressKind = ""
            panning = false
        }
        onDoubleClicked: mouse => {
            if (!pane.isSpeed && mouse.button === Qt.LeftButton)
                pane.graph.addKeyAt(pane.pxToX(mouse.x), pane.pxToY(mouse.y), tolY)
        }
        onExited: pane.hoverText = ""
        onWheel: wheel => {
            const delta = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y : wheel.angleDelta.x
            if (wheel.modifiers & Qt.AltModifier) {
                pane.graph.panTime(-delta / 120 * 0.06 * (pane.x1 - pane.x0))
            } else if ((wheel.modifiers & Qt.ShiftModifier) && !pane.isSpeed) {
                pane.graph.zoomValue(Math.pow(1.0015, delta), wheel.y / Math.max(1, pane.height))
            } else {
                pane.graph.zoomTime(Math.pow(1.0015, delta), wheel.x / Math.max(1, pane.width))
            }
        }

        ToolTip.visible: pane.hoverText !== "" && !pressed
        ToolTip.text: pane.hoverText
        ToolTip.delay: Theme.tooltipDelay
    }
}
