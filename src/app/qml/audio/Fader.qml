pragma ComponentBehavior: Bound
import QtQuick

// A vertical gain fader (Audio Track Mixer, M5g): +12 dB at the top to −∞ at the bottom, 0 dB marked. Drag the cap
// (Shift: fine, a tenth of the travel), double-click resets to 0 dB, ↑/↓ step 0.5 dB (Shift: 0.1). `moved(dB)` asks
// for the change; the owner applies it (AudioMixer.setGainDb). Disabled faders show `note` as their tooltip.
Item {
    id: root

    property real gainDb: 0
    property string note: ""
    property string label: ""
    readonly property real topDb: 12
    readonly property real bottomDb: -60

    /// The cap is held (M5l: Touch and Latch automation write while it is).
    readonly property bool held: area.pressed

    signal moved(real gainDb)
    signal released()

    function position(db) {
        return db <= bottomDb ? 1 : Math.max(0, Math.min(1, (topDb - db) / (topDb - bottomDb)))
    }
    function dbAt(pos) {
        return pos >= 1 ? -Infinity : topDb - Math.max(0, pos) * (topDb - bottomDb)
    }
    function step(delta) {
        const current = isFinite(gainDb) ? gainDb : bottomDb
        root.moved(Math.max(bottomDb, Math.min(topDb, Math.round((current + delta) * 10) / 10)))
        root.released()
    }

    implicitWidth: Theme.space(7)
    implicitHeight: Theme.space(40)
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : Theme.disabledOpacity
    Accessible.role: Accessible.Slider
    Accessible.name: label
    Accessible.description: note

    Keys.onUpPressed: event => step((event.modifiers & Qt.ShiftModifier) ? 0.1 : 0.5)
    Keys.onDownPressed: event => step((event.modifiers & Qt.ShiftModifier) ? -0.1 : -0.5)

    // Track
    Rectangle {
        id: track
        anchors.horizontalCenter: parent.horizontalCenter
        y: Theme.space(2)
        width: Theme.sliderTrack
        height: parent.height - Theme.space(4)
        radius: width / 2
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border
    }
    // 0 dB tick
    Rectangle {
        x: track.x - Theme.space(1)
        y: track.y + root.position(0) * track.height
        width: track.width + Theme.space(2)
        height: Theme.hairline
        color: Theme.textTertiary
    }
    // Cap
    Rectangle {
        id: cap
        anchors.horizontalCenter: parent.horizontalCenter
        y: track.y + root.position(root.gainDb) * track.height - height / 2
        width: root.width - Theme.space(1)
        height: Theme.space(3)
        radius: Theme.radiusBadge
        color: area.pressed ? Theme.accentFill : Theme.raised
        border.width: Theme.hairline
        border.color: area.containsMouse ? Theme.accentStroke : Theme.borderStrong
        Rectangle {
            anchors.centerIn: parent
            width: parent.width - Theme.space(2)
            height: Theme.hairline
            color: area.pressed ? Theme.accentOnFill : Theme.textSecondary
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        property real pressY: 0
        property real pressPos: 0
        onPressed: mouse => {
            root.forceActiveFocus(Qt.MouseFocusReason)
            pressY = mouse.y
            pressPos = root.position(root.gainDb)
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const scale = (mouse.modifiers & Qt.ShiftModifier) ? 0.1 : 1
            const pos = pressPos + (mouse.y - pressY) / track.height * scale
            const db = root.dbAt(pos)
            root.moved(isFinite(db) ? Math.round(db * 10) / 10 : db)
        }
        onReleased: root.released()
        onDoubleClicked: {
            root.moved(0)
            root.released()
        }
    }

    Loader {
        active: area.containsMouse && (root.note !== "" || root.label !== "")
        sourceComponent: Tooltip {
            parent: root
            visible: true
            text: root.enabled ? root.label : root.note
        }
    }

    FocusRing {
        targetRadius: Theme.radiusField
        visible: root.activeFocus
    }
}
