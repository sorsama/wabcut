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

    // P1 (#19): the preview settings (MediaReadiness), as in the Program Monitor and the View menu.
    component ResolutionItem: ContextMenuItem {
        id: resolution
        required property string level
        checkable: true
        checked: MediaReadiness.previewQuality === resolution.level
        onTriggered: {
            MediaReadiness.previewQuality = resolution.level
            resolution.checked = Qt.binding(() => MediaReadiness.previewQuality === resolution.level)
        }
    }

    ContextMenu {
        objectName: menu.namePrefix + "sourcePlaybackResolutionMenu"
        title: qsTr("Playback Resolution")

        ResolutionItem { text: qsTr("Auto (fit the monitor)"); level: "auto"; command: "monitor.playbackResolutionAuto" }
        ResolutionItem { text: qsTr("Full"); level: "full"; command: "monitor.playbackResolutionFull" }
        ResolutionItem { text: qsTr("1/2"); level: "half"; command: "monitor.playbackResolutionHalf" }
        ResolutionItem { text: qsTr("1/4"); level: "quarter"; command: "monitor.playbackResolutionQuarter" }
    }
    // P1 (#16): the resolution while parked (the same setting as the Program Monitor's).
    PausedResolutionMenu { namePrefix: menu.namePrefix + "source_" }
    ContextMenuItem {
        objectName: menu.namePrefix + "sourceUseProxiesItem"
        text: qsTr("Use Proxies")
        checkable: true
        checked: MediaReadiness.useProxies
        command: "monitor.toggleProxies"
        onTriggered: MediaReadiness.useProxies = !MediaReadiness.useProxies
    }
    ContextMenuSeparator {}
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
    // 0.7 (VR / 360 video): look around in a 360 item.
    ContextMenuItem {
        objectName: menu.namePrefix + "sourceVrViewItem"
        text: qsTr("VR Video View")
        checkable: true
        checked: Monitors.sourceVrView
        enabled: SourceMonitor.vrProjection !== "none"
        onTriggered: Monitors.sourceVrView = !Monitors.sourceVrView
    }
    ContextMenuItem {
        objectName: menu.namePrefix + "sourceSafeMarginsItem"
        text: qsTr("Safe Margins")
        checkable: true
        checked: Preferences.safeMargins
        onTriggered: Preferences.safeMargins = !Preferences.safeMargins
    }
}
