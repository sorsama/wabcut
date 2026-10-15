pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The 5.1 panner of a mono or stereo track (A.9, M5n; Premiere's track 5.1 panner), shown on the mixer strip instead
// of the pan knob while the master is 5.1: a square with the five full-range speakers (L −30°, R +30°, C 0°, Ls −110°,
// Rs +110°, the front at the top) and the puck at (x, y) (−1 left .. +1 right, −1 back .. +1 front). Its distance from
// the centre is the focus: at the rim the nearest speakers, at the centre all five. Drag the puck (one undo step per
// drag), arrow keys step 0.05 (Shift: 0.01), double-click returns it to the front centre. Below it, the LFE send.
// `moved(x, y, lfeDb)` asks for the change; `released()` ends the drag.
ColumnLayout {
    id: root

    property real panX: 0
    property real panY: 1
    property real lfeDb: -144
    property string label: ""

    readonly property bool held: area.pressed || lfe.pressed

    signal moved(real x, real y, real lfeDb)
    signal released()

    function clamp(v) { return Math.max(-1, Math.min(1, Math.round(v * 100) / 100)) }
    function step(dx, dy) {
        root.moved(clamp(panX + dx), clamp(panY + dy), lfeDb)
        root.released()
    }
    /// The LFE slider: −60 dB and below is off (−144).
    function lfeFromSlider(v) { return v <= -60 ? -144 : Math.round(v) }

    spacing: Theme.space(1)

    Rectangle {
        id: field
        objectName: "surroundField"
        Layout.alignment: Qt.AlignHCenter
        implicitWidth: Theme.space(18)
        implicitHeight: Theme.space(18)
        radius: Theme.radiusField
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: area.containsMouse || field.activeFocus ? Theme.accentStroke : Theme.border
        activeFocusOnTab: root.enabled
        opacity: root.enabled ? 1 : Theme.disabledOpacity

        Accessible.role: Accessible.Dial
        Accessible.name: qsTr("5.1 panner of %1").arg(root.label)
        Accessible.description: qsTr("Left %1, front %2").arg(root.panX.toFixed(2)).arg(root.panY.toFixed(2))

        Keys.onLeftPressed: event => root.step((event.modifiers & Qt.ShiftModifier) ? -0.01 : -0.05, 0)
        Keys.onRightPressed: event => root.step((event.modifiers & Qt.ShiftModifier) ? 0.01 : 0.05, 0)
        Keys.onUpPressed: event => root.step(0, (event.modifiers & Qt.ShiftModifier) ? 0.01 : 0.05)
        Keys.onDownPressed: event => root.step(0, (event.modifiers & Qt.ShiftModifier) ? -0.01 : -0.05)

        readonly property real inner: width - Theme.space(3)
        function toX(x) { return Theme.space(1.5) + (x + 1) / 2 * inner }
        function toY(y) { return Theme.space(1.5) + (1 - y) / 2 * inner }

        // The listener's circle and cross.
        Rectangle {
            anchors.centerIn: parent
            width: field.inner
            height: width
            radius: width / 2
            color: "transparent"
            border.width: Theme.hairline
            border.color: Theme.border
        }
        Rectangle {
            anchors.centerIn: parent
            width: Theme.hairline
            height: field.inner
            color: Theme.border
        }
        Rectangle {
            anchors.centerIn: parent
            width: field.inner
            height: Theme.hairline
            color: Theme.border
        }

        // The five speakers.
        Repeater {
            model: [{ name: qsTr("L"), deg: -30 }, { name: qsTr("C"), deg: 0 }, { name: qsTr("R"), deg: 30 },
                    { name: qsTr("Ls"), deg: -110 }, { name: qsTr("Rs"), deg: 110 }]
            delegate: Text {
                id: speaker
                required property var modelData
                readonly property real rad: modelData.deg * Math.PI / 180
                x: field.toX(Math.sin(rad) * 0.92) - width / 2
                y: field.toY(Math.cos(rad) * 0.92) - height / 2
                text: modelData.name
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
            }
        }

        // The puck.
        Rectangle {
            id: puck
            objectName: "surroundPuck"
            width: Theme.space(3.5)
            height: width
            radius: width / 2
            x: field.toX(root.panX) - width / 2
            y: field.toY(root.panY) - height / 2
            color: Theme.accentFill
            border.width: Theme.focusRingWidth
            border.color: area.pressed ? Theme.textPrimary : Theme.accentStroke
            scale: area.pressed && !Theme.reduceMotion ? Theme.grabScale : 1
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            preventStealing: true
            enabled: root.enabled
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            function place(mouse) {
                const x = (mouse.x - Theme.space(1.5)) / field.inner * 2 - 1
                const y = 1 - (mouse.y - Theme.space(1.5)) / field.inner * 2
                root.moved(root.clamp(x), root.clamp(y), root.lfeDb)
            }
            onPressed: mouse => {
                field.forceActiveFocus(Qt.MouseFocusReason)
                place(mouse)
            }
            onPositionChanged: mouse => {
                if (pressed)
                    place(mouse)
            }
            onReleased: root.released()
            onDoubleClicked: {
                root.moved(0, 1, root.lfeDb)
                root.released()
            }
        }

        Loader {
            active: area.containsMouse && root.label !== ""
            sourceComponent: Tooltip {
                parent: field
                visible: true
                text: qsTr("5.1 panner of %1: drag the puck; double-click for the front centre").arg(root.label)
            }
        }
        FocusRing {
            targetRadius: Theme.radiusField
            visible: field.activeFocus
        }
    }

    // The LFE send.
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        Text {
            text: qsTr("LFE")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.DemiBold
        }
        ValueSlider {
            id: lfe
            objectName: "surroundLfe"
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.space(12)
            from: -60
            to: 0
            stepSize: 1
            value: Math.max(-60, root.lfeDb)
            Accessible.name: qsTr("LFE send of %1").arg(root.label)
            onMoved: root.moved(root.panX, root.panY, root.lfeFromSlider(value))
            onPressedChanged: {
                if (!pressed)
                    root.released()
            }
        }
    }
    Text {
        objectName: "surroundLfeValue"
        Layout.alignment: Qt.AlignHCenter
        text: root.lfeDb <= -60 ? qsTr("LFE off") : qsTr("LFE %1 dB").arg(Math.round(root.lfeDb))
        color: Theme.textTertiary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textXs
    }
}
