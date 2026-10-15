pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes

// A pan knob (Audio Track Mixer, M5g): −1 (left) … +1 (right), an arc from the centre to the value. Drag up/down
// (Shift: fine), double-click centres, ←/→ step 0.05. `moved(pan)` asks for the change; disabled knobs show `note`.
Item {
    id: root

    property real pan: 0
    property string note: ""
    property string label: ""

    /// The knob is held (M5l: Touch and Latch automation write while it is).
    readonly property bool held: area.pressed

    signal moved(real pan)
    signal released()

    function step(delta) {
        root.moved(Math.max(-1, Math.min(1, Math.round((pan + delta) * 100) / 100)))
        root.released()
    }

    implicitWidth: Theme.space(8)
    implicitHeight: Theme.space(8)
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : Theme.disabledOpacity
    Accessible.role: Accessible.Dial
    Accessible.name: label
    Accessible.description: note

    Keys.onLeftPressed: step(-0.05)
    Keys.onRightPressed: step(0.05)

    readonly property real radius: Math.min(width, height) / 2 - Theme.space(1)
    // -135° (full left) .. +135° (full right), 0 straight up; Shapes angles start at 3 o'clock.
    readonly property real angle: pan * 135

    Rectangle {
        anchors.centerIn: parent
        width: root.radius * 2
        height: width
        radius: width / 2
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: area.containsMouse ? Theme.accentStroke : Theme.border
    }
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: Theme.accentStroke
            strokeWidth: Theme.focusRingWidth
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.width / 2
                centerY: root.height / 2
                radiusX: root.radius - Theme.space(1)
                radiusY: root.radius - Theme.space(1)
                startAngle: -90
                sweepAngle: root.angle
            }
        }
    }
    // Pointer
    Rectangle {
        x: root.width / 2 - width / 2
        y: root.height / 2 - root.radius + Theme.space(1)
        width: Theme.focusRingWidth
        height: root.radius - Theme.space(1)
        radius: width / 2
        color: Theme.textPrimary
        transformOrigin: Item.Bottom
        rotation: root.angle
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        property real pressY: 0
        property real pressPan: 0
        onPressed: mouse => {
            root.forceActiveFocus(Qt.MouseFocusReason)
            pressY = mouse.y
            pressPan = root.pan
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const scale = (mouse.modifiers & Qt.ShiftModifier) ? 0.002 : 0.01
            root.moved(Math.max(-1, Math.min(1, Math.round((pressPan + (pressY - mouse.y) * scale) * 100) / 100)))
        }
        onReleased: root.released()
        onDoubleClicked: {
            root.moved(0)
            root.released()
        }
    }

    Loader {
        active: area.containsMouse && (root.note !== "" || root.label !== "")
        sourceComponent: Tooltip {
            parent: root
            visible: true
            text: root.enabled ? root.label : root.note
        }
    }

    FocusRing {
        targetRadius: root.radius
        visible: root.activeFocus
    }
}
