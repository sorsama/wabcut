pragma ComponentBehavior: Bound
import QtQuick

// The launch intro (design language v3, the brand sheet's "Launch intro · Assemble"): once when Wabcut starts, on the
// canvas, the echo mark assembles in 2.1 s. One thin timeline splits into the mark's five clips (they fade in on the
// middle row, thicken while moving to their rows, then trim to length) and the two echoes peel off behind it; then the
// overlay fades away (Theme.durIntroFade). Main.qml calls play() when its window first shows. A click or Esc skips it;
// with reduce motion play() finishes at once without showing anything. While not playing it is hidden and takes no
// input.
Item {
    id: root

    /// The mark's size in px.
    property real markSize: 170
    readonly property bool playing: internals.playing
    signal finished()

    function play() {
        if (Theme.reduceMotion) {
            root.finished()
            return
        }
        intro.stop()
        internals.reset()
        internals.focusBefore = root.Window.activeFocusItem
        internals.playing = true
        root.forceActiveFocus()
        intro.start()
    }
    /// Ends the intro now (a click or Esc).
    function skip() {
        if (internals.playing)
            internals.finish()
    }

    anchors.fill: parent
    z: 1000
    visible: internals.playing
    enabled: internals.playing
    opacity: internals.fade

    Accessible.role: Accessible.Animation
    Accessible.name: qsTr("Wabcut")

    QtObject {
        id: internals
        property bool playing: false
        property Item focusBefore: null
        // The three phases of the clips (fade in, thicken and split, trim), the two echoes, and the overlay.
        property real fadeIn: 0
        property real split: 0
        property real trim: 0
        property real nearEcho: 0
        property real farEcho: 0
        property real fade: 1
        // The sheet's timings at Theme.durIntro (2.1 s).
        readonly property real scale: Theme.durIntro / 2100

        function reset() {
            fadeIn = 0
            split = 0
            trim = 0
            nearEcho = 0
            farEcho = 0
            fade = 1
        }
        function finish() {
            intro.stop()
            playing = false
            fade = 1
            const before = focusBefore
            focusBefore = null
            if (before && before.visible)
                before.forceActiveFocus()
            root.finished()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onClicked: root.skip()
    }
    Keys.onEscapePressed: root.skip()

    Item {
        id: mark
        anchors.centerIn: parent
        width: root.markSize
        height: root.markSize
        readonly property real unit: width / 120

        // The echoes slide in from the right.
        Repeater {
            model: [{ x: 6, o: 0.18, slide: 28, far: true }, { x: 20, o: 0.45, slide: 14, far: false }]
            delegate: Item {
                id: echo
                required property var modelData
                readonly property real progress: modelData.far ? internals.farEcho : internals.nearEcho
                width: mark.width
                height: mark.height
                opacity: modelData.o * progress
                x: modelData.slide * (1 - progress) * mark.unit

                Repeater {
                    model: [{ y: 18, w: 28 }, { y: 36, w: 52 }, { y: 54, w: 78, lime: true }, { y: 72, w: 52 }, { y: 90, w: 28 }]
                    delegate: Rectangle {
                        required property var modelData
                        x: echo.modelData.x * mark.unit
                        y: modelData.y * mark.unit
                        width: modelData.w * mark.unit
                        height: 12 * mark.unit
                        radius: height / 2
                        color: modelData.lime === true ? Theme.brandLime : Theme.brand
                        antialiasing: true
                    }
                }
            }
        }

        // The clips: round-capped strokes from x 40, starting 2 units thick and 66 long on the middle row.
        Repeater {
            model: [{ y: 24, dash: 16, from: 36 }, { y: 42, dash: 40, from: 18 }, { y: 78, dash: 40, from: -18 },
                    { y: 96, dash: 16, from: -36 }, { y: 60, dash: 66, from: 0, lime: true }]
            delegate: Rectangle {
                required property var modelData
                readonly property real thickness: 2 + 10 * internals.split
                readonly property real dash: 66 + (modelData.dash - 66) * internals.trim
                readonly property real centre: modelData.y + modelData.from * (1 - internals.split)
                x: (40 - thickness / 2) * mark.unit
                y: (centre - thickness / 2) * mark.unit
                width: (dash + thickness) * mark.unit
                height: thickness * mark.unit
                radius: height / 2
                opacity: internals.fadeIn
                color: modelData.lime === true ? Theme.brandLime : Theme.brand
                antialiasing: true
            }
        }
    }

    SequentialAnimation {
        id: intro
        NumberAnimation {
            target: internals; property: "fadeIn"; from: 0; to: 1; duration: Math.round(260 * internals.scale)
            easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeIntro
        }
        NumberAnimation {
            target: internals; property: "split"; from: 0; to: 1; duration: Math.round(390 * internals.scale)
            easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeIntro
        }
        NumberAnimation {
            target: internals; property: "trim"; from: 0; to: 1; duration: Math.round(650 * internals.scale)
            easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeIntro
        }
        ParallelAnimation {
            SequentialAnimation {
                PauseAnimation { duration: Math.round(100 * internals.scale) }
                NumberAnimation {
                    target: internals; property: "nearEcho"; from: 0; to: 1; duration: Math.round(550 * internals.scale)
                    easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0.8, 0.2, 1, 1, 1]
                }
            }
            SequentialAnimation {
                PauseAnimation { duration: Math.round(200 * internals.scale) }
                NumberAnimation {
                    target: internals; property: "farEcho"; from: 0; to: 1; duration: Math.round(600 * internals.scale)
                    easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0.8, 0.2, 1, 1, 1]
                }
            }
        }
        NumberAnimation {
            target: internals; property: "fade"; from: 1; to: 0; duration: Theme.durIntroFade
            easing.type: Easing.OutCubic
        }
        ScriptAction { script: internals.finish() }
    }
}
