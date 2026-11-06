pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// DYNAMICS PROCESSING (0.7, card 044): the graph of Premiere's multiband dynamics, shown by Effect Controls under the
// effect's header. Two pictures of the selected effect and its fields:
//   * the frequency view: the bands side by side on a log frequency scale (20 Hz to 20 kHz) with draggable dividers (the
//     crossovers), each band's threshold as a line you can drag up and down, its gain reduction as a bar hanging from the
//     top, and the input and output meters on either side;
//   * the transfer curve of the selected band: output level against input level over the 1:1 line, with the threshold
//     and the ratio (or, for a gate, the range) as handles to drag; the knee, range and make-up show in the curve.
// Under them: the bands (select, Solo, Bypass), the band's mode, its numeric fields and the master fields. Every value is
// one of the effect's keyframable parameters: an edit goes out as valueEdited(key, value) (a drag merges into one undo
// step until editFinished()); the host puts the values back in `fx`. Arrow keys nudge a focused handle (Shift: coarse).
//
// `fx`: { bands (1..4), crossovers: [Hz x3], band: [{ mode (0 compress, 1 expand, 2 gate, 3 limit), threshold, ratio,
// range, attack, release, knee, makeup, bypass, solo } x4], inputGain, outputGain, mix, limiter, ceiling, gate }.
// `meterProvider()` -> { active, inputDb, outputDb, reduction: [dB x4] } (read while the graph is shown);
// `curveProvider(mode, threshold, ratio, range, knee, makeup, fromDb, toDb, points)` -> output levels of the static curve.
Item {
    id: root
    objectName: "dynamicsGraph"

    property var fx: ({})
    property var presets: []
    property var meterProvider: null
    property var curveProvider: null
    property bool effectEnabled: true
    property int selected: 0
    /// A host with little room (the mixer's popover, which must stay inside the window) sets a height; the graph scrolls when
    /// all of it does not fit. 0: as tall as it is.
    property real maxHeight: 0

    /// A parameter changed by `key` (the effect's own: "band2Threshold", "crossover1", "inputGain", "bandCount", ...).
    signal valueEdited(string key, real value)
    /// A drag or a typed value ended: the host closes the undo step.
    signal editFinished()
    signal presetRequested(string name)

    readonly property int bandCount: Math.max(1, Math.min(4, Math.round(root.fx.bands ?? 4)))
    readonly property var bandColors: [Theme.hueBlue, Theme.hueMint, Theme.hueOrange, Theme.huePink]
    readonly property var modeNames: [qsTr("Compress"), qsTr("Expand"), qsTr("Gate"), qsTr("Limit")]
    /// The two pictures sit side by side from here (an Effect Controls panel is about 540 px wide); narrower they stack.
    readonly property bool wide: root.width >= Theme.space(110)
    readonly property real minDb: -80
    readonly property real maxDb: 0
    /// The value a drag has set and the model has not given back yet (cleared when the edit ends).
    property var local: ({})

    // ---- Values ----
    function cap(s: string): string {
        return s.charAt(0).toUpperCase() + s.slice(1)
    }
    function bandValue(b: int, name: string, fallback: real): real {
        const key = "band" + (b + 1) + root.cap(name)
        if (root.local[key] !== undefined)
            return root.local[key]
        const band = (root.fx.band ?? [])[b]
        const v = band ? band[name] : undefined
        return v === undefined ? fallback : v
    }
    function crossover(i: int): real {
        const key = "crossover" + (i + 1)
        if (root.local[key] !== undefined)
            return root.local[key]
        const list = root.fx.crossovers ?? [120, 1200, 8000]
        return list[i] ?? [120, 1200, 8000][i]
    }
    function masterValue(name: string, fallback: real): real {
        if (root.local[name] !== undefined)
            return root.local[name]
        const v = root.fx[name]
        return v === undefined ? fallback : v
    }
    function edit(key: string, value: real) {
        const next = Object.assign({}, root.local)
        next[key] = value
        root.local = next
        root.valueEdited(key, value)
    }
    function finish() {
        root.local = ({})
        root.editFinished()
    }
    function editBand(b: int, name: string, value: real) {
        root.edit("band" + (b + 1) + root.cap(name), value)
    }
    function clamp(v: real, lo: real, hi: real): real {
        return Math.min(hi, Math.max(lo, v))
    }

    // ---- Scales ----
    readonly property real freqLow: 20
    readonly property real freqHigh: 20000
    function hzOfFraction(f: real): real {
        return root.freqLow * Math.pow(root.freqHigh / root.freqLow, root.clamp(f, 0, 1))
    }
    function fractionOfHz(hz: real): real {
        return Math.log(Math.max(root.freqLow, hz) / root.freqLow) / Math.log(root.freqHigh / root.freqLow)
    }
    function hzText(hz: real): string {
        return hz >= 1000 ? qsTr("%1 kHz").arg((hz / 1000).toFixed(hz >= 10000 ? 0 : 1)) : qsTr("%1 Hz").arg(Math.round(hz))
    }
    /// The edges of band b in Hz.
    function lowEdge(b: int): real {
        return b === 0 ? root.freqLow : root.crossover(b - 1)
    }
    function highEdge(b: int): real {
        return b >= root.bandCount - 1 ? root.freqHigh : root.crossover(b)
    }

    // ---- Meters (the host's provider, once per frame while shown; bars fall at 24 dB a second) ----
    property real inputDb: -90
    property real outputDb: -90
    property var reduction: [0, 0, 0, 0]
    property bool metering: false
    function poll() {
        if (!root.meterProvider)
            return
        const m = root.meterProvider()
        if (!m || m.active !== true) {
            root.metering = false
            if (root.inputDb > -89.9 || root.outputDb > -89.9 || root.reduction.some(r => r > 0.01))
                root.fall(0.033)
            return
        }
        root.metering = true
        const drop = 24 * 0.033
        root.inputDb = Math.max(m.inputDb, root.inputDb - drop)
        root.outputDb = Math.max(m.outputDb, root.outputDb - drop)
        const next = []
        for (let i = 0; i < 4; ++i)
            next.push(Math.max(m.reduction[i] ?? 0, (root.reduction[i] ?? 0) - drop))
        root.reduction = next
    }
    function fall(dt: real) {
        root.inputDb = Math.max(-90, root.inputDb - 24 * dt)
        root.outputDb = Math.max(-90, root.outputDb - 24 * dt)
        root.reduction = root.reduction.map(r => Math.max(0, r - 24 * dt))
    }
    Timer {
        interval: 33
        repeat: true
        running: root.visible && root.meterProvider !== null
        onTriggered: root.poll()
    }

    // ---- The selected band's static curve (output dB at inputs -80 .. 0 dB, one per dB) ----
    readonly property var curve: {
        const b = root.selected
        if (!root.curveProvider)
            return []
        return root.curveProvider(root.bandValue(b, "mode", 0), root.bandValue(b, "threshold", -24), root.bandValue(b, "ratio", 2),
                                  root.bandValue(b, "range", 40), root.bandValue(b, "knee", 6), root.bandValue(b, "makeup", 0),
                                  root.minDb, root.maxDb, 81)
    }

    onSelectedChanged: root.repaintAll()
    function repaintAll() {
        freqCanvas.requestPaint()
        curveCanvas.requestPaint()
    }
    onFxChanged: root.repaintAll()
    onLocalChanged: root.repaintAll()
    onCurveChanged: curveCanvas.requestPaint()
    onInputDbChanged: freqCanvas.requestPaint()
    onOutputDbChanged: freqCanvas.requestPaint()
    onReductionChanged: freqCanvas.requestPaint()
    onWidthChanged: root.repaintAll()
    Connections {
        target: Theme
        function onPanelChanged() { root.repaintAll() }
    }

    readonly property real contentHeight: column.implicitHeight + Theme.space(2)
    implicitWidth: Theme.space(100)
    implicitHeight: root.maxHeight > 0 ? Math.min(root.contentHeight, root.maxHeight) : root.contentHeight
    opacity: root.effectEnabled ? 1 : Theme.disabledOpacity

    // A round handle you drag (and nudge with the arrow keys when it has the focus).
    component Grabber: Rectangle {
        id: grabber
        property string accName: ""
        property color tint: Theme.accentStroke
        property bool horizontal: true
        property bool vertical: true
        readonly property bool pressed: area.pressed
        signal dragged(real px, real py)
        signal nudged(real dx, real dy)
        signal finished()
        width: Theme.space(3)
        height: width
        radius: width / 2
        color: area.pressed || area.containsMouse ? Theme.accentFill : Theme.raised
        border.width: Theme.focusRingWidth
        border.color: grabber.tint
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: grabber.accName
        FocusRing {
            targetRadius: grabber.radius
            visible: grabber.activeFocus
        }
        MouseArea {
            id: area
            anchors.fill: parent
            anchors.margins: -Theme.space(1.5)
            hoverEnabled: true
            cursorShape: grabber.horizontal && grabber.vertical ? Qt.SizeAllCursor : grabber.horizontal ? Qt.SizeHorCursor : Qt.SizeVerCursor
            preventStealing: true
            onPressed: grabber.forceActiveFocus()
            onPositionChanged: mouse => {
                if (area.pressed) {
                    const p = area.mapToItem(grabber.parent, mouse.x, mouse.y)
                    grabber.dragged(p.x, p.y)
                }
            }
            onReleased: grabber.finished()
        }
        Keys.onPressed: event => {
            const step = event.modifiers & Qt.ShiftModifier ? 5 : 1
            let dx = 0
            let dy = 0
            if (event.key === Qt.Key_Left)
                dx = -step
            else if (event.key === Qt.Key_Right)
                dx = step
            else if (event.key === Qt.Key_Up)
                dy = step
            else if (event.key === Qt.Key_Down)
                dy = -step
            else
                return
            event.accepted = true
            grabber.nudged(dx, dy)
            grabber.finished()
        }
    }

    // A labelled number field.
    component ParamField: RowLayout {
        id: field
        property string label: ""
        property string paramKey: ""
        property real value: 0
        property real from: 0
        property real to: 1
        property real step: 1
        property int decimals: 0
        property string suffix: ""
        signal edited(real value)
        signal finished()
        spacing: Theme.space(1)
        Text {
            text: field.label
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        NumberField {
            objectName: "dynamicsField_" + field.paramKey
            value: field.value
            from: field.from
            to: field.to
            stepSize: field.step
            dragStep: (field.to - field.from) / 300
            decimals: field.decimals
            suffix: field.suffix
            implicitWidth: Theme.numberFieldWidth + Theme.space(5)
            Accessible.name: field.label
            onValueModified: v => field.edited(v)
            onEditingFinished: field.finished()
            onScrubFinished: field.finished()
        }
    }

    Flickable {
        id: scroller
        objectName: "dynamicsScroller"
        anchors.fill: parent
        contentWidth: width
        contentHeight: root.contentHeight
        clip: root.maxHeight > 0
        interactive: root.maxHeight > 0 && contentHeight > height
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: column
            y: Theme.space(1)
            width: scroller.width
            spacing: Theme.space(2)

            // ---- Toolbar: presets, the number of bands ----
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                PillButton {
                    id: presetButton
                    objectName: "dynamicsPresets"
                    visible: root.presets.length > 0
                    compact: true
                    iconName: "chevron-down"
                    text: qsTr("Presets")
                    tooltip: qsTr("Start from a typical setting of the bands")
                    onClicked: presetMenu.popup(presetButton, 0, presetButton.height + Theme.space(1))
                    OptionsMenu {
                        id: presetMenu
                        options: root.presets.map(p => ({ id: p, name: p }))
                        onChosen: id => root.presetRequested(id)
                    }
                }
                RowLayout {
                    spacing: Theme.space(1.5)
                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: qsTr("Bands")
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    PillTabs {
                        objectName: "dynamicsBandCount"
                        compact: true
                        model: ["1", "2", "3", "4"]
                        currentIndex: root.bandCount - 1
                        Accessible.name: qsTr("Number of bands")
                        onActivated: index => {
                            root.valueEdited("bandCount", index + 1)
                            root.finish()
                            if (root.selected > index)
                                root.selected = index
                        }
                    }
                }
            }

            // ---- The bands: select, Solo, Bypass ----
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1.5)
                Repeater {
                    model: root.bandCount
                    delegate: Rectangle {
                        id: chip
                        required property int index
                        readonly property bool current: chip.index === root.selected
                        readonly property color tint: root.bandColors[chip.index]
                        objectName: "dynamicsBand_" + (chip.index + 1)
                        width: chipRow.implicitWidth + Theme.space(3)
                        height: Theme.controlHeight
                        radius: height / 2
                        color: chip.current ? Qt.alpha(chip.tint, 0.22) : Theme.raised
                        border.width: chip.current ? Theme.selectionWidth : Theme.hairline
                        border.color: chip.current ? chip.tint : Theme.border
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Band %1, %2 to %3").arg(chip.index + 1).arg(root.hzText(root.lowEdge(chip.index)))
                                             .arg(root.hzText(root.highEdge(chip.index)))
                        Accessible.onPressAction: root.selected = chip.index
                        activeFocusOnTab: true
                        Keys.onSpacePressed: root.selected = chip.index
                        Keys.onReturnPressed: root.selected = chip.index
                        FocusRing {
                            targetRadius: chip.radius
                            visible: chip.activeFocus
                        }
                        TapHandler {
                            onTapped: root.selected = chip.index
                        }
                        RowLayout {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: Theme.space(1.5)
                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                Layout.preferredWidth: Theme.dotSize
                                Layout.preferredHeight: Theme.dotSize
                                radius: Theme.dotSize / 2
                                color: chip.tint
                            }
                            Text {
                                text: qsTr("%1  %2 to %3").arg(chip.index + 1).arg(root.hzText(root.lowEdge(chip.index)))
                                          .arg(root.hzText(root.highEdge(chip.index)))
                                color: chip.current ? Theme.textPrimary : Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                            Text {
                                visible: (root.reduction[chip.index] ?? 0) > 0.05
                                text: qsTr("-%1 dB").arg((root.reduction[chip.index] ?? 0).toFixed(1))
                                color: Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textXs
                            }
                            IconButton {
                                objectName: "dynamicsSolo_" + (chip.index + 1)
                                compact: true
                                checkable: true
                                checked: root.bandValue(chip.index, "solo", 0) >= 0.5
                                iconName: "headphones"
                                tooltip: qsTr("Solo band %1: hear only this band").arg(chip.index + 1)
                                Accessible.name: qsTr("Solo band %1").arg(chip.index + 1)
                                onClicked: {
                                    root.editBand(chip.index, "solo", checked ? 1 : 0)
                                    root.finish()
                                }
                            }
                            IconButton {
                                objectName: "dynamicsBypass_" + (chip.index + 1)
                                compact: true
                                checkable: true
                                checked: root.bandValue(chip.index, "bypass", 0) >= 0.5
                                iconName: "power"
                                tooltip: qsTr("Bypass band %1: it passes untouched").arg(chip.index + 1)
                                Accessible.name: qsTr("Bypass band %1").arg(chip.index + 1)
                                onClicked: {
                                    root.editBand(chip.index, "bypass", checked ? 1 : 0)
                                    root.finish()
                                }
                            }
                        }
                    }
                }
            }

            // ---- The two pictures ----
            GridLayout {
                Layout.fillWidth: true
                columns: root.wide ? 2 : 1
                columnSpacing: Theme.space(2)
                rowSpacing: Theme.space(2)

                // The frequency view.
                Item {
                    id: freqArea
                    objectName: "dynamicsFrequencyView"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    Layout.minimumWidth: Theme.space(40)
                    Layout.horizontalStretchFactor: root.wide ? 3 : 1
                    Layout.preferredHeight: Theme.space(32)
                    Accessible.role: Accessible.Graphic
                    Accessible.name: qsTr("Bands on a frequency scale, with each band's threshold and gain reduction")

                    readonly property real meterW: Theme.space(2)
                    readonly property real plotLeft: freqArea.meterW + Theme.space(2)
                    readonly property real plotRight: freqArea.width - freqArea.meterW - Theme.space(2)
                    readonly property real plotTop: Theme.space(4)
                    readonly property real plotBottom: freqArea.height - Theme.space(3.5)
                    function xOfHz(hz: real): real {
                        return freqArea.plotLeft + (freqArea.plotRight - freqArea.plotLeft) * root.fractionOfHz(hz)
                    }
                    function hzOfX(x: real): real {
                        return root.hzOfFraction((x - freqArea.plotLeft) / Math.max(1, freqArea.plotRight - freqArea.plotLeft))
                    }
                    function yOfDb(db: real): real {
                        return freqArea.plotTop + (freqArea.plotBottom - freqArea.plotTop) * (1 - (db - root.minDb) / (root.maxDb - root.minDb))
                    }
                    function dbOfY(y: real): real {
                        return root.minDb + (root.maxDb - root.minDb) * (1 - (y - freqArea.plotTop) / Math.max(1, freqArea.plotBottom - freqArea.plotTop))
                    }

                    Canvas {
                        id: freqCanvas
                        objectName: "dynamicsFrequencyCanvas"
                        anchors.fill: parent
                        renderTarget: Canvas.Image
                        antialiasing: true
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const w = width
                            const h = height
                            const left = freqArea.plotLeft
                            const right = freqArea.plotRight
                            const top = freqArea.plotTop
                            const bottom = freqArea.plotBottom
                            ctx.fillStyle = Theme.sunken
                            ctx.fillRect(0, 0, w, h)
                            // The bands.
                            for (let b = 0; b < root.bandCount; ++b) {
                                const x0 = freqArea.xOfHz(root.lowEdge(b))
                                const x1 = freqArea.xOfHz(root.highEdge(b))
                                const colour = root.bandColors[b]
                                ctx.fillStyle = Qt.alpha(colour, b === root.selected ? 0.20 : 0.10)
                                ctx.fillRect(x0, top, x1 - x0, bottom - top)
                                const solo = root.bandValue(b, "solo", 0) >= 0.5
                                const bypass = root.bandValue(b, "bypass", 0) >= 0.5
                                // The gain reduction hangs from the top (30 dB over the whole height).
                                const gr = Math.min(30, root.reduction[b] ?? 0)
                                if (gr > 0.01) {
                                    ctx.fillStyle = Qt.alpha(colour, 0.65)
                                    ctx.fillRect(x0 + 1, top, x1 - x0 - 2, (bottom - top) * gr / 30)
                                }
                                // The threshold line.
                                const y = freqArea.yOfDb(root.bandValue(b, "threshold", -24))
                                ctx.strokeStyle = colour
                                ctx.lineWidth = b === root.selected ? 2 : 1
                                ctx.setLineDash(bypass ? [4, 4] : [])
                                ctx.beginPath()
                                ctx.moveTo(x0 + 2, y)
                                ctx.lineTo(x1 - 2, y)
                                ctx.stroke()
                                ctx.setLineDash([])
                                if (solo) {
                                    ctx.fillStyle = colour
                                    ctx.fillRect(x0 + 1, bottom - 3, x1 - x0 - 2, 3)
                                }
                            }
                            // Level grid and frequency marks.
                            ctx.lineWidth = 1
                            ctx.font = Theme.textXs + "px monospace"
                            ctx.fillStyle = Theme.textTertiary
                            ctx.textAlign = "left"
                            for (let db = -60; db <= 0; db += 20) {
                                const y = freqArea.yOfDb(db)
                                ctx.strokeStyle = Theme.border
                                ctx.beginPath()
                                ctx.moveTo(left, y)
                                ctx.lineTo(right, y)
                                ctx.stroke()
                                ctx.fillText(String(db), left + 2, y - 2)
                            }
                            ctx.textAlign = "center"
                            for (const f of [100, 1000, 10000]) {
                                const x = freqArea.xOfHz(f)
                                ctx.strokeStyle = Theme.border
                                ctx.beginPath()
                                ctx.moveTo(x, top)
                                ctx.lineTo(x, bottom)
                                ctx.stroke()
                                ctx.fillText(f >= 1000 ? (f / 1000) + "k" : String(f), x, h - 4)
                            }
                            // The dividers.
                            ctx.strokeStyle = Theme.textSecondary
                            ctx.lineWidth = 1
                            for (let i = 0; i + 1 < root.bandCount; ++i) {
                                const x = freqArea.xOfHz(root.crossover(i))
                                ctx.beginPath()
                                ctx.moveTo(x, top)
                                ctx.lineTo(x, bottom)
                                ctx.stroke()
                            }
                            // The input and output meters (-60 .. 0 dB).
                            const meter = (x, db) => {
                                const m = Math.max(0, Math.min(1, (db + 60) / 60))
                                ctx.fillStyle = Theme.raised
                                ctx.fillRect(x, top, freqArea.meterW, bottom - top)
                                ctx.fillStyle = db > -1 ? Theme.danger : db > -9 ? Theme.warning : Theme.success
                                ctx.fillRect(x, bottom - (bottom - top) * m, freqArea.meterW, (bottom - top) * m)
                            }
                            meter(0, root.inputDb)
                            meter(w - freqArea.meterW, root.outputDb)
                        }
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: Theme.space(1)
                        text: qsTr("In")
                        color: Theme.textTertiary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.space(1)
                        text: qsTr("Out")
                        color: Theme.textTertiary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                    // Click a band to select it.
                    MouseArea {
                        anchors.fill: parent
                        z: -1
                        onPressed: mouse => {
                            const hz = freqArea.hzOfX(mouse.x)
                            for (let b = 0; b < root.bandCount; ++b) {
                                if (hz >= root.lowEdge(b) && hz <= root.highEdge(b))
                                    root.selected = b
                            }
                        }
                    }
                    // The dividers' handles.
                    Repeater {
                        model: Math.max(0, root.bandCount - 1)
                        delegate: Grabber {
                            id: divider
                            required property int index
                            objectName: "dynamicsDivider_" + (divider.index + 1)
                            horizontal: true
                            vertical: false
                            x: Math.round(freqArea.xOfHz(root.crossover(divider.index)) - width / 2)
                            y: Theme.space(0.5)
                            accName: qsTr("Crossover %1, %2").arg(divider.index + 1).arg(root.hzText(root.crossover(divider.index)))
                            tint: Theme.textSecondary
                            onDragged: (px, py) => {
                                const lo = divider.index > 0 ? root.crossover(divider.index - 1) * 1.05 : 20
                                const hi = divider.index + 2 < root.bandCount ? root.crossover(divider.index + 1) / 1.05 : 20000
                                root.edit("crossover" + (divider.index + 1), root.clamp(Math.round(freqArea.hzOfX(px)), lo, hi))
                            }
                            onNudged: (dx, dy) => {
                                const hz = root.crossover(divider.index) * Math.pow(2, dx / 12)
                                root.edit("crossover" + (divider.index + 1), root.clamp(Math.round(hz), 20, 20000))
                            }
                            onFinished: root.finish()
                        }
                    }
                    // The thresholds' handles.
                    Repeater {
                        model: root.bandCount
                        delegate: Grabber {
                            id: threshold
                            required property int index
                            objectName: "dynamicsThreshold_" + (threshold.index + 1)
                            horizontal: false
                            vertical: true
                            x: Math.round((freqArea.xOfHz(root.lowEdge(threshold.index)) + freqArea.xOfHz(root.highEdge(threshold.index))) / 2
                                          - width / 2)
                            y: Math.round(freqArea.yOfDb(root.bandValue(threshold.index, "threshold", -24)) - height / 2)
                            tint: root.bandColors[threshold.index]
                            accName: qsTr("Band %1 threshold, %2 dB").arg(threshold.index + 1)
                                         .arg(root.bandValue(threshold.index, "threshold", -24).toFixed(1))
                            onPressedChanged: if (pressed) root.selected = threshold.index
                            onDragged: (px, py) => root.editBand(threshold.index, "threshold",
                                                                 root.clamp(Math.round(freqArea.dbOfY(py) * 2) / 2, -80, 0))
                            onNudged: (dx, dy) => root.editBand(threshold.index, "threshold",
                                                                root.clamp(root.bandValue(threshold.index, "threshold", -24) + dy, -80, 0))
                            onFinished: root.finish()
                        }
                    }
                }

                // The transfer curve of the selected band.
                Item {
                    id: curveArea
                    objectName: "dynamicsTransferView"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    Layout.minimumWidth: Theme.space(40)
                    Layout.horizontalStretchFactor: root.wide ? 2 : 1
                    Layout.preferredHeight: Theme.space(32)
                    Accessible.role: Accessible.Graphic
                    Accessible.name: qsTr("Transfer curve of band %1: output level against input level").arg(root.selected + 1)

                    readonly property real plotLeft: Theme.space(5)
                    readonly property real plotRight: curveArea.width - Theme.space(2)
                    readonly property real plotTop: Theme.space(2)
                    readonly property real plotBottom: curveArea.height - Theme.space(3.5)
                    readonly property real outMin: -80
                    readonly property real outMax: 12
                    function xOfDb(db: real): real {
                        return curveArea.plotLeft + (curveArea.plotRight - curveArea.plotLeft) * (db - root.minDb) / (root.maxDb - root.minDb)
                    }
                    function dbOfX(x: real): real {
                        return root.minDb + (root.maxDb - root.minDb) * (x - curveArea.plotLeft) / Math.max(1, curveArea.plotRight - curveArea.plotLeft)
                    }
                    function yOfDb(db: real): real {
                        return curveArea.plotTop + (curveArea.plotBottom - curveArea.plotTop) * (1 - (db - curveArea.outMin) / (curveArea.outMax - curveArea.outMin))
                    }
                    function dbOfY(y: real): real {
                        return curveArea.outMin + (curveArea.outMax - curveArea.outMin) * (1 - (y - curveArea.plotTop) / Math.max(1, curveArea.plotBottom - curveArea.plotTop))
                    }
                    /// The curve's output at input `inDb` (the provider's one-per-dB samples, interpolated).
                    function outAt(inDb: real): real {
                        const c = root.curve
                        if (!c || c.length < 2)
                            return inDb
                        const f = root.clamp((inDb - root.minDb) / (root.maxDb - root.minDb), 0, 1) * (c.length - 1)
                        const i = Math.min(c.length - 2, Math.floor(f))
                        return c[i] + (c[i + 1] - c[i]) * (f - i)
                    }
                    readonly property int mode: Math.round(root.bandValue(root.selected, "mode", 0))
                    /// The ratio handle: at 0 dB in for a compressor or limiter, 20 dB under the threshold for an expander. A gate has
                    /// its range there instead. A limiter has none.
                    readonly property real handleIn: curveArea.mode === 1 || curveArea.mode === 2
                                                     ? Math.max(root.minDb, root.bandValue(root.selected, "threshold", -24) - 20) : 0

                    Canvas {
                        id: curveCanvas
                        objectName: "dynamicsTransferCanvas"
                        anchors.fill: parent
                        renderTarget: Canvas.Image
                        antialiasing: true
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const w = width
                            const h = height
                            ctx.fillStyle = Theme.sunken
                            ctx.fillRect(0, 0, w, h)
                            ctx.lineWidth = 1
                            ctx.font = Theme.textXs + "px monospace"
                            ctx.fillStyle = Theme.textTertiary
                            ctx.textAlign = "right"
                            for (let db = -80; db <= 0; db += 20) {
                                ctx.strokeStyle = Theme.border
                                const y = curveArea.yOfDb(db)
                                ctx.beginPath()
                                ctx.moveTo(curveArea.plotLeft, y)
                                ctx.lineTo(curveArea.plotRight, y)
                                ctx.stroke()
                                ctx.fillText(String(db), curveArea.plotLeft - 3, y + 3)
                            }
                            ctx.textAlign = "center"
                            for (let db = -80; db <= 0; db += 20) {
                                const x = curveArea.xOfDb(db)
                                ctx.strokeStyle = Theme.border
                                ctx.beginPath()
                                ctx.moveTo(x, curveArea.plotTop)
                                ctx.lineTo(x, curveArea.plotBottom)
                                ctx.stroke()
                                ctx.fillText(String(db), x, h - 4)
                            }
                            // The 1:1 line, then the band's curve.
                            ctx.strokeStyle = Theme.borderStrong
                            ctx.setLineDash([4, 4])
                            ctx.beginPath()
                            ctx.moveTo(curveArea.xOfDb(root.minDb), curveArea.yOfDb(root.minDb))
                            ctx.lineTo(curveArea.xOfDb(root.maxDb), curveArea.yOfDb(root.maxDb))
                            ctx.stroke()
                            ctx.setLineDash([])
                            const c = root.curve
                            if (c && c.length > 1) {
                                ctx.strokeStyle = root.bandColors[root.selected]
                                ctx.lineWidth = 2
                                ctx.beginPath()
                                for (let i = 0; i < c.length; ++i) {
                                    const x = curveArea.xOfDb(root.minDb + i)
                                    const y = curveArea.yOfDb(Math.max(curveArea.outMin, Math.min(curveArea.outMax, c[i])))
                                    if (i === 0)
                                        ctx.moveTo(x, y)
                                    else
                                        ctx.lineTo(x, y)
                                }
                                ctx.stroke()
                            }
                            // Where the signal is now: the band's reduction at the loudest input.
                            const gr = root.reduction[root.selected] ?? 0
                            if (gr > 0.05) {
                                const x = curveArea.xOfDb(root.clamp(root.inputDb, root.minDb, root.maxDb))
                                ctx.strokeStyle = Theme.textSecondary
                                ctx.lineWidth = 1
                                ctx.beginPath()
                                ctx.moveTo(x, curveArea.plotTop)
                                ctx.lineTo(x, curveArea.plotBottom)
                                ctx.stroke()
                            }
                        }
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.margins: Theme.space(1)
                        text: qsTr("In (dB) to Out")
                        color: Theme.textTertiary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                    // The threshold handle: on the curve at the threshold.
                    Grabber {
                        objectName: "dynamicsCurveThreshold"
                        horizontal: true
                        vertical: false
                        tint: root.bandColors[root.selected]
                        readonly property real thr: root.bandValue(root.selected, "threshold", -24)
                        x: Math.round(curveArea.xOfDb(thr) - width / 2)
                        y: Math.round(curveArea.yOfDb(curveArea.outAt(thr)) - height / 2)
                        accName: qsTr("Band %1 threshold on the curve, %2 dB").arg(root.selected + 1).arg(thr.toFixed(1))
                        onDragged: (px, py) => root.editBand(root.selected, "threshold",
                                                             root.clamp(Math.round(curveArea.dbOfX(px) * 2) / 2, -80, 0))
                        onNudged: (dx, dy) => root.editBand(root.selected, "threshold", root.clamp(thr + dx, -80, 0))
                        onFinished: root.finish()
                    }
                    // The ratio handle (compressor, expander) or the range handle (gate).
                    Grabber {
                        id: shapeHandle
                        objectName: "dynamicsCurveRatio"
                        visible: curveArea.mode !== 3
                        horizontal: false
                        vertical: true
                        tint: root.bandColors[root.selected]
                        readonly property real thr: root.bandValue(root.selected, "threshold", -24)
                        readonly property real makeup: root.bandValue(root.selected, "makeup", 0)
                        x: Math.round(curveArea.xOfDb(curveArea.handleIn) - width / 2)
                        y: Math.round(curveArea.yOfDb(curveArea.outAt(curveArea.handleIn)) - height / 2)
                        accName: curveArea.mode === 2 ? qsTr("Band %1 range, %2 dB").arg(root.selected + 1).arg(root.bandValue(root.selected, "range", 40).toFixed(0))
                                                      : qsTr("Band %1 ratio, %2 to 1").arg(root.selected + 1).arg(root.bandValue(root.selected, "ratio", 2).toFixed(2))
                        function ratioFor(outDb: real): real {
                            // Compress: out = thr + (in - thr) / ratio + makeup. Expand: out = thr - (thr - in) * ratio + makeup.
                            const inDb = curveArea.handleIn
                            const o = outDb - shapeHandle.makeup
                            const r = curveArea.mode === 1 ? (shapeHandle.thr - o) / Math.max(0.1, shapeHandle.thr - inDb)
                                                           : (inDb - shapeHandle.thr) / Math.max(0.05, o - shapeHandle.thr)
                            return root.clamp(Math.round(r * 20) / 20, 1, 30)
                        }
                        onDragged: (px, py) => {
                            const out = curveArea.dbOfY(py)
                            if (curveArea.mode === 2)
                                root.editBand(root.selected, "range", root.clamp(Math.round(curveArea.handleIn - out + shapeHandle.makeup), 0, 80))
                            else
                                root.editBand(root.selected, "ratio", shapeHandle.ratioFor(out))
                        }
                        onNudged: (dx, dy) => {
                            if (curveArea.mode === 2)
                                root.editBand(root.selected, "range", root.clamp(root.bandValue(root.selected, "range", 40) - dy, 0, 80))
                            else
                                root.editBand(root.selected, "ratio", root.clamp(root.bandValue(root.selected, "ratio", 2) - dy * 0.5, 1, 30))
                        }
                        onFinished: root.finish()
                    }
                }
            }

            // ---- The selected band: mode and fields ----
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Text {
                    text: qsTr("Band %1").arg(root.selected + 1)
                    color: root.bandColors[root.selected]
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                }
                PillTabs {
                    objectName: "dynamicsMode"
                    compact: true
                    Layout.fillWidth: true
                    Layout.maximumWidth: Theme.space(90)
                    model: root.modeNames
                    currentIndex: Math.round(root.bandValue(root.selected, "mode", 0))
                    Accessible.name: qsTr("Mode of band %1").arg(root.selected + 1)
                    onActivated: index => {
                        root.editBand(root.selected, "mode", index)
                        root.finish()
                    }
                }
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(3)
                ParamField {
                    paramKey: "threshold"
                    label: qsTr("Threshold")
                    suffix: qsTr("dB")
                    from: -80; to: 0; step: 0.5; decimals: 1
                    value: root.bandValue(root.selected, "threshold", -24)
                    onEdited: v => root.editBand(root.selected, "threshold", v)
                    onFinished: root.finish()
                }
                ParamField {
                    paramKey: "ratio"
                    label: qsTr("Ratio")
                    suffix: qsTr(":1")
                    from: 1; to: 30; step: 0.1; decimals: 2
                    value: root.bandValue(root.selected, "ratio", 2)
                    onEdited: v => root.editBand(root.selected, "ratio", v)
                    onFinished: root.finish()
                }
                ParamField {
                    paramKey: "range"
                    label: qsTr("Range")
                    suffix: qsTr("dB")
                    from: 0; to: 80; step: 1; decimals: 1
                    value: root.bandValue(root.selected, "range", 40)
                    onEdited: v => root.editBand(root.selected, "range", v)
                    onFinished: root.finish()
                }
                ParamField {
                    paramKey: "attack"
                    label: qsTr("Attack")
                    suffix: qsTr("ms")
                    from: 0.1; to: 500; step: 1; decimals: 1
                    value: root.bandValue(root.selected, "attack", 10)
                    onEdited: v => root.editBand(root.selected, "attack", v)
                    onFinished: root.finish()
                }
                ParamField {
                    paramKey: "release"
                    label: qsTr("Release")
                    suffix: qsTr("ms")
                    from: 1; to: 5000; step: 10; decimals: 0
                    value: root.bandValue(root.selected, "release", 100)
                    onEdited: v => root.editBand(root.selected, "release", v)
                    onFinished: root.finish()
                }
                ParamField {
                    paramKey: "knee"
                    label: qsTr("Knee")
                    suffix: qsTr("dB")
                    from: 0; to: 24; step: 1; decimals: 1
                    value: root.bandValue(root.selected, "knee", 6)
                    onEdited: v => root.editBand(root.selected, "knee", v)
                    onFinished: root.finish()
                }
                ParamField {
                    paramKey: "makeup"
                    label: qsTr("Make-up")
                    suffix: qsTr("dB")
                    from: -24; to: 24; step: 0.5; decimals: 1
                    value: root.bandValue(root.selected, "makeup", 0)
                    onEdited: v => root.editBand(root.selected, "makeup", v)
                    onFinished: root.finish()
                }
            }

            // ---- The master section ----
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(3)
                ParamField {
                    paramKey: "inputGain"
                    label: qsTr("Input")
                    suffix: qsTr("dB")
                    from: -24; to: 24; step: 0.5; decimals: 1
                    value: root.masterValue("inputGain", 0)
                    onEdited: v => root.edit("inputGain", v)
                    onFinished: root.finish()
                }
                ParamField {
                    paramKey: "outputGain"
                    label: qsTr("Output")
                    suffix: qsTr("dB")
                    from: -24; to: 24; step: 0.5; decimals: 1
                    value: root.masterValue("outputGain", 0)
                    onEdited: v => root.edit("outputGain", v)
                    onFinished: root.finish()
                }
                ParamField {
                    paramKey: "mix"
                    label: qsTr("Mix")
                    suffix: qsTr("%")
                    from: 0; to: 100; step: 1; decimals: 0
                    value: root.masterValue("mix", 100)
                    onEdited: v => root.edit("mix", v)
                    onFinished: root.finish()
                }
                Toggle {
                    objectName: "dynamicsLimiter"
                    text: qsTr("Limiter")
                    checked: root.masterValue("limiter", 0) >= 0.5
                    Accessible.name: qsTr("Output limiter")
                    onToggled: {
                        root.edit("limiterEnable", checked ? 1 : 0)
                        root.finish()
                    }
                }
                Toggle {
                    objectName: "dynamicsAutoGate"
                    text: qsTr("Auto Gate")
                    checked: root.masterValue("gate", 0) >= 0.5
                    Accessible.name: qsTr("Auto Gate")
                    onToggled: {
                        root.edit("autoGate", checked ? 1 : 0)
                        root.finish()
                    }
                }
            }
        }
    }
}
