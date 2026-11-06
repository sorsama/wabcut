pragma ComponentBehavior: Bound
import QtQuick

// A filter chip of Settings > AI (r07): a compact pill that is the accent fill while `checked` (black text on it),
// raised otherwise, with an optional leading icon. Clicking emits clicked(); the owner decides what is checked.
Rectangle {
    id: chip

    property string text: ""
    property string iconName: ""
    property bool checked: false
    property string tooltip: ""
    signal clicked()

    implicitHeight: Theme.controlHeightSmall
    implicitWidth: row.implicitWidth + Theme.space(6)
    radius: height / 2
    color: chip.checked ? Theme.accentFill : mouse.containsMouse ? Theme.hover : Theme.raised
    border.width: chip.checked ? 0 : Theme.hairline
    border.color: Theme.border
    activeFocusOnTab: true

    Accessible.role: Accessible.RadioButton
    Accessible.name: chip.text
    Accessible.description: chip.tooltip
    Accessible.checkable: true
    Accessible.checked: chip.checked
    Accessible.onPressAction: chip.clicked()
    Keys.onSpacePressed: chip.clicked()
    Keys.onReturnPressed: chip.clicked()

    Behavior on color { ColorAnimation { duration: Theme.durFast } }

    FocusRing {
        targetRadius: chip.radius
        visible: chip.activeFocus
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.space(1.5)

        Icon {
            anchors.verticalCenter: parent.verticalCenter
            visible: chip.iconName !== ""
            name: chip.iconName
            size: Theme.iconXs
            color: chip.checked ? Theme.accentOnFill : Theme.textSecondary
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: chip.text
            color: chip.checked ? Theme.accentOnFill : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Theme.weightLabel
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            chip.forceActiveFocus(Qt.MouseFocusReason)
            chip.clicked()
        }
    }

    Loader {
        active: mouse.containsMouse && chip.tooltip !== ""
        sourceComponent: Tooltip {
            parent: chip
            visible: true
            text: chip.tooltip
        }
    }
}
