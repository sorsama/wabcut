import QtQuick

// P.2 (E6): Premiere's Overlays in the Program Monitor (the settings menu; bridge Monitors): the source timecode of the
// topmost clip at the playhead on the left, the sequence timecode on the right, over the bottom of the picture. Not in
// exports.
Item {
    id: root
    objectName: "programOverlays"

    readonly property var overlay: Monitors.overlayText

    component OverlayText: Text {
        color: Theme.scrimText
        style: Text.Outline
        styleColor: Theme.scrim
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
    }

    OverlayText {
        objectName: "overlaySource"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space(2)
        visible: text !== ""
        text: (root.overlay.source ?? "") !== "" ? (root.overlay.clip ?? "") + "  " + root.overlay.source : ""
    }
    OverlayText {
        objectName: "overlaySequence"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space(2)
        text: root.overlay.sequence ?? ""
    }
}
