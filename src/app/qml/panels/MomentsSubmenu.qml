pragma ComponentBehavior: Bound
import QtQuick

// Moments (M5o, gaming): the clip menu's one-click presets for the clip, around the playhead (the clip's middle when
// the playhead is elsewhere): Slow-mo 25 % / 50 %, Speed Ramp In, Freeze and Replay, Fast Forward 200 / 300 / 400 %,
// Punch In, Punch In and Hold, Shake. Each is one undo step of ordinary time remap keys or Motion keyframes
// (MomentsBridge), editable afterwards. Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does
// not shadow ours.
ContextMenu {
    id: menu
    objectName: "momentsSubmenu"

    /// The clip it acts on ("c12").
    property string clipKey: ""
    /// What the clip allows (Moments.targetFor): speed (footage to retime), video (a picture to zoom), atText.
    property var info: ({})
    /// Why the last preset was refused ("" after a good one).
    property string error: ""
    readonly property bool canRetime: menu.info.speed === true
    readonly property bool canZoom: menu.info.available === true

    function refresh() {
        menu.info = Moments.targetFor(menu.clipKey)
        menu.error = ""
    }
    function run(preset) {
        menu.error = Moments.apply(preset, { clip: menu.clipKey })
    }

    title: qsTr("Moments")

    ContextMenuItem {
        objectName: "momentsSlowMo25"
        text: qsTr("Slow-mo 25 %")
        iconName: "timer"
        command: "moments.slowMo"
        enabled: menu.canRetime
        onTriggered: menu.run("slowMo25")
    }
    ContextMenuItem {
        objectName: "momentsSlowMo50"
        text: qsTr("Slow-mo 50 %")
        iconName: "timer"
        enabled: menu.canRetime
        onTriggered: menu.run("slowMo50")
    }
    ContextMenuItem {
        objectName: "momentsRampIn"
        text: qsTr("Speed Ramp In")
        iconName: "gauge"
        enabled: menu.canRetime
        onTriggered: menu.run("rampIn")
    }
    ContextMenuItem {
        objectName: "momentsFreezeReplay"
        text: qsTr("Freeze and Replay")
        iconName: "repeat"
        enabled: menu.canRetime
        onTriggered: menu.run("freezeReplay")
    }
    ContextMenuItem {
        objectName: "momentsFastForward200"
        text: qsTr("Fast Forward 200 %")
        iconName: "skip-forward"
        enabled: menu.canRetime
        onTriggered: menu.run("fastForward200")
    }
    ContextMenuItem {
        objectName: "momentsFastForward300"
        text: qsTr("Fast Forward 300 %")
        iconName: "skip-forward"
        enabled: menu.canRetime
        onTriggered: menu.run("fastForward300")
    }
    ContextMenuItem {
        objectName: "momentsFastForward400"
        text: qsTr("Fast Forward 400 %")
        iconName: "skip-forward"
        enabled: menu.canRetime
        onTriggered: menu.run("fastForward400")
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "momentsPunchIn"
        text: qsTr("Punch In")
        iconName: "zoom-in"
        command: "moments.punchIn"
        enabled: menu.canZoom
        onTriggered: menu.run("punchIn")
    }
    ContextMenuItem {
        objectName: "momentsPunchInHold"
        text: qsTr("Punch In and Hold")
        iconName: "scaling"
        enabled: menu.canZoom
        onTriggered: menu.run("punchInHold")
    }
    ContextMenuItem {
        objectName: "momentsShake"
        text: qsTr("Shake")
        iconName: "move-horizontal"
        enabled: menu.canZoom
        onTriggered: menu.run("shake")
    }
    // C.16's gaming edits, when the catalog has them.
    ContextMenuSeparator {
        visible: Moments.gamingEdits.replay === true || Moments.gamingEdits.version === true
    }
    ContextMenuItem {
        objectName: "momentsInstantReplay"
        visible: Moments.gamingEdits.replay === true
        text: qsTr("Instant Replay")
        iconName: "repeat"
        enabled: menu.canRetime
        onTriggered: menu.run("instantReplay")
    }
    ContextMenuItem {
        objectName: "momentsShortsVersion"
        visible: Moments.gamingEdits.version === true
        text: qsTr("Make Shorts Version")
        iconName: "crop"
        onTriggered: menu.run("shortsVersion")
    }
}
