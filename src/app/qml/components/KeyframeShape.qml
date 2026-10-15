import QtQuick
import QtQuick.Shapes

// A keyframe in the Effect Controls lane. Its shape names the interpolation, Premiere's visual language: diamond =
// linear, square = hold, hourglass = Bezier, circle = ease in and out, half round / half diamond = ease in (round on
// the arriving side) or ease out (round on the leaving side). Selected keys fill with the accent and grow a little.
Item {
    id: root

    property string shape: "linear"
    property bool selected: false
    property real size: Theme.space(3)

    readonly property real c: size / 2
    readonly property real r: c - 1
    readonly property string path: {
        const c = root.c, r = root.r
        const p = (x, y) => x.toFixed(2) + " " + y.toFixed(2)
        switch (root.shape) {
        case "hold": {
            const s = r * 0.8
            return "M " + p(c - s, c - s) + " L " + p(c + s, c - s) + " L " + p(c + s, c + s) + " L " + p(c - s, c + s) + " Z"
        }
        case "bezier":
            return "M " + p(c - r, c - r) + " L " + p(c + r, c - r) + " L " + p(c, c) + " Z M " + p(c - r, c + r) + " L "
                 + p(c + r, c + r) + " L " + p(c, c) + " Z"
        case "easeInOut":
            return "M " + p(c - r, c) + " A " + p(r, r) + " 0 1 0 " + p(c + r, c) + " A " + p(r, r) + " 0 1 0 " + p(c - r, c) + " Z"
        case "easeIn":
            return "M " + p(c, c - r) + " A " + p(r, r) + " 0 0 0 " + p(c, c + r) + " L " + p(c + r, c) + " Z"
        case "easeOut":
            return "M " + p(c, c - r) + " A " + p(r, r) + " 0 0 1 " + p(c, c + r) + " L " + p(c - r, c) + " Z"
        default:
            return "M " + p(c, c - r) + " L " + p(c + r, c) + " L " + p(c, c + r) + " L " + p(c - r, c) + " Z"
        }
    }

    width: size
    height: size
    scale: selected ? Theme.grabScale : 1
    Behavior on scale { NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic } }

    Shape {
        anchors.fill: parent
        antialiasing: true

        ShapePath {
            fillColor: root.selected ? Theme.accentStroke : Theme.textSecondary
            strokeColor: root.selected ? Theme.accentSoft : Theme.sunken
            strokeWidth: 1
            PathSvg { path: root.path }
        }
    }
}
