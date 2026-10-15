import QtQuick
import QtQuick.Controls.Basic

// Thin themed slider (design language v3): a 4 px pill track in `well`, the accent fill from `origin` to the value,
// and a white round knob that grows while grabbed. Used by PropSlider and the timeline zoom. `origin` defaults to
// `from`; set it to 0 for bipolar ranges (rotation, pan, dB) so the fill grows outward from zero.
Slider {
    id: control

    property real origin: from
    readonly property real originPosition: to !== from ? (origin - from) / (to - from) : 0

    implicitWidth: Theme.space(30)
    implicitHeight: Theme.controlHeightSmall
    padding: 0
    leftPadding: Theme.sliderHandle / 2
    rightPadding: Theme.sliderHandle / 2
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    background: Item {
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: control.availableWidth
        height: Theme.sliderTrack
        implicitWidth: Theme.space(20)
        implicitHeight: Theme.sliderTrack

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: Theme.well
        }

        Rectangle {
            readonly property real a: Math.min(control.originPosition, control.visualPosition)
            readonly property real b: Math.max(control.originPosition, control.visualPosition)
            x: a * parent.width
            width: (b - a) * parent.width
            height: parent.height
            radius: height / 2
            color: control.enabled ? Theme.accentStroke : Theme.textDisabled
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * control.availableWidth - width / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        implicitWidth: Theme.sliderHandle
        implicitHeight: Theme.sliderHandle
        radius: width / 2
        // White in both themes; on light surfaces a hairline keeps it from melting into the panel.
        color: control.enabled ? Theme.knob : Theme.textDisabled
        border.width: Theme.dark ? 0 : Theme.hairline
        border.color: Theme.borderStrong
        scale: control.pressed ? Theme.grabScale : control.hovered ? Theme.liftScale : 1

        Behavior on scale {
            NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
        }

        FocusRing {
            targetRadius: Theme.sliderHandle / 2
            visible: control.visualFocus
        }

    }

    HoverHandler {
        enabled: control.enabled
        cursorShape: Qt.PointingHandCursor
    }
}
