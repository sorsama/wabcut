import QtQuick
import QtQuick.Controls.Basic

// Tooltip with a keyboard-shortcut slot: "Play/Stop  [Space]" (design language v3: an inverted pill, light on the dark
// theme, with the key in an accent chip). The shortcut text comes from the keymap (KeymapDispatcher.shortcutText, via
// the buttons' `command`). It pops from its trigger with a short fade + scale.
ToolTip {
    id: control

    property string shortcut: ""

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

    contentItem: Row {
        spacing: Theme.space(2)

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font: control.font
            color: Theme.canvas
        }

        Rectangle {
            visible: control.shortcut !== ""
            anchors.verticalCenter: parent.verticalCenter
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
        radius: height / 2
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
