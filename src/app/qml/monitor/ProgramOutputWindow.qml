import QtQuick
import QtQuick.Window

// P.2 (E6): the second program output (bridge Monitors; docs/monitors.md): a top-level window that shows the program
// through a playback mirror (Playback.attachMirror: the same transport and edits, decoded on its own in CPU frames, so
// the main monitor keeps the decoders' GPU device). Toggle Full Screen (Ctrl+`) puts it full screen on the main
// window's display; Enable Transmit keeps it full screen on the chosen display (Premiere's Mercury Transmit), without
// taking the keyboard focus. In full screen: Space plays or stops, J/K/L shuttle, Left/Right step a frame, Esc,
// Ctrl+` or a double-click returns. 0.7 (card 110): Monitors.outputScaling / outputSafeMargins / outputSyncOffsetMs shape the
// transmitted picture (Video Output Settings...); the output is off while its display is not connected.
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

    // 0.7 (card 110): the picture sits in the display as the output settings say (Monitors.outputScaling: Fit letterboxes it,
    // Fill crops it to cover the display, 1:1 shows the frame's own pixels) while transmitting; full screen always fits.
    Item {
        id: stage
        objectName: "programOutputStage"
        anchors.fill: parent
        clip: true
        readonly property real pixelRatio: output.screen ? output.screen.devicePixelRatio : 1
        readonly property var placed: {
            // Both are read as values so the placement is made again when the scaling, the display or the sequence's shape
            // change (the plan changes with each).
            const scaling = Monitors.outputScaling
            const plan = Monitors.outputPlan
            if (!output.transmitting || scaling === "" || plan.state === undefined)
                return { x: 0, y: 0, width: stage.width, height: stage.height }
            return Monitors.placeOutputPicture(stage.width, stage.height, stage.pixelRatio)
        }

        ProgramMonitor {
            id: picture
            objectName: "programOutputPicture"
            x: stage.placed.x
            y: stage.placed.y
            width: stage.placed.width
            height: stage.placed.height
            surroundColor: "black"
            shareDecoderDevice: false // the main monitor keeps the decoders' device
            previewDivisor: MediaReadiness.previewDivisor
        }
        // Title and action safe lines over the picture (Premiere's Safe Margins; here the output's own switch).
        SafeMarginsOverlay {
            objectName: "programOutputSafeMargins"
            x: picture.x
            y: picture.y
            width: picture.width
            height: picture.height
            visible: output.transmitting && Monitors.outputSafeMargins
        }
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
