pragma ComponentBehavior: Bound
import QtQuick

// The gaming layout picker on the Program Monitor (M5o, bridge `Layouts`), filling the program picture's frame area:
//   * while the picker is open, the layout's boxes as a schematic over the picture (the monitor cannot draw the
//     layout's masks, borders and blur until the compositor has them);
//   * the facecam region (the webcam inside the capture) as a dashed outline;
//   * while Layouts.drawingRegion, a drag draws that region (Layouts.setRegionFromFrame, fractions of the frame).
// Nothing otherwise: clicks pass through to the monitor.
Item {
    id: root
    objectName: "layoutMonitorOverlay"

    readonly property var region: Layouts.facecamRegionInFrame
    readonly property bool hasRegion: region.width !== undefined
    /// The rectangle being dragged, in item pixels.
    property rect dragRect: Qt.rect(0, 0, 0, 0)
    property bool dragging: false

    visible: Layouts.previewShown || Layouts.drawingRegion

    LayoutSchematic {
        objectName: "layoutMonitorSchematic"
        anchors.fill: parent
        translucent: true
        visible: Layouts.previewShown && !Layouts.drawingRegion
        // Layouts.frameAspect notifies with the targets (the facecam's shape, the gameplay's fit): read first, so a new
        // target redraws the boxes.
        boxes: {
            const aspect = Layouts.frameAspect
            return Layouts.previewShown ? Layouts.schematic(Layouts.previewOptions, aspect) : []
        }
    }

    // The facecam region.
    Rectangle {
        objectName: "layoutRegionOutline"
        visible: root.hasRegion && !root.dragging
        x: (root.region.x ?? 0) * root.width
        y: (root.region.y ?? 0) * root.height
        width: (root.region.width ?? 0) * root.width
        height: (root.region.height ?? 0) * root.height
        color: Theme.accentWash
        border.width: Theme.selectionWidth
        border.color: Theme.accentStroke
        radius: Theme.radiusBadge
    }
    Rectangle {
        visible: root.dragging
        x: root.dragRect.x
        y: root.dragRect.y
        width: root.dragRect.width
        height: root.dragRect.height
        color: Theme.accentWash
        border.width: Theme.selectionWidth
        border.color: Theme.accentStroke
        radius: Theme.radiusBadge
    }

    // What to do while drawing.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: Theme.space(2)
        visible: Layouts.drawingRegion && !root.dragging
        implicitWidth: hint.implicitWidth + Theme.space(4)
        implicitHeight: hint.implicitHeight + Theme.space(2)
        radius: height / 2
        color: Theme.scrim
        Text {
            id: hint
            anchors.centerIn: parent
            text: qsTr("Drag a box around the webcam in the gameplay")
            color: Theme.scrimText
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    MouseArea {
        id: area
        objectName: "layoutRegionArea"
        anchors.fill: parent
        enabled: Layouts.drawingRegion
        cursorShape: Layouts.drawingRegion ? Qt.CrossCursor : Qt.ArrowCursor
        preventStealing: true
        property point start: Qt.point(0, 0)
        onPressed: mouse => {
            start = Qt.point(mouse.x, mouse.y)
            root.dragRect = Qt.rect(mouse.x, mouse.y, 0, 0)
            root.dragging = true
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const x = Math.max(0, Math.min(root.width, mouse.x))
            const y = Math.max(0, Math.min(root.height, mouse.y))
            root.dragRect = Qt.rect(Math.min(start.x, x), Math.min(start.y, y), Math.abs(x - start.x), Math.abs(y - start.y))
        }
        onReleased: mouse => {
            root.dragging = false
            if (root.width <= 0 || root.height <= 0)
                return
            Layouts.setRegionFromFrame(start.x / root.width, start.y / root.height, mouse.x / root.width, mouse.y / root.height)
            Layouts.drawingRegion = false
            // The facecam's shape changed: check the layout again.
            if (Layouts.previewShown)
                Layouts.preview(Layouts.previewOptions)
        }
    }
}
