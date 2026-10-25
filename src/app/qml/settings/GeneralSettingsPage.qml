pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs

// Settings > General (0.1.3): where Wabcut starts, the window frame, the project's save state (it was the title bar's
// autosave pill) with the autosave interval, and the name review notes are signed with.
// UX rework (stream L): Start-up asks what Wabcut shows when it starts (Home, or the last project again:
// AppSettings.launchAction); Projects gathers the project options that were spread over dialogs - the default
// location for new projects (Home.defaultFolderSetting; empty = automatic), how many recent projects Home lists
// (Home.recentLimit; starred ones always show; a lower number hides the older ones, it never forgets them) and
// project locking (ProjectFile.lockProjects, the same switch as Preferences > Media and Timeline). Every row has a
// rowKey for the Settings search (AppSettings.index).
// 0.4: the save state says when autosave failed (red); Background work keeps Windows awake while long work runs.
SettingsPage {
    id: page
    objectName: "generalSettingsPage"

    title: qsTr("General")
    description: qsTr("Start-up, the window, and how your projects are saved.")

    // The workspaces Wabcut can start in: the title bar's, in its order, without Export (never where Wabcut starts).
    // Never a workspace made from the Export preset (Wabcut does not start there).
    readonly property var startWorkspaces: Workspaces.all.filter(w => w.preset !== "export")
    readonly property int startId: Workspaces.startId(Preferences.startWorkspace)
    // Autosave every 30 s, 1, 2, 5 or 10 minutes (0 in Preferences = the built-in minute).
    readonly property var autosaveChoices: [30, 60, 120, 300, 600]
    // What Wabcut shows when it starts (AppSettings.launchAction).
    readonly property var launchChoices: ["home", "reopen"]
    // How many recent projects Home lists (Home.recentLimit, 5-200; the older ones come back when it is raised).
    readonly property var recentChoices: [10, 20, 50, 100, 200]

    /// A folder URL as the system writes the path ("D:\Projects", "/home/me/Projects"); "" for an empty URL.
    function displayPath(folder: url): string {
        const text = folder.toString()
        if (!text.startsWith("file:"))
            return text
        let path = decodeURIComponent(text.replace(/^file:\/\//, ""))
        if (/^\/[A-Za-z]:/.test(path)) // file:///C:/... on Windows
            path = path.substring(1)
        return Qt.platform.os === "windows" ? path.replace(/\//g, "\\") : path
    }

    SettingsSection {
        title: qsTr("Start-up")

        SettingsRow {
            rowKey: "launchAction"
            label: qsTr("When Wabcut starts")
            description: qsTr("Reopen opens your newest recent project; Home shows when that file is missing.")
            PillTabs {
                objectName: "launchActionTabs"
                compact: true
                model: [qsTr("Show Home"), qsTr("Reopen the last project")]
                currentIndex: Math.max(0, page.launchChoices.indexOf(AppSettings.launchAction))
                Accessible.name: qsTr("When Wabcut starts")
                onActivated: index => {
                    AppSettings.launchAction = page.launchChoices[index]
                    currentIndex = Qt.binding(() => Math.max(0, page.launchChoices.indexOf(AppSettings.launchAction)))
                }
            }
        }
        SettingsRow {
            rowKey: "startWorkspace"
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
            rowKey: "nativeFrame"
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
        title: qsTr("Projects")

        SettingsRow {
            objectName: "projectSaveStatus"
            rowKey: "saveStatus"
            // 0.4: a failed autosave: "Autosave failed", a red dot, the reason ("Autosave failed 14:05: <reason>").
            label: ProjectFile.demo ? qsTr("Demo project")
                 : !ProjectFile.autosaveOk ? qsTr("Autosave failed")
                 : ProjectFile.dirty ? qsTr("Unsaved changes") : qsTr("All changes saved")
            description: ProjectFile.demo ? qsTr("The demo is never saved. Start a project from Home to keep your work.")
                       : [ProjectFile.autosaveText, ProjectFile.filePath].filter(s => s !== "").join(" · ")
            Rectangle {
                objectName: "projectSaveStatusDot"
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                width: Theme.dotSize + Theme.space(1)
                height: width
                radius: width / 2
                color: !ProjectFile.demo && !ProjectFile.autosaveOk ? Theme.danger
                     : ProjectFile.dirty ? Theme.warning : Theme.success
                Accessible.ignored: true
            }
        }
        SettingsRow {
            rowKey: "autosave"
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
        SettingsRow {
            objectName: "projectLocationRow"
            rowKey: "projectLocation"
            label: qsTr("Default location for new projects")
            description: Home.defaultFolderSetting.toString() === ""
                         //: Settings > General > Projects: no folder chosen; %1 = the folder Home suggests now
                         ? qsTr("Automatic: the last project's folder, now %1").arg(page.displayPath(Home.defaultFolder))
                         : page.displayPath(Home.defaultFolderSetting)
            PillButton {
                objectName: "projectLocationAutomatic"
                compact: true
                variant: "ghost"
                visible: Home.defaultFolderSetting.toString() !== ""
                text: qsTr("Automatic")
                tooltip: qsTr("Suggest the last project's folder again")
                onClicked: Home.defaultFolderSetting = ""
            }
            PillButton {
                objectName: "projectLocationBrowse"
                compact: true
                iconName: "folder-open"
                text: qsTr("Browse…")
                tooltip: qsTr("Choose the folder new projects are saved in")
                Accessible.name: qsTr("Choose the default location for new projects")
                onClicked: folderDialog.open()
            }
        }
        SettingsRow {
            rowKey: "recentProjects"
            label: qsTr("Recent projects to show")
            description: qsTr("How many projects Home and File > Open Recent list. Fewer hides the older ones until you raise it again; starred projects always show.")
            PillTabs {
                objectName: "recentProjectsTabs"
                compact: true
                model: page.recentChoices.map(n => n.toString())
                currentIndex: page.recentChoices.indexOf(Home.recentLimit)
                Accessible.name: qsTr("Recent projects to show")
                onActivated: index => {
                    Home.recentLimit = page.recentChoices[index]
                    currentIndex = Qt.binding(() => page.recentChoices.indexOf(Home.recentLimit))
                }
            }
        }
        SettingsRow {
            rowKey: "lockProjects"
            label: qsTr("Enable project locking")
            description: qsTr("Someone else who opens a project you have open (in a shared folder) gets it read-only. Projects in a production are always locked.")
            Toggle {
                objectName: "settingsLockProjects"
                tooltip: qsTr("Enable project locking")
                checked: ProjectFile.lockProjects
                onToggled: {
                    ProjectFile.lockProjects = checked
                    checked = Qt.binding(() => ProjectFile.lockProjects)
                }
            }
        }
    }

    // 0.4: keep Windows awake while long work runs (Main's PowerKeeper; Preferences.keepAwake, power/keepAwake).
    SettingsSection {
        title: qsTr("Background work")

        SettingsRow {
            rowKey: "keepAwake"
            label: qsTr("Keep the computer awake while exporting")
            description: qsTr("While exports, Render In to Out, proxies, transcription, Highlights, Shorts or Collect Files run, Windows does not go to sleep on its own. The screen can still turn off. On Windows.")
            Toggle {
                objectName: "keepAwakeToggle"
                tooltip: qsTr("Keep the computer awake while exporting")
                checked: Preferences.keepAwake
                onToggled: {
                    Preferences.keepAwake = checked
                    checked = Qt.binding(() => Preferences.keepAwake)
                }
            }
        }
    }

    SettingsSection {
        title: qsTr("Review")

        SettingsRow {
            rowKey: "reviewAuthor"
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

    FolderDialog {
        id: folderDialog
        title: qsTr("Default location for new projects")
        currentFolder: Home.defaultFolder
        onAccepted: Home.defaultFolderSetting = selectedFolder
    }
}
