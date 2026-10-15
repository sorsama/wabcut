pragma ComponentBehavior: Bound
import QtQuick

// A colour picker (0.1.3, Settings > Appearance): a saturation/brightness square, a hue strip, an opacity strip (when
// `allowAlpha`) and the hex value. Every change is reported at once (colorPicked: "#RRGGBB", or "#AARRGGBB" when not
// opaque), so the theme previews live; Escape or a click outside closes it. Keyboard: the square takes the arrows
// (saturation, brightness), the strips Left/Right; Shift moves ten times as far. The gradients are the colour model
// itself, not interface chrome, so they are the one place with literal colours.
Popover {
    id: root
    objectName: "colorPickerPopover"

    property bool allowAlpha: true
    // The colour being edited as hue/saturation/value/alpha, kept apart so a grey keeps its hue while dragging.
    property real hue: 0
    property real saturation: 0
    property real value: 1
    property real alpha: 1
    readonly property color current: Qt.hsva(hue, saturation, value, alpha)

    signal colorPicked(string hex)

    /// Opens the picker on `initial`, titled `label`.
    function edit(initial: color, label: string) {
        root.title = label
        if (initial.hsvHue >= 0)
            root.hue = initial.hsvHue
        root.saturation = initial.hsvSaturation
        root.value = initial.hsvValue
        root.alpha = root.allowAlpha ? initial.a : 1
        hexField.text = root.hexOf(root.current)
        root.open()
    }
    function hexOf(c: color): string {
        const two = v => ("0" + Math.round(Math.max(0, Math.min(1, v)) * 255).toString(16)).slice(-2).toUpperCase()
        const withAlpha = root.allowAlpha && c.a < 0.998
        return "#" + (withAlpha ? two(c.a) : "") + two(c.r) + two(c.g) + two(c.b)
    }
    function set(h: real, s: real, v: real, a: real) {
        root.hue = Math.max(0, Math.min(1, h))
        root.saturation = Math.max(0, Math.min(1, s))
        root.value = Math.max(0, Math.min(1, v))
        root.alpha = root.allowAlpha ? Math.max(0, Math.min(1, a)) : 1
        const hex = root.hexOf(root.current)
        if (!hexField.activeFocus)
            hexField.text = hex
        root.colorPicked(hex)
    }

    transformOrigin: Item.TopLeft

    Column {
        width: Theme.space(60)
        spacing: Theme.space(3)

        // ---- Saturation (x) and brightness (y) ----
        Item {
            id: square
            objectName: "colorPickerSquare"
            width: parent.width
            height: Theme.space(40)
            activeFocusOnTab: true
            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Saturation %1 %, brightness %2 %").arg(Math.round(root.saturation * 100))
                                                                     .arg(Math.round(root.value * 100))
            Keys.onPressed: event => {
                const step = (event.modifiers & Qt.ShiftModifier) ? 0.1 : 0.01
                if (event.key === Qt.Key_Left) root.set(root.hue, root.saturation - step, root.value, root.alpha)
                else if (event.key === Qt.Key_Right) root.set(root.hue, root.saturation + step, root.value, root.alpha)
                else if (event.key === Qt.Key_Up) root.set(root.hue, root.saturation, root.value + step, root.alpha)
                else if (event.key === Qt.Key_Down) root.set(root.hue, root.saturation, root.value - step, root.alpha)
                else return
                event.accepted = true
            }
            Keys.onShortcutOverride: event => event.accepted = [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down].includes(event.key)

            Rectangle {
                anchors.fill: parent
                radius: Theme.radiusField
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: Theme.pickerWhite }
                    GradientStop { position: 1; color: Qt.hsva(root.hue, 1, 1, 1) }
                }
            }
            Rectangle {
                anchors.fill: parent
                radius: Theme.radiusField
                border.width: Theme.hairline
                border.color: Theme.borderStrong
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.pickerClear }
                    GradientStop { position: 1; color: Theme.pickerBlack }
                }
            }
            Rectangle {
                width: Theme.space(3.5)
                height: width
                radius: width / 2
                x: root.saturation * square.width - width / 2
                y: (1 - root.value) * square.height - height / 2
                color: "transparent"
                border.width: Theme.focusRingWidth
                border.color: root.value > 0.55 && root.saturation < 0.45 ? Theme.pickerBlack : Theme.pickerWhite
            }
            FocusRing {
                targetRadius: Theme.radiusField
                visible: square.activeFocus
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.CrossCursor
                function pick(mouse: MouseEvent) {
                    root.set(root.hue, mouse.x / square.width, 1 - mouse.y / square.height, root.alpha)
                }
                onPressed: mouse => {
                    square.forceActiveFocus(Qt.MouseFocusReason)
                    pick(mouse)
                }
                onPositionChanged: mouse => pick(mouse)
            }
        }

        // ---- Hue ----
        PickerStrip {
            objectName: "colorPickerHue"
            width: parent.width
            position: root.hue
            accessibleName: qsTr("Hue %1 degrees").arg(Math.round(root.hue * 360))
            onMoved: position => root.set(position, root.saturation, root.value, root.alpha)
            stops: Theme.pickerHueStops
        }

        // ---- Opacity ----
        PickerStrip {
            objectName: "colorPickerAlpha"
            visible: root.allowAlpha
            width: parent.width
            position: root.alpha
            accessibleName: qsTr("Opacity %1 %").arg(Math.round(root.alpha * 100))
            onMoved: position => root.set(root.hue, root.saturation, root.value, position)
            stops: [Qt.hsva(root.hue, root.saturation, root.value, 0), Qt.hsva(root.hue, root.saturation, root.value, 1)]
        }

        Row {
            spacing: Theme.space(2)

            Rectangle {
                width: Theme.controlHeightSmall + Theme.space(1)
                height: width
                radius: Theme.radiusField
                color: root.current
                border.width: Theme.hairline
                border.color: Theme.borderStrong
                Accessible.ignored: true
            }
            FieldInput {
                id: hexField
                objectName: "colorPickerHex"
                width: Theme.space(32)
                font.family: Theme.fontMono
                Accessible.name: qsTr("Hex colour")
                onEditingFinished: {
                    const t = text.trim()
                    const hex = (t.startsWith("#") ? t : "#" + t).toUpperCase()
                    if (/^#([0-9A-F]{6}|[0-9A-F]{8})$/.test(hex)) {
                        const c = Qt.color(hex)
                        root.set(c.hsvHue >= 0 ? c.hsvHue : root.hue, c.hsvSaturation, c.hsvValue, c.a)
                    } else {
                        text = root.hexOf(root.current)
                    }
                }
            }
            PillButton {
                objectName: "colorPickerDone"
                compact: true
                text: qsTr("Done")
                onClicked: root.close()
            }
        }
    }
}
