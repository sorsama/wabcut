pragma ComponentBehavior: Bound
import QtQuick

// The right-click menu of an audio track header (A.9, M5n): the track's channel format, Premiere's track type (mono,
// stereo, 5.1, adaptive: track.setFormat, one undo step). Clips are mixed to it (mono averages, 5.1 to stereo is
// ITU-R BS.775), and it to the master's format; adaptive passes channels through one to one. Its own file, importing
// only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "trackHeaderMenu"

    property string trackId: ""
    property string shortName: ""
    readonly property string format: AudioChannels.trackFormats[trackId] ?? "stereo"
    /// Why the last choice was refused ("" after a good one).
    property string error: ""

    function openFor(target, track, name, point) {
        trackId = track
        shortName = name
        error = ""
        popup(target, point.x, point.y)
    }
    function choose(format) {
        menu.error = AudioChannels.setTrackFormat(menu.trackId, format)
    }

    // Group label.
    Item {
        implicitWidth: groupLabel.implicitWidth + Theme.space(6)
        implicitHeight: groupLabel.implicitHeight + Theme.space(2)
        Text {
            id: groupLabel
            x: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%1 channel format").arg(menu.shortName)
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
    }
    // The formats: checked while the track has it (re-bound after a click, which would break the binding).
    ContextMenuItem {
        id: format0
        objectName: "trackFormat_mono"
        text: qsTr("Mono")
        checkable: true
        checked: menu.format === "mono"
        onTriggered: {
            format0.checked = Qt.binding(() => menu.format === "mono")
            menu.choose("mono")
        }
    }
    ContextMenuItem {
        id: format1
        objectName: "trackFormat_stereo"
        text: qsTr("Stereo")
        checkable: true
        checked: menu.format === "stereo"
        onTriggered: {
            format1.checked = Qt.binding(() => menu.format === "stereo")
            menu.choose("stereo")
        }
    }
    ContextMenuItem {
        id: format2
        objectName: "trackFormat_5.1"
        text: qsTr("5.1")
        checkable: true
        checked: menu.format === "5.1"
        onTriggered: {
            format2.checked = Qt.binding(() => menu.format === "5.1")
            menu.choose("5.1")
        }
    }
    ContextMenuItem {
        id: format3
        objectName: "trackFormat_adaptive"
        text: qsTr("Adaptive")
        checkable: true
        checked: menu.format === "adaptive"
        onTriggered: {
            format3.checked = Qt.binding(() => menu.format === "adaptive")
            menu.choose("adaptive")
        }
    }
}
