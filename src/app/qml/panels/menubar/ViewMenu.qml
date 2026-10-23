pragma ComponentBehavior: Bound
import QtQuick

// View (the menu bar, Premiere's View menu): the Program Monitor's rulers and guides, its overlays and indicators, and
// full screen. The switches show whether they are on (Monitors). Every row is a keymap command (its shortcut shows).
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
