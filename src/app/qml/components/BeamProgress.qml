import QtQuick

// Progress (design language v3, the kit's progress bar): an 8 px pill track in `well` with a solid fill (`hue`, the
// accent by default). `value` is 0..1; every job shows one (M5j). A job without a percentage is `indeterminate`:
// given the room (its height reaches a LoaderPill's, as it does at its implicit height) it shows the echo mark's
// loader in a pill with the label (`loader`: chase, equalizer, scan or build; "" keeps the bar); squeezed to a bar by
// its caller, a segment slides along the track instead. Nothing moves while the job is paused (`active` false) or
// under reduce motion.
Item {
    id: root

    property real value: 0
    property bool active: true
    property bool indeterminate: false
    property string label: ""
    property color hue: Theme.accentFill
    property string loader: "chase"
    readonly property real pillHeight: Theme.space(8.5)
    /// The loader pill is shown (indeterminate, a loader, and the room for it).
    readonly property bool pill: indeterminate && loader !== "" && height >= pillHeight - 0.5

    implicitWidth: Theme.space(30)
    implicitHeight: indeterminate && loader !== "" ? pillHeight : Theme.progressHeight

    Accessible.role: Accessible.ProgressBar
    Accessible.name: label
    Accessible.description: indeterminate ? qsTr("Working") : qsTr("%1%").arg(Math.round(Math.max(0, Math.min(1, value)) * 100))

    LoaderPill {
        objectName: "progressLoader"
        visible: root.pill
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, root.width)
        variant: root.loader
        text: root.label
        running: root.pill && root.active
    }

    Item {
        id: bar
        visible: !root.pill
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: Math.min(root.height, Theme.progressHeight)

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: Theme.well
        }
        // The done part.
        Rectangle {
            visible: !root.indeterminate
            width: Math.round(bar.width * Math.max(0, Math.min(1, root.value)))
            height: bar.height
            radius: height / 2
            color: root.hue
        }
        // No percentage: a segment slides to and fro (the whole track lit when nothing may move).
        Rectangle {
            id: segment
            readonly property bool sliding: root.indeterminate && !root.pill && root.active && !Theme.reduceMotion
                                            && bar.visible && bar.width > bar.height * 4
            visible: root.indeterminate
            width: sliding ? Math.max(bar.height, Math.round(bar.width * 0.3)) : bar.width
            height: bar.height
            radius: height / 2
            color: root.hue
            opacity: sliding ? 1 : 0.6

            SequentialAnimation on x {
                running: segment.sliding
                loops: Animation.Infinite
                NumberAnimation {
                    from: 0; to: bar.width - segment.width; duration: 900
                    easing.type: Easing.InOutSine
                }
                NumberAnimation {
                    from: bar.width - segment.width; to: 0; duration: 900
                    easing.type: Easing.InOutSine
                }
            }
            onSlidingChanged: if (!sliding) x = 0
        }
    }
}
