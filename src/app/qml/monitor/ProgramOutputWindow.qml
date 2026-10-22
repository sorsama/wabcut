import QtQuick
import QtQuick.Window

// P.2 (E6): the second program output (bridge Monitors; docs/monitors.md): a top-level window that shows the program
// through a playback mirror (Playback.attachMirror: the same transport and edits, decoded on its own in CPU frames, so
// the main monitor keeps the decoders' GPU device). Toggle Full Screen (Ctrl+`) puts it full screen on the main
// window's display; Enable Transmit keeps it full screen on the chosen display (Premiere's Mercury Transmit), without
// taking the keyboard focus. In full screen: Space plays or stops, J/K/L shuttle, Left/Right step a frame, Esc,
// Ctrl+` or a double-click returns.
Window {
    id: output
    objectName: "programOutputWindow"

    /// The main window (whose display full screen covers): a QQuickWindow (Window.window).
    property var mainWindow: null
    readonly property bool transmitting: !Monitors.fullScreen && Monitors.transmit

    title: qsTr("Program Monitor")
    color: "black"
    transientParent: null // a window of its own, also on another display
    flags: transmitting ? (Qt.Window | Qt.FramelessWindowHint | Qt.WindowDoesNotAcceptFocus)
                        : (Qt.Window | Qt.FramelessWindowHint)

    ProgramMonitor {
        id: picture
        objectName: "programOutputPicture"
        anchors.fill: parent
        surroundColor: "black"
        shareDecoderDevice: false // the main monitor keeps the decoders' device
        previewDivisor: MediaReadiness.previewDivisor
    }

    // On its display first, then shown or hidden; the mirror runs while the output is wanted (also when the window
    // cannot show, e.g. in tests).
    function refreshOutput() {
        Monitors.placeOutputWindow(output, output.mainWindow)
        output.visibility = Monitors.outputVisible ? Window.FullScreen : Window.Hidden
        if (Monitors.outputActive)
            Playback.attachMirror(picture)
        else
            Playback.detachMirror(picture)
    }
    Connections {
        target: Monitors
        function onOutputChanged() { output.refreshOutput() }
    }
    Component.onCompleted: refreshOutput()
    onMainWindowChanged: refreshOutput()

    Item {
        id: keys
        anchors.fill: parent
        focus: true
        Keys.onPressed: event => {
            const back = event.key === Qt.Key_Escape
                    || (event.key === Qt.Key_QuoteLeft && (event.modifiers & Qt.ControlModifier) !== 0)
            if (back) {
                Monitors.fullScreen = false
            } else if (event.key === Qt.Key_Space) {
                Playback.togglePlay()
            } else if (event.key === Qt.Key_J) {
                Playback.shuttle(-1)
            } else if (event.key === Qt.Key_K) {
                Playback.shuttle(0)
            } else if (event.key === Qt.Key_L) {
                Playback.shuttle(1)
            } else if (event.key === Qt.Key_Left) {
                Playback.stepFrames(-1)
            } else if (event.key === Qt.Key_Right) {
                Playback.stepFrames(1)
            } else {
                return
            }
            event.accepted = true
        }
        TapHandler {
            onDoubleTapped: Monitors.fullScreen = false
        }
    }
}
