pragma ComponentBehavior: Bound
import QtQuick

// Resize handles and hairline outline for the frameless main window. Each edge/corner hands the drag to the
// window manager (startSystemResize) so native snapping and resize animations apply. Hidden while maximized.
Item {
    id: root

    property int margin: Theme.windowResizeMargin

    anchors.fill: parent

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }

    component ResizeHandle: MouseArea {
        required property int edges

        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: edges === (Qt.LeftEdge | Qt.TopEdge) || edges === (Qt.RightEdge | Qt.BottomEdge) ? Qt.SizeFDiagCursor
                   : edges === (Qt.RightEdge | Qt.TopEdge) || edges === (Qt.LeftEdge | Qt.BottomEdge) ? Qt.SizeBDiagCursor
                   : edges === Qt.LeftEdge || edges === Qt.RightEdge ? Qt.SizeHorCursor
                   : Qt.SizeVerCursor
        onPressed: Window.window?.startSystemResize(edges)
    }

    // Edges
    ResizeHandle { edges: Qt.LeftEdge; x: 0; y: root.margin; width: root.margin; height: root.height - 2 * root.margin }
    ResizeHandle { edges: Qt.RightEdge; x: root.width - root.margin; y: root.margin; width: root.margin; height: root.height - 2 * root.margin }
    ResizeHandle { edges: Qt.TopEdge; x: root.margin; y: 0; width: root.width - 2 * root.margin; height: root.margin }
    ResizeHandle { edges: Qt.BottomEdge; x: root.margin; y: root.height - root.margin; width: root.width - 2 * root.margin; height: root.margin }
    // Corners (twice the margin, easier to hit)
    ResizeHandle { edges: Qt.LeftEdge | Qt.TopEdge; x: 0; y: 0; width: 2 * root.margin; height: 2 * root.margin }
    ResizeHandle { edges: Qt.RightEdge | Qt.TopEdge; x: root.width - 2 * root.margin; y: 0; width: 2 * root.margin; height: 2 * root.margin }
    ResizeHandle { edges: Qt.LeftEdge | Qt.BottomEdge; x: 0; y: root.height - 2 * root.margin; width: 2 * root.margin; height: 2 * root.margin }
    ResizeHandle { edges: Qt.RightEdge | Qt.BottomEdge; x: root.width - 2 * root.margin; y: root.height - 2 * root.margin; width: 2 * root.margin; height: 2 * root.margin }
}
