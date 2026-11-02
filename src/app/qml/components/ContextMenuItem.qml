pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Row of a ContextMenu (design language v3: a pill that lights up under the pointer): optional icon, label, keyboard
// shortcut and a check mark for checkable items. A submenu's row (a ContextMenu declared or inserted into another)
// ends in a chevron.
// P1 round (#31): a disabled row with a `disabledReason` shows it in a tooltip while the pointer rests on it (a Menu
// does not highlight disabled rows, so a hover handler of the row's own sees the pointer).
MenuItem {
    id: control

    property string iconName: ""
    /// The icon's colour (a label colour in the Library's Label menu); by default the text's.
    property color iconColor: control.highlighted ? Theme.textPrimary : Theme.textSecondary
    // Keymap command id (docs/keymap.md); the tooltip shows its current shortcut from KeymapDispatcher.
    property string command: ""
    property string shortcut: command !== "" && KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText(command) : ""
    /// Why the row cannot be chosen now ("Select a clip first"): its tooltip and accessible description while disabled.
    property string disabledReason: ""

    implicitHeight: Theme.controlHeightSmall + Theme.space(1)
    leftPadding: Theme.space(3.5)
    rightPadding: Theme.space(3.5)
    spacing: Theme.space(2)
    hoverEnabled: true
    font.family: Theme.fontSans
    font.pixelSize: Theme.textMd
    Accessible.description: !enabled ? disabledReason : ""

    indicator: null
    arrow: null

    contentItem: Item {
        implicitWidth: row.implicitWidth + Theme.space(4) + (control.subMenu ? Theme.iconSm : keys.implicitWidth)
        implicitHeight: row.implicitHeight

        Row {
            id: row
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: control.spacing

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.dotSize
                height: Theme.dotSize
                radius: Theme.dotSize / 2
                color: Theme.accentStroke
                visible: control.checkable
                opacity: control.checked ? 1 : 0
            }

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.iconName !== ""
                name: control.iconName
                size: Theme.iconSm
                color: control.iconColor
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                font.family: control.font.family
                font.pixelSize: control.font.pixelSize
                font.weight: Font.DemiBold
                color: control.enabled ? Theme.textPrimary : Theme.textDisabled
            }
        }

        Icon {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: control.subMenu !== null
            name: "chevron-right"
            size: Theme.iconSm
            color: control.enabled ? Theme.textSecondary : Theme.textDisabled
        }

        Text {
            id: keys
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: control.subMenu === null
            text: control.shortcut
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
    }

    background: Rectangle {
        radius: height / 2
        color: control.highlighted ? Theme.hover : "transparent"

        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }

    HoverHandler {
        enabled: control.enabled
        cursorShape: Qt.PointingHandCursor
    }
    // Sees the pointer on a disabled row too, for its reason; no cursor of its own.
    HoverHandler {
        id: hoverAll
    }
    Loader {
        objectName: "menuItemReasonTip"
        active: !control.enabled && control.disabledReason !== "" && hoverAll.hovered
        sourceComponent: Tooltip {
            parent: control
            visible: true
            text: control.disabledReason
        }
    }
}
