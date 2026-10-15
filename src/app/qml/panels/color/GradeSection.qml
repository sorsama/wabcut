import QtQuick
import QtQuick.Layouts

// A collapsible section of the Color panel: chevron · title · stage switch · reset, then its rows. The stage switch
// is the grade's "<stage>.enabled" parameter; reset puts the stage back to its defaults (one undo step).
ColumnLayout {
    id: root

    property string title: ""
    /// The grade stage ("basic", "creative", "curves", "wheels", "hsl", "vignette"); "" = no switch or reset.
    property string stage: ""
    property bool expanded: true
    default property alias content: body.data
    readonly property bool stageOn: stage === "" || (Color.params[stage + ".enabled"] ?? 1) > 0.5

    spacing: 0
    Layout.fillWidth: true

    Item {
        Layout.fillWidth: true
        implicitHeight: Theme.controlHeight

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusField
            color: header.containsMouse ? Theme.hover : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }

        MouseArea {
            id: header
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.expanded = !root.expanded
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.space(2)
            anchors.rightMargin: Theme.space(1)
            spacing: Theme.space(2)

            Icon {
                name: "chevron-right"
                size: Theme.iconSm
                color: Theme.textTertiary
                rotation: root.expanded ? 90 : 0
                Behavior on rotation { NumberAnimation { duration: Theme.durFast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft } }
            }
            Text {
                Layout.fillWidth: true
                text: root.title
                color: root.stageOn ? Theme.textPrimary : Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textLg
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            IconButton {
                visible: root.stage !== ""
                compact: true
                iconName: "rotate-ccw"
                iconSize: Theme.iconSm
                tooltip: qsTr("Reset %1").arg(root.title)
                enabled: Color.hasGrade
                onClicked: Color.resetStage(root.stage)
            }
            Toggle {
                visible: root.stage !== ""
                checked: root.stageOn
                tooltip: root.stageOn ? qsTr("Turn %1 off").arg(root.title) : qsTr("Turn %1 on").arg(root.title)
                onToggled: Color.setStage(root.stage, checked)
            }
        }
    }

    ColumnLayout {
        id: body
        Layout.fillWidth: true
        Layout.leftMargin: Theme.space(2)
        Layout.rightMargin: Theme.space(1)
        Layout.topMargin: root.expanded ? Theme.space(1) : 0
        Layout.bottomMargin: root.expanded ? Theme.space(3) : 0
        spacing: Theme.space(0.5)
        visible: root.expanded
        opacity: root.stageOn ? 1 : 0.55
        Behavior on opacity { NumberAnimation { duration: Theme.durState } }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: Theme.hairline
        color: Theme.border
    }
}
