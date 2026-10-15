import QtQuick

// The tracker's confidence per frame (Masks.confidence, I.6): one bar per tracked frame, green when the region was
// followed surely, amber when unsure, red where it was lost. Semantic colours carry the meaning, not the beam.
Item {
    id: root
    objectName: "maskConfidenceStrip"

    /// Per frame: 0..1, or -1 for the frame where the picture was lost.
    property var values: []
    /// Frames the track goes through at most: the strip's full width (0 = as many as there are values).
    property int total: 0

    implicitHeight: Theme.space(2)
    Accessible.role: Accessible.Indicator
    Accessible.name: qsTr("Tracking confidence, %n frame(s)", "", values.length)

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusBadge
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: Theme.hairline
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const n = root.values.length
            if (n === 0)
                return
            const w = width / Math.max(n, root.total, 1)
            for (let i = 0; i < n; ++i) {
                const v = root.values[i]
                ctx.fillStyle = v < 0 ? Theme.danger : v >= 0.7 ? Theme.success : v >= 0.4 ? Theme.warning : Theme.danger
                ctx.fillRect(Math.floor(i * w), 0, Math.max(1, Math.ceil(w) - (w > 3 ? 1 : 0)), height)
            }
        }
        Connections {
            target: root
            function onValuesChanged() { canvas.requestPaint() }
            function onTotalChanged() { canvas.requestPaint() }
        }
        onWidthChanged: requestPaint()
    }
}
