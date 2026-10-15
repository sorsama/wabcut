pragma ComponentBehavior: Bound
import QtQuick

// Read-only status chip (design language v3: a raised pill): a colored dot, a label and an optional monospace
// detail, e.g. "● GPU D3D11 · RTX 3060".
// The dot is a static state cue (no pulsing, no glow). An optional `tooltip` shows on hover.
Rectangle {
    id: root

    property string label: ""
    property string detail: ""
    property color dotColor: Theme.success
    property string tooltip: ""

    implicitWidth: row.implicitWidth + Theme.space(6)
    implicitHeight: Theme.controlHeightSmall
    radius: height / 2
    color: Theme.raised

    Accessible.role: Accessible.StaticText
    Accessible.name: detail !== "" ? label + " " + detail : label
    Accessible.description: tooltip

    HoverHandler {
        id: hover
    }

    Loader {
        active: hover.hovered && root.tooltip !== ""
        sourceComponent: Tooltip {
            parent: root
            visible: true
            text: root.tooltip
        }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.space(2)

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.dotSize
            height: Theme.dotSize
            radius: Theme.dotSize / 2
            color: root.dotColor
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Theme.weightLabel

        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.detail !== ""
            text: root.detail
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
    }
}
