import QtQuick
import QtQuick.Controls.Basic

// Minimize / maximize-restore / close pill buttons for the frameless window. Close turns red on hover.
Row {
    id: root

    property bool maximized: false

    signal minimizeRequested()
    signal maximizeRequested()
    signal closeRequested()

    spacing: Theme.space(1)

    component WindowButton: Button {
        id: button

        property string iconName: ""
        property string tooltip: ""
        property color hoverColor: Theme.hover
        property color hoverIconColor: Theme.textPrimary

        implicitWidth: Theme.controlHeightSmall + Theme.space(3)
        implicitHeight: Theme.controlHeightSmall
        padding: 0
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.name: tooltip

        scale: down ? Theme.pressScale : 1.0
        Behavior on scale {
            NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
        }

        contentItem: Item {
            Icon {
                anchors.centerIn: parent
                name: button.iconName
                size: Theme.iconSm
                color: button.hovered ? button.hoverIconColor : Theme.textSecondary
            }
        }

        background: Rectangle {
            radius: height / 2
            color: button.hovered || button.down ? button.hoverColor : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.durFast } }

            FocusRing {
                targetRadius: parent.height / 2
                visible: button.visualFocus
            }
        }

        HoverHandler {
            enabled: button.enabled
            cursorShape: Qt.PointingHandCursor
        }
    }

    WindowButton {
        iconName: "minus"
        tooltip: qsTr("Minimize")
        onClicked: root.minimizeRequested()
    }

    WindowButton {
        iconName: root.maximized ? "copy" : "square"
        tooltip: root.maximized ? qsTr("Restore") : qsTr("Maximize")
        onClicked: root.maximizeRequested()
    }

    WindowButton {
        iconName: "x"
        tooltip: qsTr("Close")
        hoverColor: Theme.danger
        hoverIconColor: Theme.scrimText
        onClicked: root.closeRequested()
    }
}
