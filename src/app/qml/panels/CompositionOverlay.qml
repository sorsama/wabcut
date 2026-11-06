pragma ComponentBehavior: Bound
import QtQuick

// Composition guides over the Program Monitor's picture (0.7; Premiere has none): the rule of thirds, the golden ratio
// (the lines at 38.2 % and 61.8 %) and the frame's two diagonals, each switched on its own (Monitors.compositionGuides:
// View > Rule of Thirds Guides / Golden Ratio Guides / Diagonal Guides, the Program Monitor's wrench menu, the command
// palette). Every line is two-tone, a dark stroke under a light one, so it reads on bright and dark footage. Monitor
// only: never in exports or Export Frame. It takes no input (clicks reach the picture and the overlays below) and hides
// while the Trim view or the multi-camera view shows. Fill the picture's frame with it.
Item {
    id: root
    objectName: "compositionOverlay"

    /// The guides on, in drawing order: "thirds", "golden", "diagonals" (the tests read it).
    readonly property var kinds: {
        const mask = Monitors.compositionGuides
        const out = []
        if (mask & 1)
            out.push("thirds")
        if (mask & 2)
            out.push("golden")
        if (mask & 4)
            out.push("diagonals")
        return out
    }
    /// Where the vertical (and, by the same fractions, the horizontal) lines of a guide fall, as fractions of the frame.
    readonly property var thirds: [1 / 3, 2 / 3]
    readonly property var golden: [0.381966, 0.618034]

    visible: kinds.length > 0 && !Trim.viewOpen && !Multicam.viewOn
    enabled: false

    Canvas {
        id: lines
        objectName: "compositionLines"
        anchors.fill: parent
        readonly property color line: Theme.guide
        readonly property color under: Qt.alpha(Theme.scrim, 0.55)
        readonly property var shown: root.kinds
        onLineChanged: requestPaint()
        onUnderChanged: requestPaint()
        onShownChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = lines.width
            const h = lines.height
            if (w <= 0 || h <= 0)
                return
            // Strokes: the dark one first, then the light one on top of it.
            const strokes = [{ color: lines.under, width: 3 }, { color: lines.line, width: 1 }]
            for (const stroke of strokes) {
                ctx.beginPath()
                for (const kind of lines.shown) {
                    if (kind === "diagonals") {
                        ctx.moveTo(0, 0)
                        ctx.lineTo(w, h)
                        ctx.moveTo(w, 0)
                        ctx.lineTo(0, h)
                        continue
                    }
                    for (const f of kind === "thirds" ? root.thirds : root.golden) {
                        const x = Math.round(f * w) + 0.5
                        const y = Math.round(f * h) + 0.5
                        ctx.moveTo(x, 0)
                        ctx.lineTo(x, h)
                        ctx.moveTo(0, y)
                        ctx.lineTo(w, y)
                    }
                }
                ctx.strokeStyle = stroke.color
                ctx.lineWidth = stroke.width
                ctx.stroke()
            }
        }
    }
}
