pragma ComponentBehavior: Bound
import QtQuick

// The microphone's level (A.8, M5m): `levels` is VoiceOver.levels ({ peakDb, rmsDb, clipped }, ~30 Hz while the input
// is open; {} while it is closed) on a dB scale from `floorDb` to 0. The RMS is the bar (green, yellow from -18 dB, red
// from -6 dB), the peak a tick, and the clip light stays lit after an over until it is clicked or the input closes.
// Horizontal (the count-in overlay, Preferences > Audio) or `vertical`; `showScale` adds the dB marks under a
// horizontal meter.
Item {
    id: root

    property var levels: VoiceOver.levels
    property bool vertical: false
    property bool showScale: false
    property real floorDb: -60
    property real warnDb: -18
    property real hotDb: -6
    /// An over since the light was last cleared.
    property bool clipHeld: false

    readonly property bool live: root.isLive(root.levels)
    readonly property real peakDb: root.live ? Number(root.levels.peakDb) : root.floorDb
    readonly property real rmsDb: root.live ? Number(root.levels.rmsDb) : root.floorDb
    readonly property real peakPosition: root.position(root.peakDb)
    readonly property real rmsPosition: root.position(root.rmsDb)
    // Design language v3: the meter hues (mint, lime, orange); the clip light stays red.
    readonly property color levelColor: root.rmsDb >= root.hotDb ? Theme.hueOrange : root.rmsDb >= root.warnDb ? Theme.hueLime : Theme.hueMint
    readonly property real thickness: Theme.space(2)
    readonly property real lightSize: Theme.space(2)

    /// Levels from an open input (a closed one gives {}).
    function isLive(l) {
        return l !== undefined && l !== null && l.peakDb !== undefined
    }
    /// 0 at the floor (and below: silence is -inf), 1 at 0 dBFS.
    function position(db) {
        if (!(db > root.floorDb))
            return 0
        return Math.min(1, (db - root.floorDb) / -root.floorDb)
    }

    // From the new levels, not `live`: its binding may not have been updated yet when this runs.
    onLevelsChanged: {
        if (!root.isLive(root.levels))
            root.clipHeld = false
        else if (root.levels.clipped === true)
            root.clipHeld = true
    }

    implicitWidth: vertical ? thickness : Theme.space(40)
    implicitHeight: vertical ? Theme.space(30) : thickness + (showScale ? marks.implicitHeight + Theme.space(1) : 0)

    Accessible.role: Accessible.ProgressBar
    Accessible.name: qsTr("Input level")
    Accessible.description: root.live ? qsTr("Peak %1 dB").arg(root.peakDb > -120 ? root.peakDb.toFixed(1) : "-∞") : qsTr("No input")

    // The trough with the RMS bar and the peak tick.
    Rectangle {
        id: trough
        objectName: "inputMeterTrough"
        x: 0
        y: root.vertical ? root.lightSize + Theme.space(1) : 0
        width: root.vertical ? root.thickness : root.width - root.lightSize - Theme.space(1)
        height: root.vertical ? root.height - root.lightSize - Theme.space(1) : root.thickness
        radius: Theme.radiusBadge / 2
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border
        clip: true

        Rectangle {
            objectName: "inputMeterBar"
            x: 0
            y: root.vertical ? parent.height - height : 0
            width: root.vertical ? parent.width : parent.width * root.rmsPosition
            height: root.vertical ? parent.height * root.rmsPosition : parent.height
            color: root.levelColor
            opacity: root.live ? 1 : 0
        }
        Rectangle {
            objectName: "inputMeterPeak"
            visible: root.live && root.peakPosition > 0
            x: root.vertical ? 0 : Math.max(0, parent.width * root.peakPosition - width)
            y: root.vertical ? Math.min(parent.height - height, parent.height * (1 - root.peakPosition)) : 0
            width: root.vertical ? parent.width : Theme.hairline * 2
            height: root.vertical ? Theme.hairline * 2 : parent.height
            color: Theme.textPrimary
        }
    }

    // The clip light (above a vertical meter): lit after an over, cleared by a click.
    Rectangle {
        id: light
        objectName: "inputMeterClip"
        x: root.vertical ? 0 : trough.width + Theme.space(1)
        y: 0
        width: root.lightSize
        height: root.lightSize
        radius: Theme.radiusBadge / 2
        color: root.clipHeld ? Theme.danger : Theme.sunken
        border.width: Theme.hairline
        border.color: root.clipHeld ? Theme.danger : Theme.border
        Accessible.role: Accessible.Indicator
        Accessible.name: root.clipHeld ? qsTr("Clipped: click to clear") : qsTr("No clipping")
        Accessible.onPressAction: root.clipHeld = false
        TapHandler {
            onTapped: root.clipHeld = false
        }
        HoverHandler {
            enabled: root.clipHeld
            cursorShape: Qt.PointingHandCursor
        }
    }

    // dB marks under a horizontal meter.
    Item {
        id: marks
        visible: root.showScale && !root.vertical
        y: root.thickness + Theme.space(1)
        width: trough.width
        implicitHeight: Theme.textXs + Theme.space(1)
        height: implicitHeight
        Repeater {
            model: [-48, -36, -24, -12, -6, 0]
            delegate: Text {
                required property int modelData
                x: Math.min(marks.width - implicitWidth, Math.max(0, marks.width * root.position(modelData) - implicitWidth / 2))
                text: String(modelData)
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
        }
    }
}
