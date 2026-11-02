pragma ComponentBehavior: Bound
import QtQuick

// P1 (#16): Premiere's Paused Resolution, as a submenu of the monitors' menus (the Program Monitor's quality pill and
// wrench, the Source Monitor's wrench): the resolution a parked frame is drawn at - Full, 1/2, 1/4, or the same as
// playback. It is MediaReadiness.pausedQuality, the one setting both monitors use (Settings > Media & cache has the
// same choice). The rows are the keymap commands monitor.pausedResolution*, so a bound key shows. Its own file,
// importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu

    /// Put before each row's objectName (each monitor menu holds its own copy).
    property string namePrefix: ""

    objectName: namePrefix + "pausedResolutionMenu"
    title: qsTr("Paused Resolution")

    component PausedItem: ContextMenuItem {
        id: row
        required property string level
        objectName: menu.namePrefix + "pausedResolution_" + row.level
        checkable: true
        checked: MediaReadiness.pausedQuality === row.level
        command: "monitor.pausedResolution" + row.level.charAt(0).toUpperCase() + row.level.slice(1)
        onTriggered: {
            MediaReadiness.pausedQuality = row.level
            row.checked = Qt.binding(() => MediaReadiness.pausedQuality === row.level)
        }
    }

    PausedItem { level: "full"; text: qsTr("Full") }
    PausedItem { level: "half"; text: qsTr("1/2") }
    PausedItem { level: "quarter"; text: qsTr("1/4") }
    ContextMenuSeparator {}
    PausedItem { level: "same"; text: qsTr("Same as playback") }
}
