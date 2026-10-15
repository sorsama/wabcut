pragma ComponentBehavior: Bound
import QtQuick

// A horizontal strip of a colour picker (0.1.3: hue, opacity): a gradient through `stops` with a handle at
// `position` (0..1). Drag or click to set it; Left/Right (Shift: x10) when focused. Reports `moved(position)`.
Item {
    id: strip

    property real position: 0
    property var stops: []
    property string accessibleName: ""
    signal moved(real position)

    function stopPosition(i: int): real {
        const n = strip.stops.length
        return n > 1 ? Math.min(i, n - 1) / (n - 1) : 0
    }
    function stopColor(i: int): color {
        const n = strip.stops.length
        return n > 0 ? strip.stops[Math.min(i, n - 1)] : "transparent"
    }
    function setTo(p: real) {
        strip.moved(Math.max(0, Math.min(1, p)))
    }

    height: Theme.space(4)
    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: strip.accessibleName
    Keys.onPressed: event => {
        const step = (event.modifiers & Qt.ShiftModifier) ? 0.1 : 0.01
        if (event.key === Qt.Key_Left)
            strip.setTo(strip.position - step)
        else if (event.key === Qt.Key_Right)
            strip.setTo(strip.position + step)
        else
            return
        event.accepted = true
    }
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Left || event.key === Qt.Key_Right

    Rectangle {
        id: bar
        anchors.fill: parent
        radius: height / 2
        border.width: Theme.hairline
        border.color: Theme.borderStrong
        // Up to seven stops (the hue strip's); a shorter list repeats its last colour at the end.
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: strip.stopPosition(0); color: strip.stopColor(0) }
            GradientStop { position: strip.stopPosition(1); color: strip.stopColor(1) }
            GradientStop { position: strip.stopPosition(2); color: strip.stopColor(2) }
            GradientStop { position: strip.stopPosition(3); color: strip.stopColor(3) }
            GradientStop { position: strip.stopPosition(4); color: strip.stopColor(4) }
            GradientStop { position: strip.stopPosition(5); color: strip.stopColor(5) }
            GradientStop { position: strip.stopPosition(6); color: strip.stopColor(6) }
        }
    }

    Rectangle {
        width: Theme.space(2)
        height: parent.height + Theme.space(2)
        y: -Theme.space(1)
        x: strip.position * (strip.width - width)
        radius: Theme.radiusBadge
        color: Theme.textPrimary
        border.width: Theme.hairline
        border.color: Theme.canvas
    }
    FocusRing {
        targetRadius: strip.height / 2
        visible: strip.activeFocus
    }
    MouseArea {
        anchors.fill: parent
        anchors.topMargin: -Theme.space(1)
        anchors.bottomMargin: -Theme.space(1)
        cursorShape: Qt.PointingHandCursor
        onPressed: mouse => {
            strip.forceActiveFocus(Qt.MouseFocusReason)
            strip.setTo(mouse.x / strip.width)
        }
        onPositionChanged: mouse => strip.setTo(mouse.x / strip.width)
    }
}
