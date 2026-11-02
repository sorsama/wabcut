pragma ComponentBehavior: Bound
import QtQuick

// P1 (#17): the Program Monitor's edit overlay (Premiere's): while a Library clip or the Source Monitor's picture is
// dragged over the Program Monitor, five zones say what a drop there does, at the playhead and the targeted tracks:
//
//   +-----------------------------------+
//   |              Overlay              |   the first free video track at or above the target
//   +--------+-----------------+--------+
//   | Insert |    Overwrite    | Append |   Insert pushes later clips; Append puts it after the last clip
//   +--------+-----------------+--------+
//   |              Replace              |   the clip under the playhead on the target track
//   +-----------------------------------+
//
// MonitorPanel's drop area drives it (zoneAt on every move, `zone` the one under the pointer) and runs the edit
// (MonitorPanel.programDrop). Replace is dimmed when there is no clip under the playhead to replace. The zones are
// drawn over the whole picture well, so even a narrow vertical frame has big targets. No input of its own.
Item {
    id: overlay
    objectName: "programEditOverlay"

    /// The zone under the pointer: "insert", "overwrite", "overlay", "replace", "append" or "".
    property string zone: ""
    /// There is a clip under the playhead that Replace would replace.
    property bool replaceAvailable: true

    readonly property real band: Math.max(Theme.space(10), height * 0.24)
    readonly property real side: Math.max(Theme.space(14), width * 0.24)

    /// The zone at (x, y) in this item's coordinates ("" outside it, or on Replace when it is not available).
    function zoneAt(x: real, y: real): string {
        if (x < 0 || y < 0 || x > width || y > height)
            return ""
        if (y < band)
            return "overlay"
        if (y > height - band)
            return overlay.replaceAvailable ? "replace" : ""
        if (x < side)
            return "insert"
        if (x > width - side)
            return "append"
        return "overwrite"
    }

    // A dim veil, so the zones read over any picture.
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusMonitor
        color: Theme.scrim
        opacity: 0.55
    }

    component Zone: Rectangle {
        id: zoneItem
        required property string key
        required property string label
        required property string hint
        property bool available: true
        readonly property bool hot: overlay.zone === zoneItem.key
        objectName: "editOverlayZone_" + zoneItem.key
        radius: Theme.radiusCard
        color: zoneItem.hot ? Theme.accentFill : Qt.alpha(Theme.raised, 0.86)
        border.width: zoneItem.hot ? 0 : Theme.hairline
        border.color: Theme.borderStrong
        opacity: zoneItem.available ? 1 : 0.45
        Accessible.role: Accessible.StaticText
        Accessible.name: zoneItem.label + ". " + zoneItem.hint

        Behavior on color { enabled: !Theme.reduceMotion; ColorAnimation { duration: Theme.durFast } }

        Column {
            anchors.centerIn: parent
            width: parent.width - Theme.space(4)
            spacing: Theme.space(1)

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: zoneItem.label
                color: zoneItem.hot ? Theme.accentOnFill : Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textLg
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                // On a small monitor the zones show their names only.
                visible: zoneItem.height >= Theme.space(16) && zoneItem.width >= Theme.space(30)
                text: zoneItem.hint
                color: zoneItem.hot ? Theme.accentOnFill : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
        }
    }

    Item {
        anchors.fill: parent
        anchors.margins: Theme.space(2)
        readonly property real gap: Theme.space(1.5)

        Zone {
            key: "overlay"
            label: qsTr("Overlay")
            hint: qsTr("On the first free video track above the target")
            x: 0
            y: 0
            width: parent.width
            height: overlay.band - Theme.space(2) - parent.gap / 2
        }
        Zone {
            key: "insert"
            label: qsTr("Insert")
            hint: qsTr("At the playhead; later clips move right")
            x: 0
            y: overlay.band - Theme.space(2) + parent.gap / 2
            width: overlay.side - Theme.space(2) - parent.gap / 2
            height: parent.height - 2 * (overlay.band - Theme.space(2)) - parent.gap
        }
        Zone {
            key: "overwrite"
            label: qsTr("Overwrite")
            hint: qsTr("At the playhead, over what is there")
            x: overlay.side - Theme.space(2) + parent.gap / 2
            y: overlay.band - Theme.space(2) + parent.gap / 2
            width: parent.width - 2 * (overlay.side - Theme.space(2)) - parent.gap
            height: parent.height - 2 * (overlay.band - Theme.space(2)) - parent.gap
        }
        Zone {
            key: "append"
            label: qsTr("Append at End")
            hint: qsTr("After the last clip")
            x: parent.width - width
            y: overlay.band - Theme.space(2) + parent.gap / 2
            width: overlay.side - Theme.space(2) - parent.gap / 2
            height: parent.height - 2 * (overlay.band - Theme.space(2)) - parent.gap
        }
        Zone {
            key: "replace"
            label: qsTr("Replace")
            hint: overlay.replaceAvailable ? qsTr("The clip under the playhead on the target track")
                                           : qsTr("No clip under the playhead to replace")
            available: overlay.replaceAvailable
            x: 0
            y: parent.height - height
            width: parent.width
            height: overlay.band - Theme.space(2) - parent.gap / 2
        }
    }
}
