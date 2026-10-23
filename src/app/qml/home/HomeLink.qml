pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// A small text link on Home (design language v3): an optional icon and a bold 12 px label in the secondary text colour,
// the accent and an underline under the pointer. "Import folder...", "How to export", "Reset to defaults".
AbstractButton {
    id: link

    property string iconName: ""
    property string tooltip: ""

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    padding: Theme.space(1)
    Accessible.role: Accessible.Link
    Accessible.name: text
    Accessible.description: tooltip
    Keys.onReturnPressed: link.clicked()
    Keys.onEnterPressed: link.clicked()

    contentItem: Row {
        spacing: Theme.space(1)
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            visible: link.iconName !== ""
            name: link.iconName
            size: Theme.iconXs
            color: link.hovered ? Theme.accentStroke : Theme.textSecondary
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: link.text
            color: link.hovered ? Theme.accentStroke : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Theme.weightLabel
            font.underline: link.hovered
        }
    }
    background: null
    Loader {
        active: link.hovered && link.tooltip !== ""
        sourceComponent: Tooltip {
            parent: link
            visible: true
            text: link.tooltip
        }
    }
    FocusRing {
        targetRadius: Theme.radiusBadge
        visible: link.visualFocus
    }
    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }
}
