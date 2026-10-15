pragma ComponentBehavior: Bound
import QtQuick

// The badge of a plugin effect (A.6; Plugins.effectInfo): a micro-tag when it cannot run (MISSING in warning: not on
// this computer, kept and bypassed; CRASHED / FAILED / HUNG in danger: bypassed until it is played again), else a
// warning dot when it missed blocks on the master. The full reason is its tooltip.
Item {
    id: root

    /// "" | "missing" | "crashed" | "failed" | "hung"
    property string kind: ""
    /// The reason ("Missing plugin: TDR Nova (Tokyo Dawn Labs)").
    property string detail: ""
    property int missedBlocks: 0

    readonly property bool late: kind === "" && missedBlocks > 0
    readonly property color tint: kind === "missing" || late ? Theme.warning : Theme.danger
    readonly property string tip: late ? qsTr("Missed %n block(s) on the master", "", missedBlocks) : detail

    objectName: "pluginBadge"
    visible: kind !== "" || late
    implicitWidth: late ? Theme.dotSize : chip.width
    implicitHeight: late ? Theme.dotSize : chip.height
    Accessible.role: Accessible.StaticText
    Accessible.name: tip

    Rectangle {
        id: chip
        visible: !root.late
        width: label.implicitWidth + Theme.space(2)
        height: label.implicitHeight + Theme.space(1)
        radius: Theme.radiusBadge
        color: root.tint
        Text {
            id: label
            anchors.centerIn: parent
            text: root.kind === "missing" ? qsTr("Missing")
                : root.kind === "crashed" ? qsTr("Crashed")
                : root.kind === "failed" ? qsTr("Failed")
                : root.kind === "hung" ? qsTr("Hung") : ""
            color: Theme.canvas
            font.family: Theme.fontSans
            font.capitalization: Font.AllUppercase
            font.pixelSize: Theme.textMicro
            font.letterSpacing: Theme.microTracking
            font.weight: Font.DemiBold
        }
    }
    Rectangle {
        visible: root.late
        anchors.centerIn: parent
        width: Theme.dotSize
        height: Theme.dotSize
        radius: width / 2
        color: Theme.warning
    }

    HoverHandler {
        id: hover
    }
    Loader {
        active: hover.hovered && root.tip !== ""
        sourceComponent: Tooltip {
            parent: root
            visible: true
            text: root.tip
        }
    }
}
