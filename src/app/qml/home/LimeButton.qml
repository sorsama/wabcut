pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// The brand's lime pill (design language v3: lime only for Create and brand moments): the one Create on Home's New
// project page. Black text, ExtraBold; press scales to 0.98; a tooltip after the usual delay.
AbstractButton {
    id: lime

    property string tooltip: ""

    implicitHeight: Theme.controlHeightLarge
    implicitWidth: limeText.implicitWidth + Theme.space(10)
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    scale: down ? Theme.pressScale : 1
    Behavior on scale { NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSnapOut } }
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: tooltip
    Keys.onReturnPressed: lime.clicked()
    Keys.onEnterPressed: lime.clicked()

    background: Rectangle {
        radius: height / 2
        opacity: lime.enabled ? 1 : Theme.disabledOpacity
        color: lime.down ? Qt.darker(Theme.brandLime, 1.08) : lime.hovered ? Qt.lighter(Theme.brandLime, 1.06) : Theme.brandLime
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }
    contentItem: Text {
        id: limeText
        text: lime.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        color: Theme.clipInk
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        font.weight: Theme.weightTitle
    }
    Loader {
        active: lime.hovered && lime.tooltip !== ""
        sourceComponent: Tooltip {
            parent: lime
            visible: true
            text: lime.tooltip
        }
    }
    FocusRing {
        targetRadius: lime.height / 2
        visible: lime.visualFocus
    }
    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }
}
