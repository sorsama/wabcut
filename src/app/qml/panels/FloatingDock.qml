import QtQuick
import QtQuick.Window

// A docked panel popped out into a window of its own (0.1.3 panel docking): move it to another screen, resize it;
// closing it docks the panel back. Keys typed here reach the same shortcuts as in the main window (KeymapDispatcher).
Window {
    id: win

    /// The DockSlot whose holder this window shows.
    required property DockSlot slot

    objectName: "floatingDock_" + slot.dockKey
    title: qsTr("%1 — Wabcut").arg(slot.title)
    color: Theme.canvas
    minimumWidth: 320
    minimumHeight: 200
    flags: Qt.Window
    visible: slot.active

    // Moved or resized: remembered (a moment later, once it settles) for the next start.
    onXChanged: settle.restart()
    onYChanged: settle.restart()
    onWidthChanged: settle.restart()
    onHeightChanged: settle.restart()
    Timer {
        id: settle
        interval: 500
        onTriggered: win.slot.manager?.save()
    }

    onClosing: close => {
        close.accepted = false
        win.slot.dockBack()
    }
    Component.onCompleted: KeymapDispatcher.addWindow(win)
    Component.onDestruction: KeymapDispatcher.removeWindow(win)
}
