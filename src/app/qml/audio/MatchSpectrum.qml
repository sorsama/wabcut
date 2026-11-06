pragma ComponentBehavior: Bound
import QtQuick

// The picture of an EQ Match (0.7, card 045): the average spectrum of the reference and of the target in the 30 third-octave
// bands of the Graphic Equalizer (left to right 31 Hz to 25 kHz, so frequency is logarithmic), the target as it will sound
// after the match (the target plus the correction, level-aligned to the reference), and below them the correction itself
// as bars, +-18 dB around a zero line. `bands` is AudioMixer.match.bands: [{ hz, referenceDb, targetDb, gainDb }] (null for a
// band the signal does not reach); `offsetDb` is the level difference EQ Match leaves to Level Match.
Item {
    id: root
    objectName: "matchSpectrum"

    property var bands: []
    property real offsetDb: 0
    readonly property bool hasData: root.bands.length > 0

    implicitWidth: Theme.space(80)
    implicitHeight: Theme.space(46)

    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Spectrum of the reference and the target, and the correction")
    Accessible.description: root.hasData ? qsTr("The correction ranges from %1 to %2 dB")
                                               .arg(root.minGain.toFixed(1)).arg(root.maxGain.toFixed(1))
                                         : qsTr("Nothing learned yet")

    readonly property real minGain: root.bands.reduce((m, b) => b.gainDb === null ? m : Math.min(m, b.gainDb), 0)
    readonly property real maxGain: root.bands.reduce((m, b) => b.gainDb === null ? m : Math.max(m, b.gainDb), 0)

    onBandsChanged: canvas.requestPaint()
    onOffsetDbChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    Connections {
        target: Theme
        function onPanelChanged() { canvas.requestPaint() }
    }

    Canvas {
        id: canvas
        objectName: "matchSpectrumCanvas"
        anchors.fill: parent
        renderTarget: Canvas.Image
        antialiasing: true

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const left = 4
            const right = 4
            const spectraBottom = h * 0.62
            const curveTop = h * 0.68
            const curveBottom = h - 14
            const plotW = Math.max(1, w - left - right)
            const xOf = i => left + plotW * (i + 0.5) / 30
            ctx.fillStyle = Theme.sunken
            ctx.fillRect(0, 0, w, h)
            // Frequency marks: 100 Hz, 1 kHz and 10 kHz.
            ctx.strokeStyle = Theme.border
            ctx.lineWidth = 1
            for (const f of [100, 1000, 10000]) {
                const x = xOf(3 * Math.log(f / 31.25) / Math.LN2)
                ctx.beginPath()
                ctx.moveTo(x, 4)
                ctx.lineTo(x, curveBottom)
                ctx.stroke()
            }
            if (!root.hasData)
                return
            const b = root.bands
            // The level scale of the spectra: from the quietest to the loudest value of the three lines, with a margin.
            let lo = 1e9
            let hi = -1e9
            for (const band of b) {
                for (const v of [band.referenceDb, band.targetDb]) {
                    if (v !== null && v !== undefined) {
                        lo = Math.min(lo, v)
                        hi = Math.max(hi, v)
                    }
                }
            }
            if (!(hi > lo)) {
                lo = -90
                hi = -30
            }
            lo -= 3
            hi += 3
            const yOf = v => 6 + (1 - (v - lo) / (hi - lo)) * (spectraBottom - 6)
            const line = (key, shift, color, width, dash) => {
                ctx.strokeStyle = color
                ctx.lineWidth = width
                ctx.setLineDash(dash)
                ctx.beginPath()
                let started = false
                for (let i = 0; i < b.length; ++i) {
                    const v = key(b[i])
                    if (v === null || v === undefined) {
                        started = false
                        continue
                    }
                    const x = xOf(i)
                    const y = yOf(v + shift)
                    if (started)
                        ctx.lineTo(x, y)
                    else
                        ctx.moveTo(x, y)
                    started = true
                }
                ctx.stroke()
                ctx.setLineDash([])
            }
            line(band => band.targetDb, 0, Theme.textTertiary, 1.5, [])
            line(band => band.targetDb === null || band.gainDb === null ? null : band.targetDb + band.gainDb + root.offsetDb, 0,
                 Qt.alpha(Theme.accentStroke, 0.75), 1.5, [4, 3])
            line(band => band.referenceDb, 0, Theme.accentStroke, 2, [])

            // The correction: bars from the zero line.
            const span = 18
            const zero = (curveTop + curveBottom) / 2
            const scale = (curveBottom - curveTop) / 2 / span
            ctx.strokeStyle = Theme.borderStrong
            ctx.lineWidth = 1
            ctx.beginPath()
            ctx.moveTo(left, zero)
            ctx.lineTo(w - right, zero)
            ctx.stroke()
            const barW = Math.max(2, plotW / 30 * 0.7)
            for (let i = 0; i < b.length; ++i) {
                const g = b[i].gainDb
                if (g === null || g === undefined || Math.abs(g) < 0.05)
                    continue
                ctx.fillStyle = g > 0 ? Theme.hueOrange : Theme.hueBlue
                const y = zero - g * scale
                ctx.fillRect(xOf(i) - barW / 2, Math.min(y, zero), barW, Math.max(1, Math.abs(y - zero)))
            }
        }
    }

    // The scale of the frequency marks and the correction's limits.
    Repeater {
        model: [{ f: 100, label: "100" }, { f: 1000, label: "1k" }, { f: 10000, label: "10k" }]
        delegate: Text {
            required property var modelData
            x: 4 + (root.width - 8) * (3 * Math.log(modelData.f / 31.25) / Math.LN2 + 0.5) / 30 - width / 2
            y: root.height - height - 1
            text: modelData.label
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.space(1)
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 1
        text: qsTr("Hz")
        color: Theme.textTertiary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textXs
    }
    Text {
        anchors.left: parent.left
        anchors.leftMargin: Theme.space(1)
        anchors.top: parent.top
        anchors.topMargin: Theme.space(0.5)
        visible: !root.hasData
        text: qsTr("Learn to see the spectra")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }
}
