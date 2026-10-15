pragma ComponentBehavior: Bound
import QtQuick

// A gaming layout's boxes (Layouts.schematic: fractions of the output frame, bottom to top) drawn into this item: the
// blurred background as a dim wash, the gameplay as a neutral box, the facecam in the accent with its corner radius
// (or a circle), border and shadow. The layout picker's cards draw it on a black frame; the Program Monitor's overlay
// draws it `translucent` over the picture, since the monitor cannot draw the layout's effects yet.
Item {
    id: root

    property var boxes: []
    /// Over the monitor's picture: no frame, outlines only, light fills.
    property bool translucent: false
    /// "Gameplay" / "Facecam" inside the boxes when they are large enough (over the monitor's picture, the facecam's
    /// only: the gameplay is the picture itself).
    property bool labels: true

    clip: true

    Rectangle {
        anchors.fill: parent
        visible: !root.translucent
        color: Theme.videoBlack
    }

    Repeater {
        model: root.boxes
        delegate: Item {
            id: cell
            required property var modelData
            readonly property string role: modelData.role ?? ""
            readonly property bool facecam: role === "facecam"
            readonly property bool background: role === "background"
            readonly property real radiusPx: modelData.circle ? Math.min(width, height) / 2
                                                              : (modelData.radius ?? 0) * Math.min(width, height)
            objectName: "layoutBox_" + role
            x: (modelData.x ?? 0) * root.width
            y: (modelData.y ?? 0) * root.height
            width: (modelData.width ?? 0) * root.width
            height: (modelData.height ?? 0) * root.height

            // The facecam's drop shadow.
            Rectangle {
                visible: cell.facecam && cell.modelData.shadow === true
                x: Theme.space(0.5)
                y: Theme.space(1)
                width: parent.width
                height: parent.height
                radius: cell.radiusPx
                color: Qt.alpha(Theme.videoBlack, 0.45)
            }
            Rectangle {
                anchors.fill: parent
                radius: cell.radiusPx
                color: cell.background ? Qt.alpha(Theme.textTertiary, root.translucent ? 0.10 : 0.18)
                     : cell.facecam ? Qt.alpha(Theme.accentStroke, root.translucent ? 0.22 : 0.35)
                     : Qt.alpha(Theme.textTertiary, root.translucent ? 0.12 : 0.30)
                border.width: cell.facecam && cell.modelData.border === true ? Math.max(Theme.hairline, Theme.selectionWidth)
                            : root.translucent ? Theme.hairline : 0
                border.color: cell.facecam && cell.modelData.border === true ? (cell.modelData.borderColor ?? Theme.contentWhite)
                            : cell.facecam ? Theme.accentStroke : Qt.alpha(Theme.scrimText, 0.5)
            }
            Text {
                anchors.centerIn: parent
                visible: root.labels && (!root.translucent || cell.facecam) && parent.width > Theme.space(14)
                         && parent.height > Theme.space(5)
                text: cell.background ? qsTr("Blurred") : cell.facecam ? qsTr("Facecam") : qsTr("Gameplay")
                color: Theme.scrimText
                opacity: 0.85
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
    }
}
