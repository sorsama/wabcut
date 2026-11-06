pragma ComponentBehavior: Bound
import QtQuick

// The subject box of Track Motion on the program monitor (0.7, card #83; docs/masks.md "Track Motion"): a sibling of the
// monitor slot that fills the canvas (fractions of the canvas are fractions of this item), like MaskOverlay. While the
// section's "Draw box" is armed a drag draws the box (Esc cancels); a box can be moved by dragging inside it and resized
// by its four corner handles. The box is where the subject is at the playhead's frame; everything else passes through to
// the monitor.
Item {
    id: root
    objectName: "programTrackOverlay"

    readonly property var corners: TrackMotion.box
    readonly property bool hasBox: corners.length === 4
    readonly property bool active: TrackMotion.panelOpen && TrackMotion.available && (hasBox || TrackMotion.boxTool)
    visible: active

    /// The box in this item's pixels.
    readonly property real boxX: hasBox ? corners[0] * width : 0
    readonly property real boxY: hasBox ? corners[1] * height : 0
    readonly property real boxW: hasBox ? (corners[2] - corners[0]) * width : 0
    readonly property real boxH: hasBox ? (corners[3] - corners[1]) * height : 0

    // Drawing: a drag while the tool is armed.
    MouseArea {
        id: draw
        objectName: "trackDrawArea"
        anchors.fill: parent
        enabled: TrackMotion.boxTool
        visible: enabled
        cursorShape: Qt.CrossCursor
        property real x0: 0
        property real y0: 0
        property real x1: 0
        property real y1: 0
        property bool dragging: false

        onPressed: mouse => {
            root.forceActiveFocus()
            x0 = x1 = mouse.x
            y0 = y1 = mouse.y
            dragging = true
        }
        onPositionChanged: mouse => {
            if (pressed) {
                x1 = mouse.x
                y1 = mouse.y
            }
        }
        onReleased: mouse => {
            if (!dragging)
                return
            dragging = false
            // A click without a drag makes no box (the tool stays armed).
            if (Math.abs(mouse.x - x0) > 3 && Math.abs(mouse.y - y0) > 3)
                TrackMotion.setBox(x0 / width, y0 / height, mouse.x / width, mouse.y / height)
        }
        onCanceled: dragging = false
    }
    // The box being drawn.
    Rectangle {
        visible: draw.dragging
        x: Math.min(draw.x0, draw.x1)
        y: Math.min(draw.y0, draw.y1)
        width: Math.abs(draw.x1 - draw.x0)
        height: Math.abs(draw.y1 - draw.y0)
        color: Qt.alpha(Theme.accentStroke, 0.12)
        border.width: 2
        border.color: Theme.accentStroke
    }

    // The box: move it by its body, resize it by its corners.
    Rectangle {
        id: frame
        objectName: "trackBox"
        visible: root.hasBox
        x: root.boxX
        y: root.boxY
        width: root.boxW
        height: root.boxH
        color: Qt.alpha(Theme.accentStroke, 0.10)
        border.width: 2
        border.color: Theme.accentStroke
        Accessible.role: Accessible.Graphic
        Accessible.name: qsTr("Subject box to track")

        // A crosshair at its centre: the point the follower keeps.
        Rectangle {
            anchors.centerIn: parent
            width: 11
            height: 1
            color: Theme.accentStroke
        }
        Rectangle {
            anchors.centerIn: parent
            width: 1
            height: 11
            color: Theme.accentStroke
        }

        MouseArea {
            id: body
            objectName: "trackBoxBody"
            anchors.fill: parent
            enabled: !TrackMotion.boxTool && !TrackMotion.tracking
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            property point start
            property var origin: []
            onPressed: mouse => {
                start = mapToItem(root, mouse.x, mouse.y)
                origin = root.corners.slice()
            }
            onPositionChanged: mouse => {
                if (!pressed || origin.length !== 4)
                    return
                const p = mapToItem(root, mouse.x, mouse.y)
                let dx = (p.x - start.x) / root.width
                let dy = (p.y - start.y) / root.height
                // Kept inside the canvas whole (the box keeps its size).
                dx = Math.min(1 - origin[2], Math.max(-origin[0], dx))
                dy = Math.min(1 - origin[3], Math.max(-origin[1], dy))
                TrackMotion.setBox(origin[0] + dx, origin[1] + dy, origin[2] + dx, origin[3] + dy)
            }
        }
    }

    Repeater {
        model: root.hasBox && !TrackMotion.tracking ? 4 : 0
        delegate: Rectangle {
            id: handle
            required property int index
            // 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left.
            readonly property bool atRight: index === 1 || index === 2
            readonly property bool atBottom: index === 2 || index === 3
            objectName: "trackBoxHandle" + index
            x: (atRight ? root.boxX + root.boxW : root.boxX) - width / 2
            y: (atBottom ? root.boxY + root.boxH : root.boxY) - height / 2
            width: 11
            height: 11
            radius: 2
            color: handleMouse.pressed ? Theme.accentFill : Theme.scrimText
            border.width: 1.5
            border.color: Theme.accentStroke
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Resize the subject box: corner %1").arg(index + 1)

            MouseArea {
                id: handleMouse
                anchors.fill: parent
                anchors.margins: -5
                enabled: !TrackMotion.boxTool
                preventStealing: true
                cursorShape: handle.atRight === handle.atBottom ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
                onPositionChanged: mouse => {
                    if (!pressed || root.corners.length !== 4)
                        return
                    const p = mapToItem(root, mouse.x, mouse.y)
                    const x = Math.min(1, Math.max(0, p.x / root.width))
                    const y = Math.min(1, Math.max(0, p.y / root.height))
                    // The opposite corner stays where it is.
                    const fixedX = handle.atRight ? root.corners[0] : root.corners[2]
                    const fixedY = handle.atBottom ? root.corners[1] : root.corners[3]
                    TrackMotion.setBox(fixedX, fixedY, x, y)
                }
            }
        }
    }

    Keys.onEscapePressed: TrackMotion.boxTool = false
}
