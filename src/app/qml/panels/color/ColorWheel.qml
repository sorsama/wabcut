import QtQuick
import QtQuick.Layouts

// A colour wheel of the grade (I.5): the centre puck moves the colour balance (the three channels around their mean),
// the slider below moves the mean (luma); double-click resets the wheel. Values are the grade's parameters
// `<prefix>.red/.green/.blue` (lift, gamma, gain, offset - the ASC CDL); one drag is one undo step.
//
// The puck's position is the chroma of (r, g, b) on the hue circle: x = 3/2 (r - m), y = sqrt(3)/2 (g - b) with
// m = (r + g + b) / 3, so red points right (0 deg), green to 120 deg, blue to 240 deg - the hues drawn under it.
ColumnLayout {
    id: root

    property string title: ""
    property string prefix: "wheels.lift"
    /// The neutral value of the channels (0 for lift and offset, 1 for gamma and gain).
    property real neutral: 0
    /// Chroma at the rim of the wheel, in parameter units.
    property real range: 0.25
    property real lumaFrom: -1
    property real lumaTo: 1
    property int size: 108

    readonly property real r: Color.params[prefix + ".red"] ?? neutral
    readonly property real g: Color.params[prefix + ".green"] ?? neutral
    readonly property real b: Color.params[prefix + ".blue"] ?? neutral
    readonly property real mean: (r + g + b) / 3
    readonly property real chromaX: 1.5 * (r - mean)
    readonly property real chromaY: Math.sqrt(3) / 2 * (g - b)
    property bool inGesture: false

    function begin() {
        if (!inGesture) {
            inGesture = true
            Color.beginGesture()
        }
    }
    function end() {
        if (inGesture) {
            inGesture = false
            Color.endGesture()
        }
    }
    /// Writes mean m with chroma (x, y).
    function write(m, x, y) {
        const cr = 2 * x / 3
        const cg = (-cr + 2 * y / Math.sqrt(3)) / 2
        const cb = (-cr - 2 * y / Math.sqrt(3)) / 2
        const values = {}
        values[prefix + ".red"] = m + cr
        values[prefix + ".green"] = m + cg
        values[prefix + ".blue"] = m + cb
        Color.setParams(values)
    }

    spacing: Theme.space(1)

    Text {
        Layout.alignment: Qt.AlignHCenter
        text: root.title
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }

    Item {
        id: wheel
        Layout.alignment: Qt.AlignHCenter
        implicitWidth: root.size
        implicitHeight: root.size
        readonly property real radius: width / 2 - 4

        Canvas {
            id: ring
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const cx = width / 2, cy = height / 2, r = wheel.radius
                // The hues themselves (data, not styling), counter-clockwise from red at 0 degrees.
                const hues = ctx.createConicalGradient(cx, cy, 0)
                for (let i = 0; i <= 12; ++i)
                    hues.addColorStop(i / 12, Qt.hsva((i / 12) % 1, 0.75, 1, 1))
                ctx.beginPath()
                ctx.arc(cx, cy, r, 0, 2 * Math.PI)
                ctx.fillStyle = hues
                ctx.fill()
                const fade = ctx.createRadialGradient(cx, cy, 0, cx, cy, r)
                fade.addColorStop(0, Theme.sunken)
                fade.addColorStop(0.75, Qt.alpha(Theme.sunken, 0.55))
                fade.addColorStop(1, Qt.alpha(Theme.sunken, 0.15))
                ctx.fillStyle = fade
                ctx.fill()
                ctx.lineWidth = 1
                ctx.strokeStyle = Theme.borderStrong
                ctx.stroke()
                ctx.strokeStyle = Theme.guide
                ctx.beginPath()
                ctx.moveTo(cx - r, cy)
                ctx.lineTo(cx + r, cy)
                ctx.moveTo(cx, cy - r)
                ctx.lineTo(cx, cy + r)
                ctx.stroke()
            }
        }

        Rectangle {
            id: puck
            readonly property real dx: Math.max(-1, Math.min(1, root.chromaX / root.range)) * wheel.radius
            readonly property real dy: Math.max(-1, Math.min(1, root.chromaY / root.range)) * wheel.radius
            x: wheel.width / 2 + dx - width / 2
            y: wheel.height / 2 - dy - height / 2
            width: Theme.sliderHandle
            height: Theme.sliderHandle
            radius: width / 2
            color: Theme.raised
            border.width: Theme.focusRingWidth
            border.color: area.pressed || area.containsMouse ? Theme.accentStroke : Theme.borderStrong
            scale: area.pressed ? Theme.grabScale : 1
            Behavior on scale { NumberAnimation { duration: Theme.durFast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft } }
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.CrossCursor
            property real startX: 0
            property real startY: 0
            property real baseX: 0
            property real baseY: 0
            function move(mx, my) {
                // Relative to where the drag started, so grabbing the puck never jumps it.
                let x = root.range * (baseX + (mx - startX) / wheel.radius)
                let y = root.range * (baseY - (my - startY) / wheel.radius)
                const len = Math.hypot(x, y)
                if (len > root.range) {
                    x *= root.range / len
                    y *= root.range / len
                }
                root.write(root.mean, x, y)
            }
            onPressed: mouse => {
                root.begin()
                startX = mouse.x
                startY = mouse.y
                baseX = root.chromaX / root.range
                baseY = root.chromaY / root.range
            }
            onPositionChanged: mouse => {
                if (pressed)
                    move(mouse.x, mouse.y)
            }
            onReleased: root.end()
            onCanceled: root.end()
            onDoubleClicked: {
                root.begin()
                root.write(root.neutral, 0, 0)
                root.end()
            }
        }

        Accessible.role: Accessible.Dial
        Accessible.name: qsTr("%1 colour balance").arg(root.title)
    }

    ValueSlider {
        Layout.fillWidth: true
        Layout.maximumWidth: root.size + Theme.space(6)
        Layout.alignment: Qt.AlignHCenter
        from: root.lumaFrom
        to: root.lumaTo
        origin: root.neutral
        value: root.mean
        stepSize: 0.001
        onMoved: {
            root.begin()
            root.write(value, root.chromaX, root.chromaY)
        }
        onPressedChanged: {
            if (!pressed)
                root.end()
        }
        Accessible.name: qsTr("%1 luma").arg(root.title)
    }
}
