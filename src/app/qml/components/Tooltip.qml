import QtQuick
import QtQuick.Controls.Basic

// Tooltip with a keyboard-shortcut slot: "Play/Stop  [Space]" (design language v3: an inverted pill, light on the dark
// theme, with the key in an accent chip). The shortcut text comes from the keymap (KeymapDispatcher.shortcutText, via
// the buttons' `command`). It pops from its trigger with a short fade + scale.
// P1 round (#31): a long text wraps at `maxTextWidth` (the pill becomes a rounded box); the shortcut chip stays on the
// first line.
ToolTip {
    id: control

    property string shortcut: ""
    /// Where a long text wraps.
    property real maxTextWidth: Theme.space(80)

    delay: Theme.tooltipDelay
    timeout: -1
    margins: Theme.space(2)
    topPadding: Theme.space(1.5)
    bottomPadding: Theme.space(1.5)
    leftPadding: Theme.space(3.5)
    rightPadding: Theme.space(shortcut !== "" ? 1.5 : 3.5)
    font.family: Theme.fontSans
    font.pixelSize: Theme.textSm
    font.weight: Font.DemiBold

    contentItem: Item {
        implicitWidth: label.width + (chip.visible ? Theme.space(2) + chip.width : 0)
        implicitHeight: Math.max(label.y + label.height, chip.visible ? chip.y + chip.height : 0)

        FontMetrics {
            id: metrics
            font: control.font
        }
        // The text on one line: how wide the label is before it wraps.
        Text {
            id: natural
            visible: false
            text: control.text
            font: control.font
        }

        Text {
            id: label
            objectName: "tooltipText"
            y: chip.visible ? Math.max(0, (chip.height - metrics.height) / 2) : 0
            width: Math.min(Math.ceil(natural.implicitWidth), control.maxTextWidth)
            text: control.text
            font: control.font
            color: Theme.canvas
            wrapMode: Text.Wrap
        }

        // The shortcut chip, centred on the first line.
        Rectangle {
            id: chip
            objectName: "tooltipShortcut"
            visible: control.shortcut !== ""
            x: label.width + Theme.space(2)
            y: Math.max(0, (metrics.height - height) / 2)
            width: Math.max(height, keyText.implicitWidth + Theme.space(3))
            height: keyText.implicitHeight + Theme.space(1)
            radius: height / 2
            color: Theme.accentFill

            Text {
                id: keyText
                anchors.centerIn: parent
                text: control.shortcut
                color: Theme.accentOnFill
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
                font.weight: Font.Bold
            }
        }
    }

    background: Rectangle {
        radius: label.lineCount > 1 ? Theme.radiusControl : height / 2
        color: Theme.textPrimary
    }


    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durFast; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: Theme.popScale; to: 1; duration: Theme.durFast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durPress; easing.type: Easing.OutCubic }
    }
}
