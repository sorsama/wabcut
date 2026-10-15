import QtQuick
import QtQuick.Controls.Basic

// Monospace numeric field (design language v3: a raised pill) with drag-to-scrub and an optional dim unit `suffix`
// ("%", "px", "dB").
// - Drag horizontally to scrub (`dragStep` per pixel; Shift ×10, Ctrl/⌘ ×0.1).
// - Click (no drag) to type; Enter commits, Escape reverts.
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

    onEditingFinished: {
        const parsed = Number.parseFloat(text.replace(",", "."))
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
