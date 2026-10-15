pragma ComponentBehavior: Bound
import QtQuick

// The echo mark's loading states (design language v3, the brand sheet's "Loading states"), in the mark's 120-unit box:
//   equalizer: the clips pump like level meters (audio: analysis, loudness, the mixer)
//   chase:     the mark and its echoes light up in turn (the default: opening projects, switching sequences)
//   scan:      a head sweeps the dimmed mark, lighting it (render, preview and export waits)
//   build:     the clips grow out of dots, hold, and shrink back (long jobs: imports, proxies, conforming)
// Below 28 px each draws the sheet's compact three-clip version. It loops only while `running` and reduce motion is
// off; otherwise it shows the static mark (EchoMark).
Item {
    id: root

    /// equalizer | chase | scan | build (anything else: chase).
    property string variant: "chase"
    /// The mark box's size in px.
    property real size: 110
    property bool running: visible
    readonly property bool animating: running && visible && !Theme.reduceMotion
    readonly property bool compact: size < 28
    readonly property real unit: size / 120

    implicitWidth: size
    implicitHeight: size

    Accessible.role: Accessible.Animation
    Accessible.name: qsTr("Working")

    // ---- Geometry (mark units) ----
    // The mark's clips (EchoMark), and the compact pill-icon clips the sheet draws at 20 px.
    readonly property var markBars: [{ y: 18, w: 28, h: 12 }, { y: 36, w: 52, h: 12 }, { y: 54, w: 78, h: 12, lime: true },
                                     { y: 72, w: 52, h: 12 }, { y: 90, w: 28, h: 12 }]
    readonly property var compactBars: [{ y: 22, w: 34, h: 20 }, { y: 50, w: 74, h: 20, lime: true }, { y: 78, w: 34, h: 20 }]
    // Equalizer strokes: centre y, the two lengths it swings between, the half period.
    readonly property var eqLines: [{ y: 24, a: 16, b: 46, ms: 900 }, { y: 42, a: 40, b: 12, ms: 1100 },
                                    { y: 60, a: 66, b: 30, ms: 800, lime: true }, { y: 78, a: 22, b: 54, ms: 1000 },
                                    { y: 96, a: 16, b: 36, ms: 1200 }]
    readonly property var eqCompactLines: [{ y: 30, a: 16, b: 46, ms: 900 }, { y: 60, a: 66, b: 30, ms: 800, lime: true },
                                           { y: 90, a: 16, b: 36, ms: 1200 }]
    // Build strokes: centre y, full length, start delay.
    readonly property var buildLines: [{ y: 24, l: 16, d: 240 }, { y: 42, l: 40, d: 120 }, { y: 60, l: 66, d: 0, lime: true },
                                       { y: 78, l: 40, d: 120 }, { y: 96, l: 16, d: 240 }]
    readonly property var buildCompactLines: [{ y: 30, l: 16, d: 180 }, { y: 60, l: 66, d: 0, lime: true },
                                              { y: 90, l: 16, d: 180 }]
    readonly property var buildCurve: [0.6, 0, 0.3, 1, 1, 1]
    readonly property var meterCurve: [0.42, 0, 0.58, 1, 1, 1]   // CSS ease-in-out

    // A clip of the mark: `offset` is its layer's x, the bar's own y / width / height in mark units.
    component Clip: Rectangle {
        property real unit: 1
        property real offset: 0
        property var bar: ({})
        x: offset * unit
        y: (bar.y ?? 0) * unit
        width: (bar.w ?? 0) * unit
        height: (bar.h ?? 12) * unit
        radius: height / 2
        color: bar.lime === true ? Theme.brandLime : Theme.brand
        antialiasing: true
    }
    // A round-capped stroke of the sheet's SVG (from x1, `t` thick, `dash` long) as a pill.
    component Stroke: Rectangle {
        property real unit: 1
        property real x1: 40
        property real cy: 60
        property real t: 12
        property real dash: 16
        property bool lime: false
        x: (x1 - t / 2) * unit
        y: (cy - t / 2) * unit
        width: (dash + t) * unit
        height: t * unit
        radius: height / 2
        color: lime ? Theme.brandLime : Theme.brand
        antialiasing: true
    }

    // At rest (or with reduce motion): the mark itself.
    EchoMark {
        objectName: "echoLoaderMark"
        anchors.fill: parent
        size: root.size
        visible: !root.animating
    }

    Loader {
        objectName: "echoLoaderMotion"
        anchors.fill: parent
        active: root.animating
        sourceComponent: root.variant === "equalizer" ? equalizer
                       : root.variant === "scan" ? scan
                       : root.variant === "build" ? build : chase
    }

    // ---- equalizer: three layers of strokes (the echoes lag behind) swing between two lengths ----
    Component {
        id: equalizer
        Item {
            Repeater {
                model: root.compact ? [{ x: 0, o: 1, d: 0 }]
                                    : [{ x: -28, o: 0.15, d: 240 }, { x: -14, o: 0.4, d: 120 }, { x: 0, o: 1, d: 0 }]
                delegate: Item {
                    id: eqLayer
                    required property var modelData
                    width: root.size
                    height: root.size
                    opacity: modelData.o

                    Repeater {
                        model: root.compact ? root.eqCompactLines : root.eqLines
                        delegate: Stroke {
                            id: eqLine
                            required property var modelData
                            unit: root.unit
                            x1: 40 + eqLayer.modelData.x
                            cy: modelData.y
                            t: root.compact ? 16 : 12
                            dash: modelData.a
                            lime: modelData.lime === true

                            SequentialAnimation {
                                running: root.animating
                                PauseAnimation { duration: eqLayer.modelData.d }
                                SequentialAnimation {
                                    loops: Animation.Infinite
                                    NumberAnimation {
                                        target: eqLine; property: "dash"
                                        from: eqLine.modelData.a; to: eqLine.modelData.b; duration: eqLine.modelData.ms
                                        easing.type: Easing.BezierSpline; easing.bezierCurve: root.meterCurve
                                    }
                                    NumberAnimation {
                                        target: eqLine; property: "dash"
                                        from: eqLine.modelData.b; to: eqLine.modelData.a; duration: eqLine.modelData.ms
                                        easing.type: Easing.BezierSpline; easing.bezierCurve: root.meterCurve
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- chase: the echoes and the mark light up one after another ----
    Component {
        id: chase
        Item {
            Repeater {
                model: root.compact ? [{ x: 6, o: 0.18, d: 0 }, { x: 40, o: 1, d: 350 }]
                                    : [{ x: 6, o: 0.18, d: 0 }, { x: 20, o: 0.45, d: 175 }, { x: 34, o: 1, d: 350 }]
                delegate: Item {
                    id: chaseLayer
                    required property var modelData
                    width: root.size
                    height: root.size
                    opacity: modelData.o

                    Repeater {
                        model: root.compact ? root.compactBars : root.markBars
                        delegate: Clip {
                            required property var modelData
                            unit: root.unit
                            offset: chaseLayer.modelData.x
                            bar: modelData
                        }
                    }
                    SequentialAnimation {
                        running: root.animating
                        PauseAnimation { duration: chaseLayer.modelData.d }
                        SequentialAnimation {
                            loops: Animation.Infinite
                            NumberAnimation {
                                target: chaseLayer; property: "opacity"; from: 0.12; to: 1; duration: 315
                                easing.type: Easing.BezierSpline; easing.bezierCurve: root.meterCurve
                            }
                            NumberAnimation {
                                target: chaseLayer; property: "opacity"; from: 1; to: 0.12; duration: 368
                                easing.type: Easing.BezierSpline; easing.bezierCurve: root.meterCurve
                            }
                            PauseAnimation { duration: 367 }
                        }
                    }
                }
            }
        }
    }

    // ---- scan: a head sweeps the dimmed mark, lighting it, then sweeps again turning it off ----
    Component {
        id: scan
        Item {
            id: scanBox
            // 0..1: the lit part grows from the left; 1..2: it shrinks from the left. Linear, 1.8 s a cycle.
            property real phase: 0
            readonly property real litLeft: Math.max(0, phase - 1) * width
            readonly property real litRight: Math.min(1, phase) * width
            readonly property var layers: root.compact ? [{ x: 40, o: 1 }]
                                                      : [{ x: 6, o: 0.18 }, { x: 20, o: 0.45 }, { x: 34, o: 1 }]

            NumberAnimation on phase {
                running: root.animating
                from: 0
                to: 2
                duration: 1800
                loops: Animation.Infinite
            }

            // The dimmed mark underneath.
            Item {
                anchors.fill: parent
                opacity: root.compact ? 0.25 : 0.22
                Repeater {
                    model: root.compact ? [{ x: 40, o: 1 }] : [{ x: 6, o: 0.4 }, { x: 20, o: 0.7 }, { x: 34, o: 1 }]
                    delegate: Item {
                        id: dimLayer
                        required property var modelData
                        width: root.size
                        height: root.size
                        opacity: modelData.o
                        Repeater {
                            model: root.compact ? root.compactBars : root.markBars
                            delegate: Clip {
                                required property var modelData
                                unit: root.unit
                                offset: dimLayer.modelData.x
                                bar: modelData
                            }
                        }
                    }
                }
            }
            // The lit window.
            Item {
                x: scanBox.litLeft
                width: Math.max(0, scanBox.litRight - scanBox.litLeft)
                height: parent.height
                clip: true
                Repeater {
                    model: scanBox.layers
                    delegate: Item {
                        id: litLayer
                        required property var modelData
                        x: -scanBox.litLeft
                        width: root.size
                        height: root.size
                        opacity: modelData.o
                        Repeater {
                            model: root.compact ? root.compactBars : root.markBars
                            delegate: Clip {
                                required property var modelData
                                unit: root.unit
                                offset: litLayer.modelData.x
                                bar: modelData
                            }
                        }
                    }
                }
            }
            // The head (not at icon size).
            Rectangle {
                visible: !root.compact
                width: Math.max(2, 3 * root.unit)
                x: (scanBox.phase < 1 ? scanBox.phase : scanBox.phase - 1) * scanBox.width - width / 2
                y: 6 * root.unit
                height: scanBox.height - 12 * root.unit
                radius: width / 2
                color: Theme.playhead
            }
        }
    }

    // ---- build: the clips grow out of dots, hold, and shrink back while the echoes flash ----
    Component {
        id: build
        Item {
            Item {
                id: ghost
                visible: !root.compact
                width: root.size
                height: root.size
                opacity: 0
                Repeater {
                    model: [{ x: 6, o: 0.18 }, { x: 20, o: 0.45 }]
                    delegate: Item {
                        id: ghostLayer
                        required property var modelData
                        width: root.size
                        height: root.size
                        opacity: modelData.o
                        Repeater {
                            model: root.markBars
                            delegate: Clip {
                                required property var modelData
                                unit: root.unit
                                offset: ghostLayer.modelData.x
                                bar: modelData
                            }
                        }
                    }
                }
                SequentialAnimation {
                    running: root.animating && !root.compact
                    loops: Animation.Infinite
                    PauseAnimation { duration: 576 }
                    NumberAnimation {
                        target: ghost; property: "opacity"; from: 0; to: 1; duration: 234
                        easing.type: Easing.BezierSpline; easing.bezierCurve: root.meterCurve
                    }
                    PauseAnimation { duration: 270 }
                    NumberAnimation {
                        target: ghost; property: "opacity"; from: 1; to: 0; duration: 288
                        easing.type: Easing.BezierSpline; easing.bezierCurve: root.meterCurve
                    }
                    PauseAnimation { duration: 432 }
                }
            }
            Repeater {
                model: root.compact ? root.buildCompactLines : root.buildLines
                delegate: Stroke {
                    id: buildLine
                    required property var modelData
                    unit: root.unit
                    x1: root.compact ? 44 : 40
                    cy: modelData.y
                    t: root.compact ? 18 : 12
                    dash: 0.01
                    opacity: 0
                    lime: modelData.lime === true

                    SequentialAnimation {
                        running: root.animating
                        PauseAnimation { duration: buildLine.modelData.d }
                        SequentialAnimation {
                            loops: Animation.Infinite
                            NumberAnimation {
                                target: buildLine; property: "opacity"; from: 0; to: 1; duration: 144
                                easing.type: Easing.BezierSpline; easing.bezierCurve: root.buildCurve
                            }
                            NumberAnimation {
                                target: buildLine; property: "dash"; from: 0.01; to: buildLine.modelData.l; duration: 486
                                easing.type: Easing.BezierSpline; easing.bezierCurve: root.buildCurve
                            }
                            PauseAnimation { duration: 540 }
                            NumberAnimation {
                                target: buildLine; property: "dash"; from: buildLine.modelData.l; to: 0.01; duration: 414
                                easing.type: Easing.BezierSpline; easing.bezierCurve: root.buildCurve
                            }
                            NumberAnimation {
                                target: buildLine; property: "opacity"; from: 1; to: 0; duration: 216
                                easing.type: Easing.BezierSpline; easing.bezierCurve: root.buildCurve
                            }
                        }
                    }
                }
            }
        }
    }
}
