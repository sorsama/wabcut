pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// A labelled on/off control for an editing mode (0.1.3: Snap and Linked selection in the Tracks toolbar): the mode's
// icon and a short label on a pill (design language v3). While on it is the accent pill with black icon and label;
// off, a raised pill. `showLabel` false keeps the icon only (narrow panels); the tooltip always says the mode and its
// key.
Button {
    id: control

    property string iconName: ""
    property string tooltip: ""
    // Keymap command id (docs/keymap.md): its current shortcut shows in the tooltip.
    property string command: ""
    property string shortcut: command !== "" && KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText(command) : ""
    property bool showLabel: true

    checkable: true
    implicitHeight: Theme.controlHeightSmall
    implicitWidth: showLabel ? row.implicitWidth + Theme.space(4) : Theme.controlHeightSmall
    padding: 0
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    scale: control.down ? Theme.pressScale : 1.0
    Behavior on scale {
        NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }

    Accessible.name: control.text
    Accessible.description: control.tooltip
    Accessible.role: Accessible.CheckBox
    Accessible.checkable: true
    Accessible.checked: control.checked

    contentItem: Item {
        Row {
            id: row
            anchors.centerIn: parent
            spacing: Theme.space(1.5)

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: control.iconName
                size: Theme.iconSm
                color: !control.enabled ? Theme.textDisabled
                     : control.checked ? Theme.accentOnFill
                     : control.hovered ? Theme.textPrimary
                     : Theme.textSecondary
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.showLabel
                text: control.text
                color: !control.enabled ? Theme.textDisabled
                     : control.checked ? Theme.accentOnFill
                     : control.hovered ? Theme.textPrimary
                     : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.Bold
            }
        }
    }

    background: Rectangle {
        radius: height / 2
        color: control.checked ? (control.down ? Theme.accentDeep : Theme.accentFill)
             : control.down || control.hovered ? Theme.hover : Theme.raised
        opacity: control.enabled ? 1 : Theme.disabledOpacity

        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        FocusRing {
            targetRadius: height / 2
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
            shortcut: control.shortcut
        }
    }
}
