pragma ComponentBehavior: Bound
import QtQuick

// Review marks on the Program Monitor (M5m, bridge `Review`), filling the program picture's frame area. It draws the
// mark of the selected note (or of the note at the playhead): a rectangle, or a ring for a spot, in the accent colour
// (green once resolved). While Review.drawing, a click marks a spot and a drag a rectangle for the next note
// (Review.pendingMark, in fractions of the frame: independent of the monitor's zoom and aspect).
Item {
    id: root
    objectName: "reviewMarkOverlay"

    readonly property var mark: Review.shownMark
    readonly property bool hasMark: mark.x !== undefined
    readonly property bool isPoint: hasMark && (mark.width ?? 0) <= 0 && (mark.height ?? 0) <= 0
    readonly property color markColor: mark.resolved === true ? Theme.success : Theme.accentStroke
    /// The rectangle being dragged, in item pixels (while drawing).
    property rect dragRect: Qt.rect(0, 0, 0, 0)
    property bool dragging: false

    function fraction(v, total) {
        return total > 0 ? Math.max(0, Math.min(1, v / total)) : 0
    }

    // The mark.
    Rectangle {
        objectName: "reviewMarkRect"
        visible: root.hasMark && !root.isPoint && !root.dragging
        x: (root.mark.x ?? 0) * root.width
        y: (root.mark.y ?? 0) * root.height
        width: (root.mark.width ?? 0) * root.width
        height: (root.mark.height ?? 0) * root.height
        color: Qt.alpha(root.markColor, 0.12)
        border.width: Theme.selectionWidth
        border.color: root.markColor
        radius: Theme.radiusBadge
    }
    Rectangle {
        objectName: "reviewMarkPoint"
        visible: root.hasMark && root.isPoint && !root.dragging
        readonly property real size: Theme.space(6)
        x: (root.mark.x ?? 0) * root.width - size / 2
        y: (root.mark.y ?? 0) * root.height - size / 2
        width: size
        height: size
        radius: size / 2
        color: "transparent"
        border.width: Theme.selectionWidth + 1
        border.color: root.markColor
        Rectangle {
            anchors.centerIn: parent
            width: Theme.space(1)
            height: width
            radius: width / 2
            color: root.markColor
        }
    }
    // The rectangle being dragged.
    Rectangle {
        visible: root.dragging
        x: root.dragRect.x
        y: root.dragRect.y
        width: root.dragRect.width
        height: root.dragRect.height
        color: Qt.alpha(Theme.accentStroke, 0.12)
        border.width: Theme.selectionWidth
        border.color: Theme.accentStroke
        radius: Theme.radiusBadge
    }

    MouseArea {
        id: area
        objectName: "reviewMarkArea"
        anchors.fill: parent
        enabled: Review.drawing
        cursorShape: Review.drawing ? Qt.CrossCursor : Qt.ArrowCursor
        property point start: Qt.point(0, 0)
        onPressed: mouse => {
            start = Qt.point(mouse.x, mouse.y)
            root.dragRect = Qt.rect(mouse.x, mouse.y, 0, 0)
            root.dragging = true
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const x0 = Math.max(0, Math.min(start.x, mouse.x))
            const y0 = Math.max(0, Math.min(start.y, mouse.y))
            const x1 = Math.min(root.width, Math.max(start.x, mouse.x))
            const y1 = Math.min(root.height, Math.max(start.y, mouse.y))
            root.dragRect = Qt.rect(x0, y0, x1 - x0, y1 - y0)
        }
        onReleased: mouse => {
            root.dragging = false
            const r = root.dragRect
            // A short drag is a click: a spot.
            if (r.width < Theme.space(2) && r.height < Theme.space(2))
                Review.setPendingMark(root.fraction(mouse.x, root.width), root.fraction(mouse.y, root.height), 0, 0)
            else
                Review.setPendingMark(root.fraction(r.x, root.width), root.fraction(r.y, root.height),
                                      root.fraction(r.width, root.width), root.fraction(r.height, root.height))
        }
        onCanceled: root.dragging = false
    }
}
