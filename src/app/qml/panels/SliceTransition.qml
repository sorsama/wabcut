import QtQuick

// The transition between workspaces (design language v3): a canvas veil over the workbench that fades out while its
// panels glide to the new layout, over Theme.durSlice (220 ms, snap-out); with reduce motion a fainter fade instead.
// v2 swept a 12° band here; the name and the `progress` / `veil` properties stay (tests and Main use them). It takes
// no input and draws nothing at rest.
Item {
    id: root
    objectName: "sliceTransition"

    // 0 → 1 while the full transition runs (the veil fades with it).
    property real progress: 1
    // The reduce-motion fade's veil.
    property real veil: 0
    readonly property bool running: sweep.running || fade.running

    function play() {
        if (width <= 0 || height <= 0)
            return
        if (Theme.reduceMotion)
            fade.restart()
        else
            sweep.restart()
    }

    enabled: false
    visible: running

    NumberAnimation {
        id: sweep
        target: root
        property: "progress"
        from: 0
        to: 1
        duration: Theme.durSlice
        easing.type: Easing.BezierSpline
        easing.bezierCurve: Theme.easeSnapOut
    }
    NumberAnimation {
        id: fade
        target: root
        property: "veil"
        from: 0.55
        to: 0
        duration: Theme.durSlice
        easing.type: Easing.OutCubic
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
        opacity: sweep.running ? 0.75 * (1 - root.progress) : root.veil
    }
}
