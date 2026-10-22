pragma ComponentBehavior: Bound
import QtQuick

// P.2 (E6): the Program Monitor's rulers (Premiere's View > Show Rulers, Ctrl+R; bridge Monitors; docs/monitors.md):
// along the top and the left of the monitor, counting the sequence frame's pixels (or percent of it: the ruler's
// menu) from the frame's top-left corner, at the monitor's zoom. Drag from the top ruler down for a horizontal guide,
// from the left ruler for a vertical one (Premiere); a drag that ends outside the frame adds nothing.
Item {
    id: root
    objectName: "programRulers"

    /// The program frame (MonitorPanel's frameArea), in this item's parent coordinates.
    required property Item frame
    readonly property real thickness: 16
    readonly property real fx: frame.x
    readonly property real fy: frame.y
    readonly property real fw: Math.max(1, frame.width)
    readonly property real fh: Math.max(1, frame.height)
    /// The guide a ruler drag is placing (-1 none).
    property int placing: -1

    /// Ruler units per item pixel and the step between labelled ticks (a 1-2-5 series at least ~60 px apart).
    function stepFor(unitsPerPixel) {
        const steps = [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000]
        for (let i = 0; i < steps.length; ++i) {
            if (steps[i] / unitsPerPixel >= 60)
                return steps[i]
        }
        return steps[steps.length - 1]
    }
    function units(vertical) {
        // Units per item pixel along the frame's width (vertical ruler: height).
        if (Monitors.rulerPercent)
            return 100 / (vertical ? root.fh : root.fw)
        return (vertical ? Monitors.frameHeight : Monitors.frameWidth) / (vertical ? root.fh : root.fw)
    }
    function paintRuler(ctx, length, vertical) {
        ctx.reset()
        ctx.fillStyle = Theme.raised
        ctx.fillRect(0, 0, vertical ? root.thickness : length, vertical ? length : root.thickness)
        const upp = root.units(vertical)
        const step = root.stepFor(upp)
        const origin = vertical ? root.fy : root.fx
        const first = Math.ceil(-origin * upp / step) * step
        ctx.strokeStyle = Theme.textTertiary
        ctx.fillStyle = Theme.textSecondary
        ctx.font = "9px '" + Theme.fontMono + "'"
        ctx.lineWidth = 1
        ctx.beginPath()
        for (let v = first - step; v * 1 <= (length - origin) * upp + step; v += step / 5) {
            const p = origin + v / upp
            if (p < 0 || p > length)
                continue
            const major = Math.abs(v / step - Math.round(v / step)) < 1e-6
            const tick = major ? root.thickness : root.thickness / 3
            if (vertical) {
                ctx.moveTo(root.thickness - tick, Math.round(p) + 0.5)
                ctx.lineTo(root.thickness, Math.round(p) + 0.5)
            } else {
                ctx.moveTo(Math.round(p) + 0.5, root.thickness - tick)
                ctx.lineTo(Math.round(p) + 0.5, root.thickness)
            }
            if (major) {
                const label = Monitors.rulerPercent ? Math.round(v) + "%" : String(Math.round(v))
                if (vertical) {
                    ctx.save()
                    ctx.translate(9, p + 2)
                    ctx.rotate(-Math.PI / 2)
                    ctx.fillText(label, 0, 0)
                    ctx.restore()
                } else {
                    ctx.fillText(label, p + 2, 9)
                }
            }
        }
        ctx.stroke()
    }
    function requestPaint() {
        top.requestPaint()
        left.requestPaint()
    }
    /// The frame fraction of an item point along a ruler drag (the guide's position).
    function fractionAt(item, x, y, vertical) {
        const p = item.mapToItem(root, x, y)
        return vertical ? (p.x - root.fx) / root.fw : (p.y - root.fy) / root.fh
    }
    function beginPlacing(vertical, fraction) {
        const value = Monitors.rulerPercent ? fraction * 100
                                            : fraction * (vertical ? Monitors.frameWidth : Monitors.frameHeight)
        root.placing = Monitors.addGuide(vertical ? "vertical" : "horizontal", Math.round(value),
                                         Monitors.rulerPercent ? "percent" : "pixels", "")
    }
    function endPlacing(fraction) {
        if (root.placing >= 0 && (fraction < 0 || fraction > 1))
            Monitors.removeGuide(root.placing)
        root.placing = -1
    }

    onFxChanged: requestPaint()
    onFyChanged: requestPaint()
    onFwChanged: requestPaint()
    onFhChanged: requestPaint()
    Connections {
        target: Monitors
        function onViewChanged() { root.requestPaint() }
        function onGuidesChanged() { root.requestPaint() }
    }

    // The top ruler: drag down for a horizontal guide.
    Canvas {
        id: top
        objectName: "programRulerTop"
        x: root.thickness
        width: parent.width - root.thickness
        height: root.thickness
        onPaint: {
            const ctx = getContext("2d")
            ctx.translate(-root.thickness, 0)
            root.paintRuler(ctx, width + root.thickness, false)
        }
        onWidthChanged: requestPaint()

        MouseArea {
            id: topArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.SplitVCursor
            preventStealing: true
            onPressed: mouse => {
                if (mouse.button === Qt.RightButton) {
                    unitsMenu.popup()
                    return
                }
                root.beginPlacing(false, root.fractionAt(topArea, mouse.x, mouse.y, false))
            }
            onPositionChanged: mouse => {
                if (root.placing >= 0)
                    Monitors.moveGuide(root.placing, root.fractionAt(topArea, mouse.x, mouse.y, false))
            }
            onReleased: mouse => root.endPlacing(root.fractionAt(topArea, mouse.x, mouse.y, false))
        }
    }
    // The left ruler: drag right for a vertical guide.
    Canvas {
        id: left
        objectName: "programRulerLeft"
        y: root.thickness
        width: root.thickness
        height: parent.height - root.thickness
        onPaint: {
            const ctx = getContext("2d")
            ctx.translate(0, -root.thickness)
            root.paintRuler(ctx, height + root.thickness, true)
        }
        onHeightChanged: requestPaint()

        MouseArea {
            id: leftArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.SplitHCursor
            preventStealing: true
            onPressed: mouse => {
                if (mouse.button === Qt.RightButton) {
                    unitsMenu.popup()
                    return
                }
                root.beginPlacing(true, root.fractionAt(leftArea, mouse.x, mouse.y, true))
            }
            onPositionChanged: mouse => {
                if (root.placing >= 0)
                    Monitors.moveGuide(root.placing, root.fractionAt(leftArea, mouse.x, mouse.y, true))
            }
            onReleased: mouse => root.endPlacing(root.fractionAt(leftArea, mouse.x, mouse.y, true))
        }
    }
    // The corner.
    Rectangle {
        width: root.thickness
        height: root.thickness
        color: Theme.raised
    }

    // The ruler's menu (right-click): pixels or percent, and Add Guide.
    ContextMenu {
        id: unitsMenu
        objectName: "rulerUnitsMenu"

        ContextMenuItem {
            text: qsTr("Pixels")
            checkable: true
            checked: !Monitors.rulerPercent
            onTriggered: Monitors.rulerPercent = false
        }
        ContextMenuItem {
            text: qsTr("Percentage")
            checkable: true
            checked: Monitors.rulerPercent
            onTriggered: Monitors.rulerPercent = true
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            text: qsTr("Add Guide…")
            command: "programMonitor.addGuide"
            onTriggered: Monitors.runCommand("programMonitor.addGuide")
        }
    }
}
