pragma ComponentBehavior: Bound
import QtQuick

// View (the menu bar, Premiere's View menu): the preview settings (Playback Resolution, Use Proxies), the Program
// Monitor's rulers and guides, its overlays and indicators, and full screen. The switches show whether they are on
// (Monitors, MediaReadiness). Every row is a keymap command (its shortcut shows).
MenuBarMenu {
    id: root
    objectName: "viewMenu"
    title: qsTr("View")
    key: "view"
    mnemonic: "V"

    // A switch row: checked from the Monitors bridge, rebound after a choice (a checkable row toggles itself).
    component SwitchItem: BarMenuItem {
        id: item
        /// Whether it is on (a binding to the Monitors bridge).
        property bool on: false
        checkable: true
        checked: item.on
        onTriggered: item.checked = Qt.binding(() => item.on)
    }

    // P1 (#19): a playback resolution row; its check shows the setting (MediaReadiness.previewQuality), its command
    // runs through the bar like any View row.
    component ResolutionItem: BarMenuItem {
        id: resolution
        required property string level
        checkable: true
        checked: MediaReadiness.previewQuality === resolution.level
        onTriggered: resolution.checked = Qt.binding(() => MediaReadiness.previewQuality === resolution.level)
    }

    // P1 (#16): a paused resolution row (MediaReadiness.pausedQuality), as ResolutionItem.
    component PausedItem: BarMenuItem {
        id: paused
        required property string level
        checkable: true
        checked: MediaReadiness.pausedQuality === paused.level
        onTriggered: paused.checked = Qt.binding(() => MediaReadiness.pausedQuality === paused.level)
    }

    // Premiere's View menu starts with Playback Resolution and Use Proxies, above the rulers.
    MenuBarMenu {
        objectName: "viewPlaybackResolutionMenu"
        title: qsTr("Playback Resolution")
        nested: true
        bar: root.bar

        ResolutionItem {
            text: qsTr("Auto (fit the monitor)")
            command: "monitor.playbackResolutionAuto"
            level: "auto"
        }
        ResolutionItem {
            text: qsTr("Full")
            command: "monitor.playbackResolutionFull"
            level: "full"
        }
        ResolutionItem {
            text: qsTr("1/2")
            command: "monitor.playbackResolutionHalf"
            level: "half"
        }
        ResolutionItem {
            text: qsTr("1/4")
            command: "monitor.playbackResolutionQuarter"
            level: "quarter"
        }
    }
    // P1 (#16): Premiere's Paused Resolution (the monitors' wrench has it too).
    MenuBarMenu {
        objectName: "viewPausedResolutionMenu"
        title: qsTr("Paused Resolution")
        nested: true
        bar: root.bar

        PausedItem {
            text: qsTr("Full")
            command: "monitor.pausedResolutionFull"
            level: "full"
        }
        PausedItem {
            text: qsTr("1/2")
            command: "monitor.pausedResolutionHalf"
            level: "half"
        }
        PausedItem {
            text: qsTr("1/4")
            command: "monitor.pausedResolutionQuarter"
            level: "quarter"
        }
        ContextMenuSeparator {}
        PausedItem {
            text: qsTr("Same as Playback")
            command: "monitor.pausedResolutionSame"
            level: "same"
        }
    }
    SwitchItem {
        text: qsTr("Use Proxies")
        command: "monitor.toggleProxies"
        on: MediaReadiness.useProxies
    }
    ContextMenuSeparator {}
    SwitchItem {
        text: qsTr("Show Rulers")
        command: "programMonitor.showRulers"
        on: Monitors.showRulers
    }
    SwitchItem {
        text: qsTr("Show Guides")
        command: "programMonitor.showGuides"
        on: Monitors.showGuides
    }
    SwitchItem {
        text: qsTr("Snap in Program Monitor")
        command: "programMonitor.snap"
        on: Monitors.snap
    }
    SwitchItem {
        text: qsTr("Lock Guides")
        command: "programMonitor.lockGuides"
        on: Monitors.lockGuides
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Add Guide…")
        command: "programMonitor.addGuide"
    }
    BarMenuItem {
        text: qsTr("Clear Guides")
        command: "programMonitor.clearGuides"
    }
    BarMenuItem {
        text: qsTr("Save Guides as Template…")
        command: "programMonitor.saveGuides"
    }
    BarMenuItem {
        text: qsTr("Manage Guides…")
        command: "programMonitor.manageGuides"
    }
    ContextMenuSeparator {}
    SwitchItem {
        text: qsTr("Transparency Grid")
        command: "programMonitor.transparencyGrid"
        on: Monitors.programTransparencyGrid
    }
    SwitchItem {
        text: qsTr("Overlays")
        command: "programMonitor.overlays"
        on: Monitors.overlays
    }
    SwitchItem {
        text: qsTr("Rule of Thirds Guides")
        command: "programMonitor.ruleOfThirds"
        on: (Monitors.compositionGuides & 1) !== 0
    }
    SwitchItem {
        text: qsTr("Golden Ratio Guides")
        command: "programMonitor.goldenRatio"
        on: (Monitors.compositionGuides & 2) !== 0
    }
    SwitchItem {
        text: qsTr("Diagonal Guides")
        command: "programMonitor.diagonalGuides"
        on: (Monitors.compositionGuides & 4) !== 0
    }
    SwitchItem {
        text: qsTr("Show Dropped Frame Indicator")
        command: "programMonitor.droppedFrameIndicator"
        on: Monitors.droppedFrameIndicator
    }
    SwitchItem {
        text: qsTr("Show Audio Time Units")
        command: "programMonitor.audioTimeUnits"
        on: Monitors.audioTimeUnits
    }
    ContextMenuSeparator {}
    SwitchItem {
        text: qsTr("Full Screen")
        command: "monitor.toggleFullScreen"
        on: Monitors.fullScreen
    }
}
