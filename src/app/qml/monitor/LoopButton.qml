import QtQuick

// P.2 (E6): Premiere's Loop button for the Program Monitor (Playback.loop) or, with `source`, the Source Monitor
// (SourceMonitor.loop): playback repeats In to Out, or the whole sequence / clip without marks.
IconButton {
    id: button

    property bool source: false

    objectName: source ? "sourceLoopButton" : "programLoopButton"
    compact: true
    checkable: true
    checked: source ? SourceMonitor.loop : Playback.loop
    iconName: "repeat"
    tooltip: qsTr("Loop")
    command: "monitor.loop"
    onClicked: {
        if (source)
            SourceMonitor.loop = checked
        else
            Playback.loop = checked
        checked = Qt.binding(() => button.source ? SourceMonitor.loop : Playback.loop)
    }
}
