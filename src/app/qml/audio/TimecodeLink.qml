pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// 0.6 (P1 round, #54): a time shown as a link - where the loudness analysis found the loudest moments and the true-peak
// overs. A click (or Space / Enter after Tab) moves the playhead there: ProjectController.seekToText reads the time back in
// the sequence's own display format, which is how the bridge wrote it. Hidden while `timecode` is empty.
AbstractButton {
    id: control

    /// The time as the sequence displays it ("00:01:02:03").
    property string timecode: ""
    /// What the link says instead of the time ("3": a count that goes to the first one), when it is not empty.
    property string label: ""
    /// What the time is, for a screen reader and the tooltip ("Loudest short-term moment").
    property string what: ""
    property color linkColor: Theme.accentStroke
    property int pixelSize: Theme.textXs

    visible: control.timecode !== ""
    text: control.label !== "" ? control.label : control.timecode
    implicitWidth: textItem.implicitWidth + Theme.space(2)
    implicitHeight: textItem.implicitHeight + Theme.space(1)
    padding: 0
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    Accessible.role: Accessible.Link
    Accessible.name: control.what !== "" ? qsTr("%1: go to %2").arg(control.what).arg(control.timecode)
                                         : qsTr("Go to %1").arg(control.timecode)
    onClicked: ProjectController.seekToText(control.timecode)

    background: Rectangle {
        radius: Theme.radiusBadge
        color: control.down ? Theme.well : control.hovered ? Theme.hover : "transparent"
        FocusRing {
            targetRadius: Theme.radiusBadge
            visible: control.visualFocus
        }
    }
    contentItem: Text {
        id: textItem
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: control.text
        color: control.linkColor
        font.family: Theme.fontMono
        font.pixelSize: control.pixelSize
        font.underline: control.hovered || control.visualFocus
    }
    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }
    Loader {
        active: control.what !== "" && control.hovered
        sourceComponent: Tooltip {
            parent: control
            visible: true
            text: qsTr("%1: go to %2").arg(control.what).arg(control.timecode)
        }
    }
}
