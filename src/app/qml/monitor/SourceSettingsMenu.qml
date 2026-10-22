pragma ComponentBehavior: Bound
import QtQuick

// P.2 (E6): Premiere's Source Monitor settings menu (the wrench; bridges Monitors and SourceMonitor; docs/monitors.md):
// Loop, Transparency Grid and Safe Margins. Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does
// not shadow ours.
ContextMenu {
    id: menu
    objectName: "sourceMonitorSettingsMenu"

    ContextMenuItem {
        objectName: "sourceLoopItem"
        text: qsTr("Loop")
        checkable: true
        checked: SourceMonitor.loop
        command: "monitor.loop"
        onTriggered: SourceMonitor.loop = !SourceMonitor.loop
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "sourceTransparencyGridItem"
        text: qsTr("Transparency Grid")
        checkable: true
        checked: Monitors.sourceTransparencyGrid
        command: "sourceMonitor.transparencyGrid"
        onTriggered: Monitors.sourceTransparencyGrid = !Monitors.sourceTransparencyGrid
    }
    ContextMenuItem {
        objectName: "sourceSafeMarginsItem"
        text: qsTr("Safe Margins")
        checkable: true
        checked: Preferences.safeMargins
        onTriggered: Preferences.safeMargins = !Preferences.safeMargins
    }
}
