pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// A caption track's header in the Tracks panel (0.5; a TimelineView.captionRows delegate): Premiere's caption tracks at
// the top of the Timeline. The name chip ("C1") targets the row for Add Edit (Ctrl+K splits the cue under the playhead
// there; view state, like source patching), the name says the track and its language ("English (en)"), a click on the
// header shows the track in the Captions page, and the eye shows or hides it (captions.setTrack, one undo step; a
// hidden track is drawn dimmed). The row's height is the lanes' caption row height.
Item {
    id: root

    required property var modelData
    /// The lanes (TimelinePanel sets it): targeting lives there.
    property TimelineView view: null

    readonly property string captionTrack: root.modelData.id
    readonly property string label: root.modelData.label
    readonly property bool trackEnabled: root.modelData.enabled
    readonly property bool targeted: root.modelData.targeted
    readonly property string title: root.modelData.language !== ""
                                    ? qsTr("%1 (%2)").arg(root.modelData.name).arg(root.modelData.language)
                                    : root.modelData.name

    objectName: "captionHeader_" + root.label
    height: root.modelData.height
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Caption track %1, %2").arg(root.label).arg(root.title)

    // Design language v3: a pill on the raised surface, like the video and audio track headers.
    Rectangle {
        id: plate
        anchors.fill: parent
        anchors.topMargin: Theme.hairline
        anchors.bottomMargin: Theme.hairline
        radius: height / 2
        color: Theme.raised
    }
    // A click on the header (not its chip or eye) shows the track in the Captions page.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: eventPoint => {
            const hit = root.childAt(eventPoint.position.x, eventPoint.position.y)
            if (hit === null || hit === plate || hit === nameLabel)
                Captions.track = root.captionTrack
        }
    }

    AbstractButton {
        id: chip
        objectName: "captionTarget_" + root.label
        anchors.left: parent.left
        anchors.leftMargin: Theme.space(1) + Theme.sourcePatchWidth + Theme.space(1.5)
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.space(9) + (root.targeted ? Theme.space(2) : 0)
        height: Math.min(Theme.space(5), root.height - Theme.space(1))
        checkable: true
        checked: root.targeted
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.role: Accessible.CheckBox
        Accessible.name: qsTr("Target %1 (%2)").arg(root.label).arg(root.title)
        Accessible.checked: root.targeted
        onClicked: {
            checked = Qt.binding(() => root.targeted)
            if (root.view)
                root.view.setCaptionTargeted(root.captionTrack, !root.targeted)
        }

        background: Rectangle {
            radius: height / 2
            color: root.targeted ? Theme.accentFill : chip.hovered ? Theme.hover : Theme.well
            opacity: root.trackEnabled ? 1 : Theme.disabledOpacity

            Rectangle {
                visible: root.targeted
                anchors.fill: parent
                anchors.margins: -Theme.space(0.75)
                radius: height / 2
                color: "transparent"
                border.width: Theme.selectionWidth
                border.color: Theme.textPrimary
            }
        }
        contentItem: Text {
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: root.targeted ? "▸" + root.label : root.label
            color: root.targeted ? Theme.accentOnFill : Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
            font.weight: Font.Bold
        }

        Tooltip {
            visible: chip.hovered
            text: root.targeted ? qsTr("%1 is targeted: Add Edit splits its caption under the playhead").arg(root.label)
                                : qsTr("Target %1 for Add Edit (Ctrl+K splits its caption under the playhead)").arg(root.label)
        }
        FocusRing {
            anchors.fill: parent
            targetRadius: height / 2
            visible: chip.visualFocus
        }
        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
    }

    Text {
        id: nameLabel
        objectName: "captionName_" + root.label
        anchors.left: chip.right
        anchors.leftMargin: Theme.space(1.5)
        anchors.right: eye.left
        anchors.rightMargin: Theme.space(1)
        anchors.verticalCenter: parent.verticalCenter
        text: root.title
        elide: Text.ElideRight
        color: root.trackEnabled ? Theme.textSecondary : Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
    }

    IconButton {
        id: eye
        objectName: "captionOutput_" + root.label
        anchors.right: parent.right
        anchors.rightMargin: Theme.space(1)
        anchors.verticalCenter: parent.verticalCenter
        compact: true
        iconName: root.trackEnabled ? "eye" : "eye-off"
        iconSize: Theme.iconSm
        tooltip: root.trackEnabled ? qsTr("Hide captions %1").arg(root.label) : qsTr("Show captions %1").arg(root.label)
        Accessible.role: Accessible.CheckBox
        Accessible.checked: root.trackEnabled
        onClicked: Captions.setTrackEnabledFor(root.captionTrack, !root.trackEnabled)
    }
}
