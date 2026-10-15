pragma ComponentBehavior: Bound
import QtQuick

// Settings > Interface (0.1.3): what the title bar shows (the GPU and save-status pills moved here; either can go back
// up), the left rail's entries (order, shown or hidden: RailCustomizer) and how panels resize.
SettingsPage {
    id: page
    objectName: "interfaceSettingsPage"

    /// The left rail's catalog (LeftRail.panels).
    property var railPanels: []

    title: qsTr("Interface")
    description: qsTr("The title bar, the panel rail on the left, and the workbench's panels.")

    SettingsSection {
        title: qsTr("Title bar")
        description: qsTr("The Save button shows a dot while there are unsaved changes; these add the full status.")

        SettingsRow {
            label: qsTr("GPU status")
            description: qsTr("The graphics API and adapter Wabcut renders with (also in Playback & performance).")
            Toggle {
                objectName: "showGpuStatusToggle"
                tooltip: qsTr("Show the GPU status in the title bar")
                checked: Preferences.showGpuStatus
                onToggled: Preferences.showGpuStatus = checked
            }
        }
        SettingsRow {
            label: qsTr("Save status")
            description: qsTr("Saved or unsaved, and when the project was last autosaved.")
            Toggle {
                objectName: "showSaveStatusToggle"
                tooltip: qsTr("Show the save status in the title bar")
                checked: Preferences.showSaveStatus
                onToggled: Preferences.showSaveStatus = checked
            }
        }
    }

    SettingsSection {
        id: railSection
        objectName: "railSettingsSection"
        title: qsTr("Left rail")
        description: qsTr("Drag the grips or use the arrows to reorder, the eye to show or hide a panel. Right-click the rail for the same.")

        RailCustomizer {
            panels: page.railPanels
        }
    }

    SettingsSection {
        title: qsTr("Panels")

        SettingsRow {
            label: qsTr("Resize panels live")
            description: Preferences.liveResize
                         ? qsTr("On: the panels follow the pointer while you drag a gutter.")
                         : qsTr("Off: a guide follows the pointer and the panels resize when you let go (smoother on big projects).")
            Toggle {
                objectName: "liveResizeToggle"
                tooltip: qsTr("Resize panels live")
                checked: Preferences.liveResize
                onToggled: Preferences.liveResize = checked
            }
        }
    }

    /// Scrolls to the rail editor (the rail's "Customize rail…").
    function revealRail() {
        page.reveal(railSection)
    }
}
