pragma ComponentBehavior: Bound
import QtQuick

// Right-click menu of a Multi-Camera view tile (0.7, card 009; bridge `Multicam`): nudge the angle's sync by frames
// without opening the multicam source - Sync 1 Frame Earlier / Later and 10 Frames Earlier / Later (multicam.setSync
// with offset = current + delta, one undo step "Sync Angle"; an angle that would start before 0 stays at 0 and the
// others move later), Reset to Measured Sync when the angle is off the offset its sync method measured (the tile shows
// "+2 f"), and Open Multi-Camera Source, the way Premiere does it (the source sequence in its own tab).
// Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "multicamAngleMenu"

    /// The angle the menu acts on (1-based) and how far it is from its measured sync (MulticamEditBridge.angles' syncFrames).
    property int angle: 0
    property int syncFrames: 0

    /// Opens the menu for `angleNumber` at `point` of `target`.
    function openFor(target: Item, angleNumber: int, shift: int, point: point) {
        menu.angle = angleNumber
        menu.syncFrames = shift
        menu.popup(target, point.x, point.y)
    }
    function nudge(frames: int) {
        Multicam.nudgeAngleSync(menu.angle, frames)
    }

    ContextMenuItem {
        objectName: "multicamSyncEarlier"
        text: qsTr("Sync 1 Frame Earlier")
        onTriggered: menu.nudge(-1)
    }
    ContextMenuItem {
        objectName: "multicamSyncLater"
        text: qsTr("Sync 1 Frame Later")
        onTriggered: menu.nudge(1)
    }
    ContextMenuItem {
        objectName: "multicamSyncEarlier10"
        text: qsTr("Sync 10 Frames Earlier")
        onTriggered: menu.nudge(-10)
    }
    ContextMenuItem {
        objectName: "multicamSyncLater10"
        text: qsTr("Sync 10 Frames Later")
        onTriggered: menu.nudge(10)
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "multicamSyncReset"
        text: qsTr("Reset to Measured Sync")
        enabled: menu.syncFrames !== 0
        disabledReason: qsTr("This angle is where its sync put it")
        onTriggered: Multicam.resetAngleSync(menu.angle)
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "multicamOpenSource"
        text: qsTr("Open Multi-Camera Source")
        enabled: Multicam.sourceKey !== ""
        onTriggered: SequenceTabs.open(Multicam.sourceKey)
    }
}
