import QtQuick
import QtQuick.Controls.Basic

// Monospace numeric field (design language v3: a raised pill) with drag-to-scrub and an optional dim unit `suffix`
// ("%", "px", "dB").
// - Drag horizontally to scrub (`dragStep` per pixel; Shift ×10, Ctrl/⌘ ×0.1).
// - Click (no drag) to type; Enter commits, Escape reverts. The text can be a calculation: 360/12, 1920-50,
//   (1080+120)/2 (+ - * / ^ and parentheses; 0.7, card #135).
// - Up/Down step by `stepSize` (Shift ×10) while focused.
// Controlled: the field shows `value` and reports user edits through `valueModified`; the owner writes the
// new value back (e.g. PropSlider). If it does not, the field snaps back to `value`.
TextField {
    id: control

    property real value: 0
    property real from: -1e9
    property real to: 1e9
    property real stepSize: 1
    property real dragStep: stepSize
    property int decimals: 0
    property string suffix: ""

    signal valueModified(real value)
    /// A drag-to-scrub gesture ended.
    signal scrubFinished()

    function clampValue(v) {
        const q = Math.pow(10, decimals)
        return Math.min(to, Math.max(from, Math.round(v * q) / q))
    }
    function setFromUser(v) {
        const next = clampValue(v)
        if (next !== value)
            valueModified(next)
        showValue()
    }
    // Re-binds the displayed text to `value` (typing or an assignment would otherwise break the binding).
    function showValue() {
        text = Qt.binding(() => control.formatted)
    }
    /// 0.7 (card #135): the value of a typed calculation - numbers with + - * / ^ and parentheses ("360/12", "1920-50",
    /// "(1080+120)/2"; × ÷ − work too; a comma is a decimal point). { ok: false } when the text is not one; the value can
    /// still be NaN or infinite ("1/0"), which the caller refuses. A parser, never eval: only the characters above are read.
    function evaluateExpression(source) {
        const src = String(source).replace(/×/g, "*").replace(/÷/g, "/").replace(/−/g, "-").replace(/,/g, ".")
        if (src.length > 200)
            return { ok: false, value: NaN }
        let pos = 0
        let depth = 0
        const skip = () => {
            while (pos < src.length && src[pos] === " ")
                ++pos
        }
        const number = () => {
            skip()
            const m = /^(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/.exec(src.slice(pos))
            if (!m)
                throw new Error("number")
            pos += m[0].length
            return Number(m[0])
        }
        let sum = null
        const primary = () => {
            skip()
            if (src[pos] !== "(")
                return number()
            if (++depth > 32)
                throw new Error("depth")
            ++pos
            const v = sum()
            skip()
            if (src[pos] !== ")")
                throw new Error("close")
            ++pos
            --depth
            return v
        }
        let unary = null
        const power = () => {
            const base = primary()
            skip()
            if (src[pos] !== "^")
                return base
            ++pos
            return Math.pow(base, unary())
        }
        unary = () => {
            skip()
            if (src[pos] === "-") {
                ++pos
                return -unary()
            }
            if (src[pos] === "+") {
                ++pos
                return unary()
            }
            return power()
        }
        const product = () => {
            let v = unary()
            for (;;) {
                skip()
                const op = src[pos]
                if (op !== "*" && op !== "/")
                    return v
                ++pos
                const r = unary()
                v = op === "*" ? v * r : v / r
            }
        }
        sum = () => {
            let v = product()
            for (;;) {
                skip()
                const op = src[pos]
                if (op !== "+" && op !== "-")
                    return v
                ++pos
                const r = product()
                v = op === "+" ? v + r : v - r
            }
        }
        try {
            const v = sum()
            skip()
            return pos === src.length ? { ok: true, value: v } : { ok: false, value: NaN }
        } catch (e) {
            return { ok: false, value: NaN }
        }
    }
    /// What typed text means: a calculation's result, else the number it starts with ("50%" -> 50, as before), else NaN.
    function parseInput(typed) {
        const r = evaluateExpression(typed)
        if (r.ok)
            return Number.isFinite(r.value) ? r.value : NaN
        return Number.parseFloat(String(typed).replace(",", "."))
    }
    function modifierScale(modifiers) {
        if (modifiers & Qt.ShiftModifier)
            return 10
        if (modifiers & Qt.ControlModifier)
            return 0.1
        return 1
    }

    readonly property string formatted: Number(value).toFixed(decimals)

    text: formatted
    implicitWidth: Theme.numberFieldWidth
    implicitHeight: Theme.controlHeightSmall
    padding: 0
    leftPadding: Theme.space(2.5)
    rightPadding: Theme.space(2.5) + (suffix !== "" ? suffixText.implicitWidth + Theme.space(1) : 0)
    horizontalAlignment: TextInput.AlignRight
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true
    inputMethodHints: Qt.ImhFormattedNumbersOnly
    font.family: Theme.fontMono
    font.pixelSize: Theme.textMd
    color: enabled ? Theme.textPrimary : Theme.textDisabled
    selectionColor: Theme.accentFill
    selectedTextColor: Theme.accentOnFill
    placeholderTextColor: Theme.textTertiary

    Accessible.role: Accessible.SpinBox
    Accessible.description: qsTr("Type a number or a calculation such as 360/12")

    onEditingFinished: {
        const parsed = control.parseInput(text)
        if (Number.isNaN(parsed))
            showValue()
        else
            setFromUser(parsed)
    }
    onActiveFocusChanged: {
        if (!activeFocus)
            showValue()
    }

    Keys.onUpPressed: event => control.setFromUser(control.value + control.stepSize * ((event.modifiers & Qt.ShiftModifier) ? 10 : 1))
    Keys.onDownPressed: event => control.setFromUser(control.value - control.stepSize * ((event.modifiers & Qt.ShiftModifier) ? 10 : 1))
    Keys.onEscapePressed: {
        control.showValue()
        control.focus = false
    }

    background: Rectangle {
        radius: height / 2
        color: control.activeFocus ? Theme.raised : scrubArea.containsMouse || scrubArea.pressed ? Theme.hover : Theme.raised
        // The focus colour's edge is the focus indicator (focus always means editing for a text field).
        border.width: control.activeFocus ? Theme.focusRingWidth : 0
        border.color: Theme.focus

        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }

    Text {
        id: suffixText
        anchors.right: parent.right
        anchors.rightMargin: Theme.space(2.5)
        anchors.verticalCenter: parent.verticalCenter
        visible: control.suffix !== ""

        text: control.suffix
        color: Theme.textTertiary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
    }

    // Scrub surface: active only while the field is not being edited, so text selection still works.
    MouseArea {
        id: scrubArea

        property real pressX: 0
        property real pressValue: 0
        property bool scrubbing: false

        anchors.fill: parent
        enabled: !control.activeFocus && control.enabled
        hoverEnabled: true
        cursorShape: Qt.SizeHorCursor
        preventStealing: true

        onPressed: mouse => {
            pressX = mouse.x
            pressValue = control.value
            scrubbing = false
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const dx = mouse.x - pressX
            if (!scrubbing && Math.abs(dx) < Application.styleHints.startDragDistance)
                return
            scrubbing = true
            control.setFromUser(pressValue + dx * control.dragStep * control.modifierScale(mouse.modifiers))
        }
        onReleased: {
            if (!scrubbing) {
                control.forceActiveFocus(Qt.MouseFocusReason)
                control.selectAll()
            } else {
                control.scrubFinished()
            }
            scrubbing = false
        }
    }
}
