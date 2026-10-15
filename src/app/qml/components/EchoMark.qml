pragma ComponentBehavior: Bound
import QtQuick

// Wabcut's echo mark (design language v3), drawn with rectangles in its 120-unit box so it stays crisp at any size and
// follows the theme: five rounded clips stacked like timeline tracks, trimmed short-long-longest-long-short, the
// longest in lime, with two fainter echoes trailing behind (resources/brand/wabcut-mark.svg is the same drawing).
// Below 28 px it switches to the three-clip mark (wabcut-mark-small.svg), whose bars stay a few pixels thick.
// `mono` draws it in one ink (monoColor) for light surfaces and high contrast.
Item {
    id: root

    property real size: 32
    /// "auto" (full from 28 px, small below), "full" or "small".
    property string detail: "auto"
    property bool mono: false
    property color monoColor: Theme.textPrimary
    readonly property bool small: detail === "small" || (detail === "auto" && size < 28)
    readonly property real unit: size / 120

    implicitWidth: size
    implicitHeight: size

    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Wabcut")

    // The clips of one layer: y and width in mark units; `lime` is the longest clip.
    readonly property var bars: small
        ? [{ y: 20, w: 40, lime: false }, { y: 49, w: 90, lime: true }, { y: 78, w: 40, lime: false }]
        : [{ y: 18, w: 28, lime: false }, { y: 36, w: 52, lime: false }, { y: 54, w: 78, lime: true },
           { y: 72, w: 52, lime: false }, { y: 90, w: 28, lime: false }]
    readonly property real barHeight: small ? 22 : 12
    // The echoes then the mark: x in mark units and opacity.
    readonly property var layers: small ? [{ x: 20, opacity: 1 }]
                                        : [{ x: 6, opacity: 0.18 }, { x: 20, opacity: 0.45 }, { x: 34, opacity: 1 }]

    Repeater {
        model: root.layers

        delegate: Item {
            id: echoLayer
            required property var modelData
            anchors.fill: parent
            opacity: modelData.opacity

            Repeater {
                model: root.bars

                delegate: Rectangle {
                    required property var modelData
                    x: echoLayer.modelData.x * root.unit
                    y: modelData.y * root.unit
                    width: modelData.w * root.unit
                    height: root.barHeight * root.unit
                    radius: height / 2
                    color: root.mono ? root.monoColor : modelData.lime ? Theme.brandLime : Theme.brand
                    antialiasing: true
                }
            }
        }
    }
}
