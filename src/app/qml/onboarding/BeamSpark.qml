import QtQuick

// The echo spark (design language v3, a light event): the Keyboard Trainer's success light. play() flashes three
// clips of the echo mark, a long lime one between two short violet ones, that slide in a step from the left as they
// light; it fades in over durSpark and out over twice that. Never under reduce motion.
Item {
    id: root

    function play() {
        if (Theme.reduceMotion)
            return
        flash.restart()
    }

    opacity: 0
    visible: opacity > 0
    // The clips' slide, in px (0 at the peak of the flash).
    property real drift: 0
    readonly property real clipHeight: Math.max(Theme.space(1.5), Math.round(Math.min(root.width * 0.12, root.height * 0.18)))

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -root.clipHeight * 1.5
        x: (root.width - width) / 2 - Theme.space(3) + root.drift
        width: Math.round(root.width * 0.3)
        height: root.clipHeight
        radius: height / 2
        color: Theme.brand
        opacity: 0.85
    }
    Rectangle {
        objectName: "beamSparkSlash"
        anchors.verticalCenter: parent.verticalCenter
        x: (root.width - width) / 2 + root.drift
        width: Math.round(root.width * 0.62)
        height: root.clipHeight
        radius: height / 2
        color: Theme.brandLime
    }
    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: root.clipHeight * 1.5
        x: (root.width - width) / 2 - Theme.space(3) + root.drift
        width: Math.round(root.width * 0.3)
        height: root.clipHeight
        radius: height / 2
        color: Theme.brand
        opacity: 0.85
    }

    SequentialAnimation {
        id: flash
        ParallelAnimation {
            NumberAnimation { target: root; property: "opacity"; from: 0; to: 1; duration: Theme.durSpark; easing.type: Easing.OutCubic }
            NumberAnimation { target: root; property: "drift"; from: -Theme.space(3); to: 0; duration: Theme.durSpark; easing.type: Easing.OutCubic }
        }
        NumberAnimation { target: root; property: "opacity"; to: 0; duration: Theme.durSpark * 2; easing.type: Easing.InCubic }
    }
}
