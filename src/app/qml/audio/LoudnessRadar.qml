pragma ComponentBehavior: Bound
import QtQuick

// The Loudness Radar (0.7, card 141): one revolution of the short-term loudness (EBU R128 / BS.1770, the 3 s window read once
// a second), drawn like the radar of TC Electronic's and Audition's loudness tools. Time runs clockwise from the top; each
// slot (a second of programme, or a slice of it) is a wedge whose length is its loudness, so a steady programme draws a
// ring and a loud shout or a quiet stretch shows as a spike or a notch. The target is the bright ring; the shaded band is the
// loudness range (the 10th to the 95th percentile of the gated short-term values, EBU Tech 3342). Wedges within 1 LU of the
// target are green, up to 3 LU above it amber and past that red; quieter ones are blue; ticks on the rim mark the quarter turns. `radar` is AudioMixer.radar:
//   live       the sweep (the white spoke) is where the playhead's audio is; the wedges ahead of it are the lap before
//              and fade, the way an old radar trace does;
//   programme  the whole last analysis of the mix in one revolution.
// The scale runs from 30 LU under the target to 10 LU over it.
Item {
    id: root
    objectName: "loudnessRadar"

    property var radar: AudioMixer.radar
    readonly property bool valid: root.radar !== undefined && root.radar.valid === true
    readonly property real target: Number(root.radar?.target ?? -23)
    readonly property real spanBelow: 30
    readonly property real spanAbove: 10

    /// The summary read out by screen readers.
    readonly property string summary: root.valid ? (root.radar.text ?? "") : (root.radar?.text ?? qsTr("No loudness measured yet"))

    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Loudness Radar")
    Accessible.description: root.summary

    implicitWidth: Theme.space(60)
    implicitHeight: Theme.space(60)

    onRadarChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    // Theme changes (light / dark / contrast) repaint it.
    Connections {
        target: Theme
        function onPanelChanged() { canvas.requestPaint() }
    }

    Canvas {
        id: canvas
        objectName: "loudnessRadarCanvas"
        anchors.fill: parent
        renderTarget: Canvas.Image
        antialiasing: true

        function zoneColor(lufs, faded) {
            const t = root.target
            let c = lufs > t + 3 ? Theme.danger : lufs > t + 1 ? Theme.warning : lufs >= t - 1 ? Theme.success : Theme.info
            return faded ? Qt.alpha(c, 0.32) : c
        }

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const cx = width / 2
            const cy = height / 2
            const outer = Math.max(8, Math.min(width, height) / 2 - 18)
            const inner = outer * 0.2
            const total = root.spanBelow + root.spanAbove
            const rOf = lufs => inner + Math.max(0, Math.min(1, (lufs - (root.target - root.spanBelow)) / total)) * (outer - inner)
            const angle = turns => -Math.PI / 2 + turns * 2 * Math.PI

            // The face: a disc and the rings at 6 LU steps from the target.
            ctx.fillStyle = Theme.sunken
            ctx.beginPath()
            ctx.arc(cx, cy, outer, 0, 2 * Math.PI)
            ctx.fill()
            ctx.lineWidth = 1
            ctx.strokeStyle = Theme.border
            for (let lu = -24; lu <= 6; lu += 6) {
                if (lu === 0)
                    continue
                ctx.beginPath()
                ctx.arc(cx, cy, rOf(root.target + lu), 0, 2 * Math.PI)
                ctx.stroke()
            }
            const r = root.radar
            const band = root.valid && r.lowLufs !== null && r.lowLufs !== undefined && r.highLufs !== null && r.highLufs !== undefined
            if (band) {
                // The loudness range: an annulus between the low and the high end.
                // (A thick stroke: Qt's canvas has no even-odd fill.)
                ctx.strokeStyle = Qt.alpha(Theme.accentFill, 0.16)
                ctx.lineWidth = Math.max(1, rOf(r.highLufs) - rOf(r.lowLufs))
                ctx.beginPath()
                ctx.arc(cx, cy, (rOf(r.highLufs) + rOf(r.lowLufs)) / 2, 0, 2 * Math.PI)
                ctx.stroke()
            }
            if (root.valid) {
                const slots = r.slots
                const head = r.head
                for (let i = 0; i < slots; ++i) {
                    const v = r.values[i]
                    if (v === null || v === undefined)
                        continue
                    // Live: the slots after the sweep still show the lap before, faded.
                    const faded = r.mode === "live" && head >= 0 && i > head
                    ctx.fillStyle = zoneColor(v, faded)
                    const a0 = angle(i / slots)
                    const a1 = angle((i + 1) / slots)
                    ctx.beginPath()
                    ctx.arc(cx, cy, rOf(v), a0, a1)
                    ctx.arc(cx, cy, inner, a1, a0, true)
                    ctx.closePath()
                    ctx.fill()
                }
                if (r.mode === "live" && head >= 0) {
                    // The sweep: a spoke at the end of the newest slot.
                    const a = angle((head + 1) / slots)
                    ctx.strokeStyle = Theme.textPrimary
                    ctx.lineWidth = 1.5
                    ctx.beginPath()
                    ctx.moveTo(cx + Math.cos(a) * inner, cy + Math.sin(a) * inner)
                    ctx.lineTo(cx + Math.cos(a) * outer, cy + Math.sin(a) * outer)
                    ctx.stroke()
                }
            }
            // The target ring, over the wedges.
            ctx.strokeStyle = Theme.textPrimary
            ctx.lineWidth = 1.5
            ctx.beginPath()
            ctx.arc(cx, cy, rOf(root.target), 0, 2 * Math.PI)
            ctx.stroke()
            // Ticks on the rim at the quarter turns (the top is the start).
            ctx.strokeStyle = Theme.textTertiary
            ctx.lineWidth = 1
            for (let q = 0; q < 4; ++q) {
                const a = angle(q / 4)
                ctx.beginPath()
                ctx.moveTo(cx + Math.cos(a) * (outer - 5), cy + Math.sin(a) * (outer - 5))
                ctx.lineTo(cx + Math.cos(a) * (outer + 4), cy + Math.sin(a) * (outer + 4))
                ctx.stroke()
            }
            // The hole and the rim.
            ctx.fillStyle = Theme.panel
            ctx.beginPath()
            ctx.arc(cx, cy, inner - 1, 0, 2 * Math.PI)
            ctx.fill()
            ctx.strokeStyle = Theme.borderStrong
            ctx.lineWidth = 1
            ctx.beginPath()
            ctx.arc(cx, cy, outer, 0, 2 * Math.PI)
            ctx.stroke()
        }
    }

    // The scale: the target's value on its ring, and the quarter-hour marks round the rim (time of one revolution).
    Text {
        objectName: "loudnessRadarTarget"
        x: root.width / 2 - width / 2
        y: root.height / 2 - Math.min(root.width, root.height) / 2 + 2
        visible: root.width > Theme.space(30)
        text: qsTr("%1 LUFS").arg(root.target.toFixed(0))
        color: Theme.textSecondary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textXs
    }
    Text {
        objectName: "loudnessRadarEmpty"
        anchors.centerIn: parent
        width: parent.width * 0.5
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        visible: !root.valid
        text: root.radar?.text ?? ""
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
}
