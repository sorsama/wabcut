pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The Audio Tools panel's SPECTRAL tab (0.7, card 047; Audition's spectral frequency display, Fairlight's and Sound Forge's spectral
// editing): the spectrogram of the selected audio clip (an STFT, logarithmic or linear frequency, a colour scale in dB, zoom
// and scroll, drawn off the GUI thread from a cached analysis) and the tools that repair what is in it.
//   Select      Box (a rectangle of time and frequency), Lasso, Brush, and Tone (click a steady tone: it snaps to the
//               strongest peak near the click; drag along a moving one to trace it).
//   Repair      Attenuate by N dB, Heal (replace the selection with what surrounds it), Fill with noise floor, Remove tone
//               (and its harmonics). Each is one undo step, stored with the clip (the clip's Spectral Repair effect: not
//               destructive, not keyframed, the same in preview and export) and shown over the spectrogram.
//   Listen      Play selection plays the part once; Repaired / Selection / Removed chooses what the player plays (the repaired
//               sound, only the sound the edits select, or only what they take away); Repair on switches the repairs off to compare.
//   View        FFT size, frequency scale, Original or Result (the spectrogram of the repaired sound), channel, the dB range.
Item {
    id: root
    objectName: "spectralTab"

    readonly property bool hasClip: SpectralRepair.available
    /// "rectangle" | "lasso" | "brush" | "tone"
    property string tool: "rectangle"
    /// The draft selection: [{ time, hz }] (a box: two opposite corners).
    property var points: []
    property bool dragging: false
    property bool hovering: false
    property real hoverX: 0
    property real hoverY: 0
    property string notice: ""
    property real gainDb: -24
    property real healAmount: 100
    property int harmonics: 1
    property real brushMilliseconds: 50
    property real brushSemitones: 3
    readonly property int needed: tool === "rectangle" ? 2 : tool === "lasso" ? 3 : 1
    readonly property bool hasSelection: points.length >= needed && !dragging
    readonly property real selStart: points.length > 0 ? points.reduce((m, p) => Math.min(m, p.time), 1e9) : 0
    readonly property real selEnd: points.length > 0 ? points.reduce((m, p) => Math.max(m, p.time), -1e9) : 0
    readonly property real selLow: points.length > 0 ? points.reduce((m, p) => Math.min(m, p.hz), 1e9) : 0
    readonly property real selHigh: points.length > 0 ? points.reduce((m, p) => Math.max(m, p.hz), -1e9) : 0

    function focusContent() {
        toolTabs.forceActiveFocus(Qt.TabFocusReason)
    }
    function fmtTime(s, step) {
        const m = Math.floor(s / 60)
        const sec = s - m * 60
        const decimals = step < 0.1 ? 2 : step < 1 ? 1 : 0
        const text = sec.toFixed(decimals)
        return m > 0 ? m + ":" + (sec < 10 ? "0" : "") + text : text
    }
    function fmtHz(hz) {
        return hz >= 1000 ? qsTr("%1 kHz").arg((hz / 1000).toFixed(hz >= 10000 ? 0 : 1)) : qsTr("%1 Hz").arg(Math.round(hz))
    }
    // A frequency axis label: "50", "500", "2k", "20k".
    function axisHz(hz) {
        return hz >= 1000 ? qsTr("%1k").arg(Math.round(hz / 100) / 10) : String(Math.round(hz))
    }
    function kindName(kind) {
        return kind === "attenuate" ? qsTr("Attenuate") : kind === "heal" ? qsTr("Heal") : kind === "noiseFloor" ? qsTr("Noise floor") : qsTr("Remove tone")
    }
    function editText(e) {
        const when = qsTr("%1 to %2 s").arg(fmtTime(e.start, 0.01)).arg(fmtTime(e.end, 0.01))
        if (e.kind === "tone")
            return qsTr("%1 · %2 · %3").arg(kindName(e.kind)).arg(fmtHz(e.hz)).arg(when)
        const level = e.kind === "attenuate" ? qsTr(" %1 dB").arg(Number(e.gainDb).toFixed(0)) : ""
        return qsTr("%1%2 · %3 · %4 to %5").arg(kindName(e.kind)).arg(level).arg(when).arg(fmtHz(e.low)).arg(fmtHz(e.high))
    }
    function clearDraft() {
        points = []
        dragging = false
    }
    function show(error) {
        notice = error
    }
    function point(x, y) {
        return { time: Math.max(SpectralRepair.rangeStart, Math.min(SpectralRepair.rangeEnd, view.timeAt(x))), hz: Math.max(20, view.hzAt(y)) }
    }
    function brushOptions() {
        return { radiusSeconds: root.brushMilliseconds / 1000, radiusOctaves: root.brushSemitones / 12 }
    }
    // Runs a repair on the draft selection.
    function apply(kind) {
        if (!hasSelection)
            return
        let options = {}
        if (kind === "attenuate")
            options.gainDb = root.gainDb
        if (kind === "heal" || kind === "noiseFloor")
            options.amount = root.healAmount
        if (tool === "brush")
            Object.assign(options, brushOptions())
        let shape = tool === "tone" ? "rectangle" : tool
        let pts = points
        if (kind === "tone") {
            // A tone: the strongest peak inside the selection (or the clicked frequency), its harmonics, over the selection's time
            // (the whole clip for a click).
            options.harmonics = root.harmonics
            if (tool === "tone" && points.length === 1) {
                options.hz = points[0].hz
            } else if (tool === "tone") {
                pts = points
            } else {
                const hz = SpectralRepair.findTone(selStart, selEnd, selLow, selHigh)
                if (hz <= 0) {
                    show(qsTr("No steady tone stands out in the selection yet: wait for the analysis, or select around the tone"))
                    return
                }
                options.hz = hz
                options.start = selStart
                options.end = selEnd
                pts = []
            }
        }
        const error = SpectralRepair.addEdit(kind, shape, pts, options)
        show(error)
        if (error === "")
            clearDraft()
    }
    // Where the nearest strong peak is, for a click with the Tone tool.
    function snapTone(p) {
        const hz = SpectralRepair.findTone(Math.max(SpectralRepair.rangeStart, p.time - 1), Math.min(SpectralRepair.rangeEnd, p.time + 1),
                                           p.hz / 1.12, p.hz * 1.12)
        return hz > 0 ? hz : p.hz
    }

    component Caption: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }
    component FieldLabel: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
    }
    component Divider: Rectangle {
        Layout.fillWidth: true
        implicitHeight: Theme.hairline
        color: Theme.border
    }

    Flickable {
        id: scroll
        objectName: "spectralScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: column
            width: scroll.width - Theme.space(3)
            spacing: Theme.space(2)

            // ---- The clip ----
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Text {
                    objectName: "spectralClipName"
                    Layout.fillWidth: true
                    text: root.hasClip ? qsTr("%1 · %2 Hz · %n channel(s)", "", SpectralRepair.channels).arg(SpectralRepair.clipName).arg(SpectralRepair.sampleRate)
                                       : SpectralRepair.reason
                    wrapMode: Text.Wrap
                    color: root.hasClip ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: root.hasClip ? Font.DemiBold : Font.Normal
                }
                StatusPill {
                    objectName: "spectralBusy"
                    visible: root.hasClip && view.analysing
                    dotColor: Theme.warning
                    label: qsTr("Analysing %1%").arg(Math.round(view.progress * 100))
                }
                IconButton {
                    objectName: "spectralZoomOut"
                    visible: root.hasClip
                    compact: true
                    iconName: "zoom-out"
                    tooltip: qsTr("Zoom out")
                    onClicked: SpectralRepair.zoomView(0.5, SpectralRepair.viewStart + SpectralRepair.viewSpan / 2)
                }
                IconButton {
                    objectName: "spectralZoomIn"
                    visible: root.hasClip
                    compact: true
                    iconName: "zoom-in"
                    tooltip: qsTr("Zoom in")
                    onClicked: SpectralRepair.zoomView(2, SpectralRepair.viewStart + SpectralRepair.viewSpan / 2)
                }
                PillButton {
                    objectName: "spectralFit"
                    visible: root.hasClip
                    compact: true
                    text: qsTr("Fit")
                    tooltip: qsTr("Show the whole clip")
                    onClicked: SpectralRepair.fitView()
                }
            }

            // ---- The spectrogram ----
            Rectangle {
                id: stage
                objectName: "spectralStage"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(Theme.space(60), Math.min(scroll.height * 0.55, Theme.space(96)))
                visible: root.hasClip
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border

                readonly property real axisWidth: Theme.space(9)
                readonly property real rulerHeight: Theme.space(6)
                readonly property var freqTicks: {
                    const top = SpectralRepair.sampleRate / 2
                    const all = SpectralRepair.logFrequency ? [50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
                                                           : [0, 2000, 4000, 6000, 8000, 10000, 12000, 16000, 20000, 24000, 32000, 40000]
                    return all.filter(f => f <= top * 0.98)
                }
                readonly property real tickStep: {
                    const raw = SpectralRepair.viewSpan / Math.max(1, view.width / Theme.space(22))
                    for (const s of [0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600])
                        if (s >= raw)
                            return s
                    return 3600
                }
                readonly property var timeTicks: {
                    const out = []
                    const first = Math.ceil(SpectralRepair.viewStart / tickStep)
                    const last = Math.floor((SpectralRepair.viewStart + SpectralRepair.viewSpan) / tickStep)
                    for (let i = first; i <= last && out.length < 40; ++i)
                        out.push(i * tickStep)
                    return out
                }

                // Frequency axis (left).
                Item {
                    id: freqAxis
                    objectName: "spectralFrequencyAxis"
                    x: Theme.hairline
                    y: Theme.hairline
                    width: stage.axisWidth
                    height: view.height
                    clip: true
                    Repeater {
                        model: stage.freqTicks
                        delegate: Text {
                            required property real modelData
                            y: {
                                view.height
                                SpectralRepair.logFrequency
                                SpectralRepair.sampleRate
                                return Math.max(0, Math.min(freqAxis.height - height, view.yAt(modelData) - height / 2))
                            }
                            width: freqAxis.width - Theme.space(1.5)
                            horizontalAlignment: Text.AlignRight
                            text: root.axisHz(modelData)
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }
                }

                SpectrogramView {
                    id: view
                    objectName: "spectrogramView"
                    x: stage.axisWidth + Theme.hairline
                    y: Theme.hairline
                    width: stage.width - stage.axisWidth - 2 * Theme.hairline
                    height: stage.height - stage.rulerHeight - 2 * Theme.hairline
                    dark: Theme.dark
                    selectionColor: Theme.accentStroke
                    editColor: Theme.accentStroke
                    playheadColor: Theme.playhead
                    backgroundColor: Theme.sunken
                    selectionShape: root.points.length > 0 ? root.tool : ""
                    selectionPoints: root.points
                    brushRadiusSeconds: root.brushMilliseconds / 1000
                    brushRadiusOctaves: root.brushSemitones / 12
                    Accessible.role: Accessible.Graphic
                    Accessible.name: qsTr("Spectrogram of %1").arg(SpectralRepair.clipName)
                    Accessible.description: qsTr("Time runs left to right, frequency bottom to top, louder is brighter. %n repair(s) on this clip.", "", SpectralRepair.edits.length)

                    // Horizontal grid at the axis ticks and vertical at the time ticks.
                    Repeater {
                        model: stage.freqTicks
                        delegate: Rectangle {
                            required property real modelData
                            width: view.width
                            height: Theme.hairline
                            y: {
                                view.height
                                SpectralRepair.logFrequency
                                SpectralRepair.sampleRate
                                return view.yAt(modelData)
                            }
                            visible: modelData > 0
                            color: Theme.border
                        }
                    }
                    Repeater {
                        model: stage.timeTicks
                        delegate: Rectangle {
                            required property real modelData
                            width: Theme.hairline
                            height: view.height
                            x: {
                                view.width
                                SpectralRepair.viewStart
                                SpectralRepair.viewSpan
                                return view.xAt(modelData)
                            }
                            color: Theme.border
                        }
                    }

                    Text {
                        objectName: "spectralPending"
                        anchors.centerIn: parent
                        visible: !view.hasImage && root.hasClip
                        text: qsTr("Analysing the sound…")
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }

                    MouseArea {
                        id: pointer
                        objectName: "spectralPointer"
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.CrossCursor
                        acceptedButtons: Qt.LeftButton
                        property point pressAt
                        property bool moved: false

                        onPressed: mouse => {
                            root.notice = ""
                            pressAt = Qt.point(mouse.x, mouse.y)
                            moved = false
                            root.dragging = true
                            const p = root.point(mouse.x, mouse.y)
                            root.points = root.tool === "rectangle" ? [p, p] : [p]
                            root.forceActiveFocus()
                        }
                        onPositionChanged: mouse => {
                            root.hoverX = mouse.x
                            root.hoverY = mouse.y
                            if (!pressed)
                                return
                            const p = root.point(Math.max(0, Math.min(width, mouse.x)), Math.max(0, Math.min(height, mouse.y)))
                            if (Math.abs(mouse.x - pressAt.x) > 4 || Math.abs(mouse.y - pressAt.y) > 4)
                                moved = true
                            if (root.tool === "rectangle") {
                                root.points = [root.points[0], p]
                            } else if (root.tool === "lasso" || root.tool === "brush" || (root.tool === "tone" && moved)) {
                                const last = root.points[root.points.length - 1]
                                const dx = view.xAt(last.time) - mouse.x
                                const dy = view.yAt(last.hz) - mouse.y
                                if (dx * dx + dy * dy < 36 || root.points.length >= 47)
                                    return
                                root.points = root.points.concat([p])
                            }
                        }
                        onReleased: mouse => {
                            root.dragging = false
                            if (root.tool === "rectangle") {
                                if (!moved) {
                                    const at = root.points[0].time
                                    root.clearDraft()
                                    SpectralRepair.seekTo(at)
                                }
                            } else if (root.tool === "tone" && !moved) {
                                const p = root.points[0]
                                root.points = [{ time: p.time, hz: root.snapTone(p) }]
                            } else if (!moved) {
                                root.clearDraft()
                            }
                        }
                        onWheel: wheel => {
                            if (wheel.modifiers & Qt.ShiftModifier) {
                                SpectralRepair.scrollView(-wheel.angleDelta.y / 120 * SpectralRepair.viewSpan * 0.1)
                            } else {
                                SpectralRepair.zoomView(wheel.angleDelta.y > 0 ? 1.25 : 0.8, view.timeAt(wheel.x))
                            }
                        }
                        onExited: root.hovering = false
                        onEntered: root.hovering = true
                    }
                }

                // Time ruler under the view: click to move the playhead.
                Item {
                    id: ruler
                    objectName: "spectralRuler"
                    x: view.x
                    y: view.y + view.height
                    width: view.width
                    height: stage.rulerHeight
                    clip: true
                    Repeater {
                        model: stage.timeTicks
                        delegate: Text {
                            required property real modelData
                            x: {
                                view.width
                                SpectralRepair.viewStart
                                SpectralRepair.viewSpan
                                return Math.max(0, Math.min(ruler.width - width, view.xAt(modelData) - width / 2))
                            }
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.fmtTime(modelData, stage.tickStep)
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: mouse => SpectralRepair.seekTo(view.timeAt(mouse.x))
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Move the playhead")
                    }
                }
            }

            // Scroll the view, and what is under the pointer.
            ThinScrollBar {
                id: scrollBar
                objectName: "spectralScrollBar"
                Layout.fillWidth: true
                visible: root.hasClip && SpectralRepair.viewSpan < SpectralRepair.rangeEnd - SpectralRepair.rangeStart - 0.001
                orientation: Qt.Horizontal
                policy: ScrollBar.AlwaysOn
                implicitHeight: Theme.space(3)
                readonly property real total: Math.max(0.001, SpectralRepair.rangeEnd - SpectralRepair.rangeStart)
                size: Math.min(1, SpectralRepair.viewSpan / total)
                position: (SpectralRepair.viewStart - SpectralRepair.rangeStart) / total
                onPositionChanged: {
                    if (pressed)
                        SpectralRepair.viewStart = SpectralRepair.rangeStart + position * total
                }
            }
            Text {
                objectName: "spectralReadout"
                Layout.fillWidth: true
                visible: root.hasClip
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
                text: {
                    if (!root.hovering)
                        return qsTr("Wheel zooms, Shift+wheel scrolls")
                    const level = view.levelAt(root.hoverX, root.hoverY)
                    const at = qsTr("%1 s · %2").arg(root.fmtTime(view.timeAt(root.hoverX), 0.01)).arg(root.fmtHz(view.hzAt(root.hoverY)))
                    return level > -999 ? qsTr("%1 · %2 dB").arg(at).arg(level.toFixed(0)) : at
                }
            }

            // ---- Tools ----
            Flow {
                Layout.fillWidth: true
                visible: root.hasClip
                spacing: Theme.space(2)
                PillTabs {
                    id: toolTabs
                    objectName: "spectralTools"
                    compact: true
                    model: [{ label: qsTr("Box"), icon: "square", tooltip: qsTr("Select a rectangle of time and frequency") },
                            { label: qsTr("Lasso"), icon: "spline", tooltip: qsTr("Draw around the sound to select") },
                            { label: qsTr("Brush"), icon: "pencil", tooltip: qsTr("Paint over the sound to select") },
                            { label: qsTr("Tone"), icon: "crosshair", tooltip: qsTr("Click a steady tone, or drag along a moving one") }]
                    currentIndex: ["rectangle", "lasso", "brush", "tone"].indexOf(root.tool)
                    Accessible.name: qsTr("Selection tool")
                    onActivated: index => {
                        root.tool = ["rectangle", "lasso", "brush", "tone"][index]
                        root.clearDraft()
                    }
                }
                RowLayout {
                    visible: root.tool === "brush"
                    spacing: Theme.space(1)
                    FieldLabel { text: qsTr("Brush") }
                    NumberField {
                        objectName: "spectralBrushTime"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 5
                        to: 2000
                        suffix: qsTr("ms")
                        value: root.brushMilliseconds
                        onValueModified: v => root.brushMilliseconds = v
                        Accessible.name: qsTr("Brush half-width in milliseconds")
                    }
                    NumberField {
                        objectName: "spectralBrushPitch"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 1
                        to: 36
                        suffix: qsTr("st")
                        value: root.brushSemitones
                        onValueModified: v => root.brushSemitones = v
                        Accessible.name: qsTr("Brush half-height in semitones")
                    }
                }
            }

            // ---- The selection ----
            Text {
                objectName: "spectralSelection"
                Layout.fillWidth: true
                visible: root.hasClip
                wrapMode: Text.Wrap
                color: root.hasSelection ? Theme.textPrimary : Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
                text: {
                    if (!root.hasSelection)
                        return root.tool === "tone" ? qsTr("Click a steady tone, or drag along a moving one") : qsTr("Drag on the spectrogram to select")
                    if (root.tool === "tone" && root.points.length === 1)
                        return qsTr("Tone at %1").arg(root.fmtHz(root.points[0].hz))
                    return qsTr("%1 to %2 s (%3) · %4 to %5").arg(root.fmtTime(root.selStart, 0.01)).arg(root.fmtTime(root.selEnd, 0.01))
                           .arg(SpectralRepair.durationText(root.selEnd - root.selStart)).arg(root.fmtHz(root.selLow)).arg(root.fmtHz(root.selHigh))
                }
            }
            Text {
                objectName: "spectralLongWarning"
                Layout.fillWidth: true
                visible: root.hasClip && root.hasSelection && root.tool !== "tone" && root.selEnd - root.selStart > 0.88
                wrapMode: Text.Wrap
                color: Theme.warning
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                text: qsTr("Longer than about a second: Heal and Fill look about a second around the selection, so what is further inside is filled from the sound above and below it")
            }
            Text {
                objectName: "spectralNotice"
                Layout.fillWidth: true
                visible: text !== ""
                text: root.notice !== "" ? root.notice : SpectralRepair.message
                wrapMode: Text.Wrap
                color: Theme.warning
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            // ---- Repair ----
            Flow {
                id: repairRow
                objectName: "spectralRepairRow"
                Layout.fillWidth: true
                visible: root.hasClip
                spacing: Theme.space(2)
                RowLayout {
                    spacing: Theme.space(1)
                    NumberField {
                        objectName: "spectralGain"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: -96
                        to: 0
                        decimals: 1
                        stepSize: 1
                        suffix: qsTr("dB")
                        value: root.gainDb
                        onValueModified: v => root.gainDb = v
                        Accessible.name: qsTr("How far the selection goes down, in dB")
                    }
                    PillButton {
                        objectName: "spectralAttenuate"
                        compact: true
                        variant: "primary"
                        text: qsTr("Attenuate")
                        enabled: root.hasSelection && root.tool !== "tone"
                        tooltip: qsTr("Lower the selection by this many dB")
                        onClicked: root.apply("attenuate")
                    }
                }
                RowLayout {
                    spacing: Theme.space(1)
                    PillButton {
                        objectName: "spectralHeal"
                        compact: true
                        variant: "primary"
                        text: qsTr("Heal")
                        enabled: root.hasSelection && root.tool !== "tone"
                        tooltip: qsTr("Replace the selection with the sound around it")
                        onClicked: root.apply("heal")
                    }
                    PillButton {
                        objectName: "spectralNoiseFloor"
                        compact: true
                        text: qsTr("Fill with noise floor")
                        enabled: root.hasSelection && root.tool !== "tone"
                        tooltip: qsTr("Replace the selection with the quiet background around it")
                        onClicked: root.apply("noiseFloor")
                    }
                    NumberField {
                        objectName: "spectralAmount"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 1
                        to: 100
                        suffix: qsTr("%")
                        value: root.healAmount
                        onValueModified: v => root.healAmount = v
                        Accessible.name: qsTr("How much of the repair replaces the sound, in percent")
                    }
                }
                RowLayout {
                    spacing: Theme.space(1)
                    PillButton {
                        objectName: "spectralRemoveTone"
                        compact: true
                        variant: "primary"
                        text: qsTr("Remove tone")
                        enabled: root.hasSelection
                        tooltip: qsTr("Take out the tone in the selection and its harmonics")
                        onClicked: root.apply("tone")
                    }
                    FieldLabel { text: qsTr("Harmonics") }
                    NumberField {
                        objectName: "spectralHarmonics"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 1
                        to: 16
                        value: root.harmonics
                        onValueModified: v => root.harmonics = Math.round(v)
                        Accessible.name: qsTr("How many harmonics of the tone to remove")
                    }
                }
            }

            // ---- Listen ----
            Flow {
                id: listenRow
                objectName: "spectralListenRow"
                Layout.fillWidth: true
                visible: root.hasClip
                spacing: Theme.space(2)
                PillButton {
                    objectName: "spectralPlaySelection"
                    compact: true
                    iconName: "play"
                    text: qsTr("Play selection")
                    enabled: root.hasSelection
                    tooltip: qsTr("Play the selected seconds once")
                    onClicked: root.show(SpectralRepair.playSelection(root.selStart, root.selEnd))
                }
                PillTabs {
                    objectName: "spectralListen"
                    compact: true
                    enabled: SpectralRepair.hasRepair
                    model: [{ label: qsTr("Repaired"), tooltip: qsTr("Play the repaired sound") },
                            { label: qsTr("Selection"), tooltip: qsTr("Play only the sound the repairs select") },
                            { label: qsTr("Removed"), tooltip: qsTr("Play only what the repairs take away") }]
                    currentIndex: SpectralRepair.listen
                    Accessible.name: qsTr("What the player plays")
                    onActivated: index => root.show(SpectralRepair.setListen(index))
                }
                Toggle {
                    objectName: "spectralRepairOn"
                    enabled: SpectralRepair.hasRepair
                    text: qsTr("Repair on")
                    tooltip: qsTr("Switch the repairs off to compare with the original")
                    checked: SpectralRepair.repairEnabled
                    onToggled: {
                        root.show(SpectralRepair.setRepairEnabled(checked))
                        checked = Qt.binding(() => SpectralRepair.repairEnabled)
                    }
                }
            }
            Caption {
                visible: root.hasClip && SpectralRepair.listen !== 0 && SpectralRepair.hasRepair
                text: qsTr("The player is playing %1: choose Repaired to hear the clip as it will export").arg(SpectralRepair.listen === 1 ? qsTr("only the selected sound") : qsTr("only what the repairs take away"))
                color: Theme.warning
            }

            // ---- View settings ----
            Flow {
                id: viewSettings
                objectName: "spectralViewSettings"
                Layout.fillWidth: true
                visible: root.hasClip
                spacing: Theme.space(2)
                PillTabs {
                    objectName: "spectralFft"
                    compact: true
                    model: [{ label: "1024" }, { label: "2048" }, { label: "4096" }]
                    currentIndex: SpectralRepair.fftSize === 1024 ? 0 : SpectralRepair.fftSize === 2048 ? 1 : 2
                    Accessible.name: qsTr("FFT size")
                    onActivated: index => SpectralRepair.fftSize = [1024, 2048, 4096][index]
                }
                PillTabs {
                    objectName: "spectralAxis"
                    compact: true
                    model: [{ label: qsTr("Log") }, { label: qsTr("Linear") }]
                    currentIndex: SpectralRepair.logFrequency ? 0 : 1
                    Accessible.name: qsTr("Frequency scale")
                    onActivated: index => SpectralRepair.logFrequency = index === 0
                }
                PillTabs {
                    objectName: "spectralShow"
                    compact: true
                    model: [{ label: qsTr("Original"), tooltip: qsTr("The clip's sound as it is") },
                            { label: qsTr("Result"), tooltip: qsTr("The sound as the clip's repairs leave it") }]
                    currentIndex: SpectralRepair.showResult ? 1 : 0
                    Accessible.name: qsTr("Show the original or the repaired sound")
                    onActivated: index => SpectralRepair.showResult = index === 1
                }
                PillTabs {
                    objectName: "spectralChannel"
                    visible: SpectralRepair.channels > 1 && SpectralRepair.channels <= 4
                    compact: true
                    model: {
                        const out = [{ label: qsTr("Mix") }]
                        for (let c = 1; c <= SpectralRepair.channels; ++c)
                            out.push({ label: String(c) })
                        return out
                    }
                    currentIndex: SpectralRepair.channel + 1
                    Accessible.name: qsTr("Channel shown")
                    onActivated: index => SpectralRepair.channel = index - 1
                }
                RowLayout {
                    spacing: Theme.space(1)
                    FieldLabel { text: qsTr("Range") }
                    NumberField {
                        objectName: "spectralFloor"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: -120
                        to: -20
                        suffix: qsTr("dB")
                        value: SpectralRepair.floorDb
                        onValueModified: v => SpectralRepair.floorDb = v
                        Accessible.name: qsTr("Quietest level the colours show, in dB")
                    }
                    NumberField {
                        objectName: "spectralCeiling"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: -60
                        to: 0
                        suffix: qsTr("dB")
                        value: SpectralRepair.ceilingDb
                        onValueModified: v => SpectralRepair.ceilingDb = v
                        Accessible.name: qsTr("Loudest level the colours show, in dB")
                    }
                }
            }

            // ---- The clip's repairs ----
            Divider { visible: root.hasClip && SpectralRepair.edits.length > 0 }
            RowLayout {
                Layout.fillWidth: true
                visible: root.hasClip && SpectralRepair.edits.length > 0
                FieldLabel {
                    Layout.fillWidth: true
                    text: qsTr("%n repair(s) on this clip", "", SpectralRepair.edits.length)
                    color: Theme.textPrimary
                    font.weight: Font.DemiBold
                }
                PillButton {
                    objectName: "spectralClear"
                    compact: true
                    text: qsTr("Clear all")
                    tooltip: qsTr("Remove every repair from the clip")
                    onClicked: root.show(SpectralRepair.clearEdits())
                }
            }
            Repeater {
                model: root.hasClip ? SpectralRepair.edits : []
                delegate: RowLayout {
                    id: editRow
                    required property var modelData
                    required property int index
                    objectName: "spectralEdit_" + index
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    FieldLabel {
                        Layout.fillWidth: true
                        text: root.editText(editRow.modelData)
                        color: Theme.textPrimary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: SpectralRepair.seekTo(editRow.modelData.start)
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Go to this repair")
                        }
                    }
                    IconButton {
                        objectName: "spectralRemoveEdit_" + editRow.index
                        compact: true
                        iconName: "trash-2"
                        tooltip: qsTr("Remove this repair")
                        onClicked: root.show(SpectralRepair.removeEdit(editRow.index))
                    }
                }
            }
            Caption {
                visible: root.hasClip && SpectralRepair.edits.length === 0
                text: qsTr("Select a part of the sound and choose a repair: it is stored with the clip, can be undone, and plays the same in the preview and the export.")
            }
        }
    }
}
