pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The strip a collapsed track group leaves in the Audio Track Mixer (0.7, card 124): one narrow strip in place of the
// group's channel strips (AudioMixer.strips: the first track's entry carries `group`, ProjectController.trackGroupInfo).
// It shows the group's name and label colour, how many tracks it holds, Mute and Solo for all of them (lit while every
// track has them, a ring while only some do), where the group goes (the Mix, a submix, or several places) and a button
// that expands the group again into its own strips. The collapsed state is the timeline's: collapsing a group in either
// place folds it in both. Every change is one undo step.
Rectangle {
    id: root

    /// The group (ProjectController.trackGroupInfo): { id, name, colorIndex, collapsed, count, shortNames, muted, solo
    /// (0 none, 1 some, 2 all of its tracks), output ("mix" | "submix" | "mixed"), submixName }.
    required property var group

    objectName: "mixerGroupStrip_" + root.group.id
    implicitWidth: Theme.mixerStripWidth
    radius: Theme.radiusField
    color: Theme.raised
    border.width: Theme.hairline
    border.color: root.group.colorIndex >= 0 ? Theme.clipColor(root.group.colorIndex) : Theme.border
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Track group %1, %2, collapsed").arg(root.group.name).arg(qsTr("%n track(s)", "", root.group.count))

    component GroupToggle: AbstractButton {
        id: toggle
        property color onColor: Theme.accentFill
        property string tip: ""
        property int coverage: 0 // how many of the group's tracks have it: 0 none, 1 some, 2 all
        Layout.fillWidth: true
        implicitHeight: Theme.controlHeightSmall - Theme.space(2)
        checkable: true
        checked: toggle.coverage === 2
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: tip
        background: Rectangle {
            radius: Theme.radiusBadge
            color: toggle.checked ? toggle.onColor : toggle.hovered ? Theme.hover : Theme.sunken
            border.width: Theme.hairline
            border.color: toggle.checked || toggle.coverage === 1 ? toggle.onColor : Theme.border
            opacity: toggle.enabled ? 1 : Theme.disabledOpacity
        }
        contentItem: Text {
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: toggle.text
            color: toggle.checked ? Theme.clipInk : toggle.coverage === 1 ? toggle.onColor : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.Bold
        }
        FocusRing {
            targetRadius: Theme.radiusBadge
            visible: toggle.visualFocus
        }
        HoverHandler {
            enabled: toggle.enabled
            cursorShape: Qt.PointingHandCursor
        }
        Loader {
            active: toggle.hovered && toggle.tip !== ""
            sourceComponent: Tooltip {
                parent: toggle
                visible: true
                text: toggle.tip
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.space(1) + Theme.hairline
        spacing: Theme.space(1)

        // The group's label colour as a bar, then the name.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.space(0.75)
            radius: height / 2
            color: root.group.colorIndex >= 0 ? Theme.clipColor(root.group.colorIndex) : Theme.well
        }
        Text {
            objectName: "mixerGroupName"
            Layout.fillWidth: true
            text: root.group.name
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.Bold
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("%n track(s)", "", root.group.count)
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
        // The tracks it holds (their short names).
        Text {
            Layout.fillWidth: true
            text: (root.group.shortNames ?? []).join(" ")
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            color: Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
        // Where the group goes.
        Text {
            objectName: "mixerGroupOutput"
            Layout.fillWidth: true
            text: root.group.output === "submix" ? qsTr("To %1").arg(root.group.submixName !== "" ? root.group.submixName : qsTr("a submix"))
                  : root.group.output === "mixed" ? qsTr("To several outputs")
                  : qsTr("To the Mix")
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
        Item {
            Layout.fillHeight: true
        }
        GroupToggle {
            objectName: "mixerGroupMute_" + root.group.id
            text: qsTr("M")
            coverage: root.group.muted
            onColor: Theme.warning
            tip: root.group.muted === 2 ? qsTr("Unmute every track in %1").arg(root.group.name)
                                        : qsTr("Mute every track in %1").arg(root.group.name)
            onClicked: ProjectController.setTrackGroupFlag(root.group.id, "muted", root.group.muted !== 2)
        }
        GroupToggle {
            objectName: "mixerGroupSolo_" + root.group.id
            text: qsTr("S")
            coverage: root.group.solo
            onColor: Theme.success
            tip: root.group.solo === 2 ? qsTr("Unsolo every track in %1").arg(root.group.name)
                                       : qsTr("Solo every track in %1").arg(root.group.name)
            onClicked: ProjectController.setTrackGroupFlag(root.group.id, "solo", root.group.solo !== 2)
        }
        AbstractButton {
            id: expand
            objectName: "mixerGroupExpand_" + root.group.id
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeightSmall - Theme.space(1)
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.name: qsTr("Expand group %1").arg(root.group.name)
            onClicked: ProjectController.setTrackGroupCollapsed(root.group.id, false)
            background: Rectangle {
                radius: Theme.radiusPill
                color: expand.hovered ? Theme.hover : Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border
            }
            contentItem: Text {
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Expand")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
            }
            FocusRing {
                targetRadius: Theme.radiusPill
                visible: expand.visualFocus
            }
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
        }
    }
}
