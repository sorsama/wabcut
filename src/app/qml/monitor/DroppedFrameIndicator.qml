import QtQuick

// P.2 (E6): Premiere's Show Dropped Frame Indicator (the Program Monitor's settings; bridges Monitors and Playback): a
// light in the corner of the picture, green while playback keeps up, amber once frames were dropped (counted since
// play started; the tooltip says how many).
Rectangle {
    id: light
    objectName: "droppedFrameIndicator"

    readonly property bool dropped: Playback.droppedFrames > 0

    width: Theme.space(3)
    height: width
    radius: width / 2
    color: dropped ? Theme.warning : Theme.success
    border.width: Theme.hairline
    border.color: Theme.monitorSurroundBorder
    Accessible.role: Accessible.Indicator
    Accessible.name: light.dropped ? qsTr("%n frame(s) dropped", "", Playback.droppedFrames) : qsTr("No frames dropped")

    HoverHandler {
        id: hover
    }
    Tooltip {
        visible: hover.hovered
        text: light.Accessible.name
    }
}
