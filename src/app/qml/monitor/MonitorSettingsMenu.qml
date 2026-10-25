pragma ComponentBehavior: Bound
import QtQuick

// P.2 (E6): Premiere's Program Monitor settings menu (the wrench; bridges Monitors and Playback; docs/monitors.md):
// Loop, rulers and guides (Show Rulers, Show Guides, Lock Guides, Add Guide…, Clear Guides, Guide Templates, Snap in
// Program Monitor), Transparency Grid, Show Dropped Frame Indicator, Overlays, Show Audio Time Units, Toggle Full Screen
// and Transmit (the display, and whether it goes dark with Wabcut in the background). 0.4: Playback Resolution (Auto,
// Full, 1/2, 1/4 and Use proxies, as the readout row's pill, which gives way on a narrow monitor), Safe Margins, and
// Platform Safe Zones (Off, YouTube Shorts, TikTok, Instagram Reels; Preferences.platformSafeZone), shaded on vertical
// sequences only. Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "programMonitorSettingsMenu"

    // One playback resolution (MediaReadiness.previewQuality).
    component ResolutionItem: ContextMenuItem {
        id: resolution
        required property string value
        objectName: "playbackResolution_" + resolution.value
        checkable: true
        checked: MediaReadiness.previewQuality === resolution.value
        onTriggered: MediaReadiness.previewQuality = resolution.value
    }

    ContextMenuItem {
        objectName: "programLoopItem"
        text: qsTr("Loop")
        checkable: true
        checked: Playback.loop
        command: "monitor.loop"
        onTriggered: Playback.loop = !Playback.loop
    }
    // 0.4: Premiere's wrench has the playback resolution too.
    ContextMenu {
        objectName: "playbackResolutionMenu"
        title: qsTr("Playback Resolution")

        ResolutionItem { value: "auto"; text: qsTr("Auto (fit the monitor)") }
        ResolutionItem { value: "full"; text: qsTr("Full") }
        ResolutionItem { value: "half"; text: qsTr("1/2") }
        ResolutionItem { value: "quarter"; text: qsTr("1/4") }
        ContextMenuSeparator {}
        ContextMenuItem {
            objectName: "playbackResolutionProxies"
            text: qsTr("Use proxies")
            checkable: true
            checked: MediaReadiness.useProxies
            onTriggered: MediaReadiness.useProxies = !MediaReadiness.useProxies
        }
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "showRulersItem"
        text: qsTr("Show Rulers")
        checkable: true
        checked: Monitors.showRulers
        command: "programMonitor.showRulers"
        onTriggered: Monitors.showRulers = !Monitors.showRulers
    }
    ContextMenuItem {
        objectName: "showGuidesItem"
        text: qsTr("Show Guides")
        checkable: true
        checked: Monitors.showGuides
        command: "programMonitor.showGuides"
        onTriggered: Monitors.showGuides = !Monitors.showGuides
    }
    ContextMenuItem {
        objectName: "lockGuidesItem"
        text: qsTr("Lock Guides")
        checkable: true
        checked: Monitors.lockGuides
        command: "programMonitor.lockGuides"
        onTriggered: Monitors.lockGuides = !Monitors.lockGuides
    }
    ContextMenuItem {
        objectName: "addGuideItem"
        text: qsTr("Add Guide…")
        command: "programMonitor.addGuide"
        onTriggered: Monitors.runCommand("programMonitor.addGuide")
    }
    ContextMenuItem {
        objectName: "clearGuidesItem"
        text: qsTr("Clear Guides")
        enabled: Monitors.guides.length > 0
        command: "programMonitor.clearGuides"
        onTriggered: Monitors.clearGuides()
    }
    ContextMenu {
        id: templates
        objectName: "guideTemplatesMenu"
        title: qsTr("Guide Templates")

        Instantiator {
            model: Monitors.guideTemplates
            delegate: ContextMenuItem {
                required property string modelData
                text: modelData
                onTriggered: Monitors.applyGuideTemplate(modelData)
            }
            onObjectAdded: (index, object) => templates.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => templates.removeItem(object as ContextMenuItem)
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            text: qsTr("Save Guides as Template…")
            enabled: Monitors.guides.length > 0
            command: "programMonitor.saveGuides"
            onTriggered: Monitors.runCommand("programMonitor.saveGuides")
        }
        ContextMenuItem {
            text: qsTr("Manage Guides…")
            command: "programMonitor.manageGuides"
            onTriggered: Monitors.runCommand("programMonitor.manageGuides")
        }
    }
    ContextMenuItem {
        objectName: "snapItem"
        text: qsTr("Snap in Program Monitor")
        checkable: true
        checked: Monitors.snap
        command: "programMonitor.snap"
        onTriggered: Monitors.snap = !Monitors.snap
    }
    ContextMenuItem {
        objectName: "programTransparencyGridItem"
        text: qsTr("Transparency Grid")
        checkable: true
        checked: Monitors.programTransparencyGrid
        command: "programMonitor.transparencyGrid"
        onTriggered: Monitors.programTransparencyGrid = !Monitors.programTransparencyGrid
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "droppedFrameIndicatorItem"
        text: qsTr("Show Dropped Frame Indicator")
        checkable: true
        checked: Monitors.droppedFrameIndicator
        command: "programMonitor.droppedFrameIndicator"
        onTriggered: Monitors.droppedFrameIndicator = !Monitors.droppedFrameIndicator
    }
    ContextMenuItem {
        objectName: "overlaysItem"
        text: qsTr("Overlays")
        checkable: true
        checked: Monitors.overlays
        command: "programMonitor.overlays"
        onTriggered: Monitors.overlays = !Monitors.overlays
    }
    // 0.4: Premiere's wrench has Safe Margins too (the readout row's toggle hides when the monitor is narrow).
    ContextMenuItem {
        objectName: "programSafeMarginsItem"
        text: qsTr("Safe Margins")
        checkable: true
        checked: Preferences.safeMargins
        onTriggered: Preferences.safeMargins = !Preferences.safeMargins
    }
    // 0.4: where Shorts, TikTok or Reels put their buttons and captions over a vertical picture (PlatformSafeZoneOverlay).
    ContextMenu {
        id: platformZones
        objectName: "platformSafeZoneMenu"
        title: qsTr("Platform Safe Zones")

        readonly property bool portrait: ProjectController.frameHeight > ProjectController.frameWidth

        ContextMenuItem {
            objectName: "platformSafeZoneHint"
            text: qsTr("Shown on vertical sequences")
            enabled: false
            visible: !platformZones.portrait
            height: visible ? implicitHeight : 0
        }
        ContextMenuItem {
            objectName: "platformSafeZone_off"
            text: qsTr("Off")
            checkable: true
            checked: Preferences.platformSafeZone === "off"
            onTriggered: Preferences.platformSafeZone = "off"
        }
        ContextMenuItem {
            objectName: "platformSafeZone_shorts"
            text: qsTr("YouTube Shorts")
            checkable: true
            checked: Preferences.platformSafeZone === "shorts"
            onTriggered: Preferences.platformSafeZone = "shorts"
        }
        ContextMenuItem {
            objectName: "platformSafeZone_tiktok"
            text: qsTr("TikTok")
            checkable: true
            checked: Preferences.platformSafeZone === "tiktok"
            onTriggered: Preferences.platformSafeZone = "tiktok"
        }
        ContextMenuItem {
            objectName: "platformSafeZone_reels"
            text: qsTr("Instagram Reels")
            checkable: true
            checked: Preferences.platformSafeZone === "reels"
            onTriggered: Preferences.platformSafeZone = "reels"
        }
    }
    ContextMenuItem {
        objectName: "audioTimeUnitsItem"
        text: qsTr("Show Audio Time Units")
        checkable: true
        checked: Monitors.audioTimeUnits
        command: "programMonitor.audioTimeUnits"
        onTriggered: Monitors.audioTimeUnits = !Monitors.audioTimeUnits
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "fullScreenItem"
        text: qsTr("Toggle Full Screen")
        checkable: true
        checked: Monitors.fullScreen
        command: "monitor.toggleFullScreen"
        onTriggered: Monitors.fullScreen = !Monitors.fullScreen
    }
    ContextMenuItem {
        objectName: "transmitItem"
        text: qsTr("Enable Transmit")
        checkable: true
        checked: Monitors.transmit
        enabled: Monitors.transmitScreen >= 0
        command: "monitor.enableTransmit"
        onTriggered: Monitors.transmit = !Monitors.transmit
    }
    ContextMenu {
        id: displays
        objectName: "transmitDisplayMenu"
        title: qsTr("Transmit To")

        Instantiator {
            model: Monitors.screens
            delegate: ContextMenuItem {
                required property var modelData
                text: modelData.primary ? qsTr("Display %1: %2 × %3 (main)").arg(modelData.index + 1).arg(modelData.width).arg(modelData.height)
                                        : qsTr("Display %1: %2 × %3").arg(modelData.index + 1).arg(modelData.width).arg(modelData.height)
                checkable: true
                checked: Monitors.transmitScreen === modelData.index
                onTriggered: Monitors.transmitScreen = modelData.index
            }
            onObjectAdded: (index, object) => displays.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => displays.removeItem(object as ContextMenuItem)
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            text: qsTr("Disable Video Output When in the Background")
            checkable: true
            checked: Monitors.disableTransmitInBackground
            onTriggered: Monitors.disableTransmitInBackground = !Monitors.disableTransmitInBackground
        }
    }
}
