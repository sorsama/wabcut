import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects

// Popover menu that pops from its trigger (fade + scale from the top edge, ease-out, no bounce). Design language v3:
// a raised card, radius 20, a hairline ring and a soft shadow; its rows are pills.
// Use ContextMenuItem children; `checkable` items show a check mark when checked.
Menu {
    id: control

    padding: Theme.space(1.5)
    margins: Theme.space(2)
    // Menu does not grow to its widest item by itself: size to it (labels + shortcuts never collide).
    implicitWidth: {
        let widest = Theme.space(48)
        for (let i = 0; i < count; ++i) {
            const item = itemAt(i)
            if (item)
                widest = Math.max(widest, item.implicitWidth + leftPadding + rightPadding)
        }
        return widest
    }
    transformOrigin: Item.Top
    font.family: Theme.fontSans
    font.pixelSize: Theme.textMd

    delegate: ContextMenuItem {}

    background: Rectangle {
        implicitWidth: Theme.space(48)
        radius: Theme.radiusMenu
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.borderStrong

        RectangularShadow {
            z: -1
            anchors.fill: parent
            radius: parent.radius
            offset.y: Theme.space(2)
            blur: Theme.space(8)
            color: Theme.shadow
        }
    }


    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durFast; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: Theme.popScale; to: 1; duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durPress; easing.type: Easing.OutCubic }
    }
}
