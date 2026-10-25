pragma ComponentBehavior: Bound
import QtQuick

// P.2 (E6): Premiere's Source Monitor settings menu (the wrench; bridges Monitors and SourceMonitor; docs/monitors.md):
// Loop, Transparency Grid and Safe Margins. Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does
// not shadow ours. 0.4: the transport's "···" menu holds a second one as its Settings submenu (`namePrefix` "overflow_").
ContextMenu {
    id: menu

    /// Put before each row's objectName (a second copy of the menu, e.g. a submenu, keeps the names apart).
    property string namePrefix: ""

    objectName: namePrefix + "sourceMonitorSettingsMenu"

    ContextMenuItem {
        objectName: menu.namePrefix + "sourceLoopItem"
        text: qsTr("Loop")
        checkable: true
        checked: SourceMonitor.loop
        command: "monitor.loop"
        onTriggered: SourceMonitor.loop = !SourceMonitor.loop
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: menu.namePrefix + "sourceTransparencyGridItem"
        text: qsTr("Transparency Grid")
        checkable: true
        checked: Monitors.sourceTransparencyGrid
        command: "sourceMonitor.transparencyGrid"
        onTriggered: Monitors.sourceTransparencyGrid = !Monitors.sourceTransparencyGrid
    }
    ContextMenuItem {
        objectName: menu.namePrefix + "sourceSafeMarginsItem"
        text: qsTr("Safe Margins")
        checkable: true
        checked: Preferences.safeMargins
        onTriggered: Preferences.safeMargins = !Preferences.safeMargins
    }
}
