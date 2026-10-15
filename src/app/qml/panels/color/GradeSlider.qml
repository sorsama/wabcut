import QtQuick

// One grade parameter of the Color panel: a PropSlider bound to Color.params[key], range and default from the
// registry (Color.paramInfo). A drag, scrub or typed edit is one undo step (Color.beginGesture/endGesture); an animated
// parameter gets a key at the playhead (the dot shows it is animated).
PropSlider {
    id: root

    required property string key
    property string title: ""
    readonly property var info: Color.paramInfo(key)
    property bool inGesture: false

    label: title !== "" ? title : (info.name ?? key)
    from: info.from ?? 0
    to: info.to ?? 100
    defaultValue: info["default"] ?? 0
    origin: from < 0 ? 0 : defaultValue
    unit: info.unit ?? ""
    decimals: to - from <= 10 ? 2 : 0
    stepSize: to - from <= 10 ? 0.01 : 1
    value: Color.params[key] ?? defaultValue

    onValueModified: v => {
        if (!inGesture) {
            inGesture = true
            Color.beginGesture()
        }
        Color.setParam(key, v)
    }
    onGestureEnded: {
        if (inGesture) {
            inGesture = false
            Color.endGesture()
        }
    }

    Rectangle {
        // Animated parameter: a small accent diamond before the label (Effect Controls owns its keyframes).
        visible: Color.animated.indexOf(root.key) >= 0
        anchors.verticalCenter: parent.verticalCenter
        x: -Theme.space(2)
        width: 6
        height: 6
        rotation: 45
        color: Theme.accentStroke
    }
}
