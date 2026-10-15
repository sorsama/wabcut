import QtQuick
import QtQuick.Shapes

// One flared foot of a browser-style tab (design language v3): the concave corner where the current tab's surface runs
// out into the strip beside it. Only the surface is drawn - the strip shows through the curve - so whatever lies
// under it (the strip, or a hovered neighbour's tab) stays visible. Place it beside the tab's foot, `size` square.
Shape {
    id: root

    /// -1: left of the tab, 1: right of it.
    property int side: -1
    property real size: Theme.radiusTab
    property color color: Theme.panel

    width: size
    height: size
    preferredRendererType: Shape.CurveRenderer

    ShapePath {
        strokeColor: "transparent"
        fillColor: root.color
        // The square's outer edge to its foot, then the quarter circle back up to the tab's side.
        startX: root.side < 0 ? root.size : 0
        startY: 0
        PathLine {
            x: root.side < 0 ? root.size : 0
            y: root.size
        }
        PathLine {
            x: root.side < 0 ? 0 : root.size
            y: root.size
        }
        PathArc {
            x: root.side < 0 ? root.size : 0
            y: 0
            radiusX: root.size
            radiusY: root.size
            direction: root.side < 0 ? PathArc.Counterclockwise : PathArc.Clockwise
        }
    }
}
