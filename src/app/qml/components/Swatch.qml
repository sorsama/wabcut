pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Colour swatch (the accent picker, label colours): a small circle (design language v3). Checked = a ring in the
// primary text colour around the chip.
AbstractButton {
    id: control

    property color swatchColor: Theme.accentFill
    property string tooltip: ""

    implicitWidth: Theme.swatchSize + Theme.space(2)
    implicitHeight: implicitWidth
    checkable: true
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    Accessible.role: Accessible.RadioButton
    Accessible.name: tooltip

    scale: control.down ? Theme.pressScale : 1.0
    Behavior on scale {
        NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }

    contentItem: Item {
        Rectangle {
            anchors.centerIn: parent
            width: Theme.swatchSize
            height: Theme.swatchSize
            radius: width / 2
            color: control.swatchColor
            border.width: Theme.hairline
            border.color: Theme.borderStrong
        }
    }

    background: Rectangle {
        radius: width / 2
        color: "transparent"
        border.width: Theme.focusRingWidth
        border.color: control.checked ? Theme.textPrimary : control.hovered ? Theme.borderStrong : "transparent"

        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

        FocusRing {
            targetRadius: control.width / 2
            visible: control.visualFocus
        }

    }

    HoverHandler {
        enabled: control.enabled
        cursorShape: Qt.PointingHandCursor
    }

    Loader {
        active: control.hovered && control.tooltip !== ""
        sourceComponent: Tooltip {
            parent: control
            visible: true
            text: control.tooltip
        }
    }
}
