pragma ComponentBehavior: Bound
import QtQuick

// Moments (M5o, gaming): the clip menu's one-click presets for the clip, around the playhead (the clip's middle when
// the playhead is elsewhere): Slow-mo 25 % / 50 %, Speed Ramp In, Freeze and Replay, Fast Forward 200 / 300 / 400 %,
// Punch In, Punch In and Hold, Shake. Each is one undo step of ordinary time remap keys or Motion keyframes
// (MomentsBridge), editable afterwards. Instant Replay (C.16, when the catalog has it; 0.4) replays In to Out when both
// marks are on the clip, else the 5 seconds before the playhead on a long clip, else the whole clip, with the speed,
// Sound and Stinger options remembered from the Moments popover (MomentsBridge applies them). 0.6 (D2 #9): Make a Short
// from In to Out turns the sequence's marks into a 1080x1920 Short with the Highlights Short options, without a find.
// Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
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
                 || Highlights.shortAvailable
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
        // 0.6 (P1 #55): a job; its progress and Cancel show in a toast (Main.qml) while the menu is gone.
        enabled: Moments.version.busy !== true
        onTriggered: menu.run("shortsVersion")
    }
    // 0.6 (P1 round, D2 #9): the In to Out range as a Short, with the Highlights Short options (no find, no subsequence).
    ContextMenuItem {
        objectName: "momentsShortFromInOut"
        visible: Highlights.shortAvailable
        text: qsTr("Make a Short from In to Out")
        iconName: "crop"
        command: "sequence.makeShortFromInOut"
        enabled: Highlights.commandAvailability("sequence.makeShortFromInOut").enabled === true
        onTriggered: {
            menu.error = Highlights.makeShortFromInOut()
            menu.close()
        }
    }
}
