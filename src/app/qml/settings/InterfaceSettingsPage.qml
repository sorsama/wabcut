pragma ComponentBehavior: Bound
import QtQuick

// Settings > Interface (0.1.3): what the title bar shows (the GPU and save-status pills moved here; either can go back
// up) and how panels resize. (The UX rework removed the left rail and its editor: the Window menu opens the panels.)
SettingsPage {
    id: page
    objectName: "interfaceSettingsPage"

    title: qsTr("Interface")
    description: qsTr("The title bar and the workbench's panels.")

    SettingsSection {
        title: qsTr("Title bar")
        description: qsTr("The Save button shows a dot while there are unsaved changes; the first two add the full status.")

        SettingsRow {
            rowKey: "gpuStatus" // the Settings search (stream L)
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
            rowKey: "saveStatusPill" // the Settings search (stream L)
            label: qsTr("Save status")
            description: qsTr("Saved or unsaved, and when the project was last autosaved.")
            Toggle {
                objectName: "showSaveStatusToggle"
                tooltip: qsTr("Show the save status in the title bar")
                checked: Preferences.showSaveStatus
                onToggled: Preferences.showSaveStatus = checked
            }
        }
        // 0.7: the workspace tabs can go to give the menus and the title the room.
        SettingsRow {
            rowKey: "workspaceTabs" // the Settings search
            label: qsTr("Workspace tabs")
            description: Preferences.showWorkspaceTabs
                         ? qsTr("On: the workspaces are tabs in the middle of the title bar.")
                         : qsTr("Off: the tabs are hidden. Switch workspaces from Window > Workspaces, with Alt+Shift+1 to 9 or from the command palette.")
            Toggle {
                objectName: "showWorkspaceTabsToggle"
                tooltip: qsTr("Show the workspace tabs in the title bar")
                checked: Preferences.showWorkspaceTabs
                onToggled: Preferences.showWorkspaceTabs = checked
            }
        }
    }

    SettingsSection {
        title: qsTr("Panels")

        SettingsRow {
            rowKey: "liveResize" // the Settings search (stream L)
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
}
