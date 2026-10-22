import QtQuick

// P.2 (E6): Premiere's Transparency Grid (each monitor's settings menu; bridge Monitors): a white and grey checkerboard
// under a monitor's picture, seen where the picture is transparent (the monitor then keeps the frame's alpha).
Image {
    fillMode: Image.Tile
    source: "qrc:/qt/qml/Wabcut/resources/monitor/transparency-grid.svg"
    sourceSize: Qt.size(16, 16)
    smooth: false
}
