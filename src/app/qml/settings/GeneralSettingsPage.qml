pragma ComponentBehavior: Bound
import QtQuick

// Settings > General (0.1.3): where Wabcut starts, the window frame, the project's save state (it was the title bar's
// autosave pill) with the autosave interval, and the name review notes are signed with.
SettingsPage {
    id: page
    objectName: "generalSettingsPage"

    title: qsTr("General")
    description: qsTr("Start-up, the window, and how your project is saved.")

    // The workspaces Wabcut can start in: the title bar's, in its order, without Export (never where Wabcut starts).
    // Never a workspace made from the Export preset (Wabcut does not start there).
    readonly property var startWorkspaces: Workspaces.all.filter(w => w.preset !== "export")
    readonly property int startId: Workspaces.startId(Preferences.startWorkspace)
    // Autosave every 30 s, 1, 2, 5 or 10 minutes (0 in Preferences = the built-in minute).
    readonly property var autosaveChoices: [30, 60, 120, 300, 600]

    SettingsSection {
        title: qsTr("Start-up")

        SettingsRow {
            label: qsTr("Open in")
            description: qsTr("The workspace Wabcut shows when it starts.")
            PillButton {
                id: startButton
                objectName: "startWorkspaceButton"
                compact: true
                variant: "secondary"
                iconName: "chevron-down"
                LayoutMirroring.enabled: true    // label first, chevron last
                LayoutMirroring.childrenInherit: true
                text: Workspaces.label(page.startId) || Workspaces.presetLabel("edit")
                Accessible.name: qsTr("Start workspace: %1").arg(text)
                onClicked: startMenu.popup(startButton, 0, startButton.height + Theme.space(1))

                ContextMenu {
                    id: startMenu
                    objectName: "startWorkspaceMenu"

                    Instantiator {
                        model: page.startWorkspaces
                        delegate: ContextMenuItem {
                            required property var modelData
                            text: Workspaces.label(modelData.id)
                            checkable: true
                            checked: modelData.id === page.startId
                            onTriggered: Preferences.startWorkspace = modelData.id
                        }
                        onObjectAdded: (index, object) => startMenu.insertItem(index, object as ContextMenuItem)
                        onObjectRemoved: (index, object) => startMenu.removeItem(object as ContextMenuItem)
                    }
                }
            }
        }
        SettingsRow {
            label: qsTr("Use native window frame")
            description: qsTr("The system's title bar and borders instead of Wabcut's frameless window. Applies immediately.")
            Toggle {
                objectName: "nativeFrameToggle"
                tooltip: qsTr("Use native window frame")
                checked: Preferences.nativeFrame
                onToggled: Preferences.nativeFrame = checked
            }
        }
    }

    SettingsSection {
        title: qsTr("Project")

        SettingsRow {
            objectName: "projectSaveStatus"
            label: ProjectFile.demo ? qsTr("Demo project")
                 : ProjectFile.dirty ? qsTr("Unsaved changes") : qsTr("All changes saved")
            description: ProjectFile.demo ? qsTr("The demo is never saved. Start a project from Home to keep your work.")
                       : [ProjectFile.autosaveText, ProjectFile.filePath].filter(s => s !== "").join(" · ")
            Rectangle {
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                width: Theme.dotSize + Theme.space(1)
                height: width
                radius: width / 2
                color: ProjectFile.dirty ? Theme.warning : Theme.success
                Accessible.ignored: true
            }
        }
        SettingsRow {
            label: qsTr("Autosave every")
            description: qsTr("A copy of the open project is written in the background; File > Recover lists them.")
            PillTabs {
                objectName: "autosaveIntervalTabs"
                width: Theme.space(100)
                compact: true
                model: [qsTr("30 s"), qsTr("1 min"), qsTr("2 min"), qsTr("5 min"), qsTr("10 min")]
                currentIndex: Math.max(0, page.autosaveChoices.indexOf(ProjectFile.autosaveIntervalSeconds))
                onActivated: index => Preferences.autosaveSeconds = page.autosaveChoices[index]
                Accessible.name: qsTr("Autosave interval")
            }
        }
    }

    SettingsSection {
        title: qsTr("Review")

        SettingsRow {
            label: qsTr("Sign notes as")
            description: qsTr("The name review notes and replies carry.")
            FieldInput {
                objectName: "reviewAuthorField"
                width: Theme.space(56)
                text: Preferences.reviewAuthor
                placeholderText: qsTr("Editor")
                Accessible.name: qsTr("Review author name")
                onEditingFinished: Preferences.reviewAuthor = text.trim()
            }
        }
    }
}
