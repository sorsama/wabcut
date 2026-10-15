import QtQuick
import QtQuick.Effects

// An icon (src/app/resources/icons: Boxicons, MIT, under Wabcut's logical names since 0.1.3; see
// tools/icons/import_boxicons.py) tinted with a theme color.
// The SVG is rasterised once at `size` (scaled for the screen's device pixel ratio by Image), kept hidden,
// and drawn through a MultiEffect colorization pass: color changes and theme animations only update a
// shader uniform, they never re-rasterise the SVG.
Item {
    id: root

    property string name: ""
    property int size: Theme.iconMd
    property color color: Theme.textPrimary

    implicitWidth: size
    implicitHeight: size

    Image {
        id: glyph
        anchors.fill: parent
        source: root.name !== "" ? "qrc:/qt/qml/Wabcut/resources/icons/" + root.name + ".svg" : ""
        sourceSize: Qt.size(root.size, root.size)
        fillMode: Image.PreserveAspectFit
        smooth: true
        visible: false
    }

    MultiEffect {
        anchors.fill: glyph
        source: glyph
        visible: glyph.status === Image.Ready
        // The glyphs are black (Boxicons fills, the own drawings' currentColor lines): lift them to white, then
        // colorize to the tint.
        brightness: 1.0
        colorization: 1.0
        colorizationColor: root.color
    }
}
