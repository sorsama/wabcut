import QtQuick
import QtQuick.Effects

// A toast (design language v3): a short message on a rounded card with a coloured dot that slides in at the
// bottom-right, then leaves after a few seconds. show(text) replaces what is shown. Anchor it to the window's
// bottom-right corner.
Item {
    id: root
    objectName: "toast"

    property string text: ""
    // The dot's colour (Theme.danger for a problem).
    property color accent: Theme.accentStroke
    property int timeout: 4000

    function show(message, color) {
        text = message
        accent = color ?? Theme.accentStroke
        shown = true
        hideTimer.restart()
    }

    property bool shown: false

    implicitWidth: Math.min(Theme.space(110), label.implicitWidth + Theme.space(10))
    implicitHeight: Theme.controlHeight + Theme.space(2)
    visible: opacity > 0
    opacity: shown ? 1 : 0
    enabled: false

    Behavior on opacity { NumberAnimation { duration: Theme.durState; easing.type: Easing.OutCubic } }

    Timer {
        id: hideTimer
        interval: root.timeout
        onTriggered: root.shown = false
    }

    Item {
        width: root.width
        height: root.height
        x: root.shown || Theme.reduceMotion ? 0 : Theme.space(6)
        Behavior on x { NumberAnimation { duration: Theme.durCollapse; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSnapOut } }

        Rectangle {
            anchors.fill: parent
            color: Theme.section
            radius: Theme.radiusCard
            border.width: Theme.hairline
            border.color: Theme.border

            RectangularShadow {
                z: -1
                anchors.fill: parent
                radius: parent.radius
                offset.y: Theme.space(2)
                blur: Theme.space(8)
                color: Theme.shadow
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(4)
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.dotSize
            height: Theme.dotSize
            radius: width / 2
            color: root.accent
        }
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.space(4) + Theme.dotSize + Theme.space(2.5)
            anchors.rightMargin: Theme.space(5)
            text: root.text
            elide: Text.ElideMiddle
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            font.weight: Font.DemiBold

            Accessible.role: Accessible.AlertMessage
            Accessible.name: root.text
        }
    }
}
