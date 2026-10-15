pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// On/off switch (design language v3): a pill track, accent when on and `well` when off, with a white round thumb.
// Direct manipulation: pressing stretches the thumb, dragging moves it with the pointer, and releasing flings it to
// the nearest end with a small bounce (easeSpring) — toggles are one of the few elements allowed to bounce.
// Reduce-motion: no stretch, no bounce, instant state.
Switch {
    id: control

    property string tooltip: ""

    // The content Text already reserves the indicator in its leftPadding.
    implicitWidth: Math.max(implicitIndicatorWidth, text !== "" ? implicitContentWidth : 0) + leftPadding + rightPadding
    implicitHeight: Math.max(implicitIndicatorHeight, implicitContentHeight) + topPadding + bottomPadding
    padding: Theme.space(1)
    spacing: Theme.space(2)
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    font.family: Theme.fontSans
    font.pixelSize: Theme.textMd

    Accessible.name: text !== "" ? text : tooltip

    indicator: Rectangle {
        id: track

        readonly property real inset: (height - Theme.toggleThumb) / 2

        implicitWidth: Theme.toggleWidth
        implicitHeight: Theme.toggleHeight
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: height / 2
        color: control.checked ? Theme.accentFill : control.hovered ? Theme.hover : Theme.well
        opacity: control.enabled ? 1 : Theme.disabledOpacity

        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        Rectangle {
            id: thumb

            readonly property real stretch: control.pressed && !Theme.reduceMotion ? Theme.toggleThumbStretch : 0

            width: Theme.toggleThumb + stretch
            height: Theme.toggleThumb
            radius: height / 2
            y: track.inset
            x: track.inset + control.visualPosition * (track.width - width - 2 * track.inset)
            // White in both themes, like a slider knob (the pill kit's switch).
            color: Theme.knob

            // Follow the pointer 1:1 while pressed/dragged; settle with a bounce on release.
            Behavior on x {
                enabled: !control.pressed
                NumberAnimation { duration: Theme.durBounce; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSpring }
            }
            Behavior on width {
                NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
            }
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }

        FocusRing {
            targetRadius: track.height / 2
            visible: control.visualFocus
        }

    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        verticalAlignment: Text.AlignVCenter
        text: control.text
        font: control.font
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        visible: control.text !== ""
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
