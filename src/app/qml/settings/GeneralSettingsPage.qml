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

    // 0.7 (card #70): where a new export's file goes (ExportController.defaultFolderMode).
    readonly property var exportFolderModes: ["last", "project", "media", "fixed"]
    // The folder a new export goes to now (follows the mode, the chosen folders and the open project).
    readonly property string exportFolderNow: (ExportController.defaultFolderMode + ExportController.defaultFolder
                                               + ExportController.lastFolder + ProjectFile.filePath).length >= 0
                                              ? ExportController.defaultFolderFor("") : ""

    // The workspaces Wabcut can start in: the title bar's, in its order, without Export (never where Wabcut starts).
    // Never a workspace made from the Export preset (Wabcut does not start there).
    readonly property var startWorkspaces: Workspaces.all.filter(w => w.preset !== "export")
    readonly property int startId: Workspaces.startId(Preferences.startWorkspace)
    // Autosave every 30 s, 1, 2, 5 or 10 minutes (0 in Preferences = the built-in minute).
    readonly property var autosaveChoices: [30, 60, 120, 300, 600]
    // Where autosaves and backups go (ProjectFile.backupLocation), in the order of the row's tabs.
    readonly property var backupLocations: ["beside", "folder", "chosen"]
    // 0.7 (card 006): Keep versions every Off (0), 2, 5, 10 or 30 minutes (History.autoIntervalSeconds).
    readonly property var versionChoices: [0, 120, 300, 600, 1800]
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
        // 0.7 (card 030): Windows lists the recent and starred projects when the taskbar button is right-clicked.
        SettingsRow {
            objectName: "jumpListRow"
            rowKey: "jumpList"
            label: qsTr("Show recent projects in the taskbar jump list")
            description: JumpList.available
                         ? qsTr("Right-click Wabcut's taskbar button to open a starred or recent project. Off removes the list.")
                         : qsTr("Windows only: right-clicking Wabcut's taskbar button lists the starred and recent projects.")
            Toggle {
                objectName: "jumpListToggle"
                enabled: JumpList.available
                tooltip: qsTr("Show recent projects in the taskbar jump list")
                checked: JumpList.available && JumpList.enabled
                onToggled: {
                    JumpList.enabled = checked
                    checked = Qt.binding(() => JumpList.available && JumpList.enabled)
                }
            }
        }
        // 0.7 (card 029): where the autosave and the ".bak" versions of a saved project go, and how many are kept.
        SettingsRow {
            objectName: "backupLocationRow"
            rowKey: "backupLocation"
            label: qsTr("Autosaves and backups")
            //: Settings > General > Projects; %1 = where the open project's copies go now
            description: qsTr("Autosaves and earlier saved versions of a project are kept in: %1").arg(ProjectFile.backupWhere)
            PillTabs {
                objectName: "backupLocationTabs"
                width: Theme.space(120)
                compact: true
                model: [qsTr("Next to the project"), qsTr("Auto-Save folder"), qsTr("A folder")]
                currentIndex: Math.max(0, page.backupLocations.indexOf(ProjectFile.backupLocation))
                Accessible.name: qsTr("Where autosaves and backups are kept")
                onActivated: index => {
                    ProjectFile.backupLocation = page.backupLocations[index]
                    currentIndex = Qt.binding(() => Math.max(0, page.backupLocations.indexOf(ProjectFile.backupLocation)))
                    if (page.backupLocations[index] === "chosen" && ProjectFile.backupFolder.toString() === "")
                        backupFolderDialog.open()
                }
            }
            PillButton {
                objectName: "backupFolderBrowse"
                compact: true
                iconName: "folder-open"
                visible: ProjectFile.backupLocation === "chosen"
                text: qsTr("Choose…")
                tooltip: qsTr("Choose the folder autosaves and backups are kept in")
                Accessible.name: qsTr("Choose the folder for autosaves and backups")
                onClicked: backupFolderDialog.open()
            }
        }
        SettingsRow {
            rowKey: "backupCount"
            label: qsTr("Backups to keep")
            description: qsTr("Earlier saved versions of a project, newest first (File > Recover lists them). 0 keeps none.")
            NumberField {
                objectName: "settingsBackupCount"
                width: Theme.numberFieldWidth
                from: 0
                to: 10
                value: ProjectFile.backupCount
                Accessible.name: qsTr("Backups to keep")
                onValueModified: value => ProjectFile.backupCount = Math.round(value)
            }
        }
        SettingsRow {
            rowKey: "autosaveCopies"
            label: qsTr("Autosave versions to keep")
            description: qsTr("Older autosaves rotate out after this many. At least 1.")
            NumberField {
                objectName: "settingsAutosaveCopies"
                width: Theme.numberFieldWidth
                from: 1
                to: 10
                value: ProjectFile.autosaveCopies
                Accessible.name: qsTr("Autosave versions to keep")
                onValueModified: value => ProjectFile.autosaveCopies = Math.round(value)
            }
        }
        // 0.7 (card 006): the version history's automatic checkpoints (History; File > Version History… lists them).
        SettingsRow {
            rowKey: "versionInterval"
            label: qsTr("Keep versions every")
            description: qsTr("While you edit, Wabcut keeps a version of the project now and then (only when something changed), next to the saved file. File > Version History lists them; Off keeps only the versions you save.")
            PillTabs {
                objectName: "versionIntervalTabs"
                compact: true
                model: [qsTr("Off"), qsTr("2 min"), qsTr("5 min"), qsTr("10 min"), qsTr("30 min")]
                currentIndex: page.versionChoices.indexOf(History.autoIntervalSeconds)
                Accessible.name: qsTr("Keep versions every")
                onActivated: index => {
                    History.autoIntervalSeconds = page.versionChoices[index]
                    currentIndex = Qt.binding(() => page.versionChoices.indexOf(History.autoIntervalSeconds))
                }
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

    // 0.7 (card #70): the folder a new export is written to unless the Export panel's file picker chooses another.
    SettingsSection {
        title: qsTr("Export")

        SettingsRow {
            objectName: "exportFolderRow"
            rowKey: "exportFolder"
            label: qsTr("Default export folder")
            description: ExportController.defaultFolderMode === "fixed" && ExportController.defaultFolder === ""
                         ? qsTr("Choose the folder every export goes to.")
                         //: Settings > General > Export; %1 = the folder a new export goes to now
                         : qsTr("New exports go to %1 unless you pick another place.").arg(page.exportFolderNow)
            PillTabs {
                objectName: "exportFolderModeTabs"
                width: Theme.space(120)
                compact: true
                model: [qsTr("Last used"), qsTr("Next to the project"), qsTr("Next to the media"), qsTr("A folder")]
                currentIndex: Math.max(0, page.exportFolderModes.indexOf(ExportController.defaultFolderMode))
                Accessible.name: qsTr("Default export folder")
                onActivated: index => {
                    ExportController.defaultFolderMode = page.exportFolderModes[index]
                    currentIndex = Qt.binding(() => Math.max(0, page.exportFolderModes.indexOf(ExportController.defaultFolderMode)))
                    if (page.exportFolderModes[index] === "fixed" && ExportController.defaultFolder === "")
                        exportFolderDialog.open()
                }
            }
            PillButton {
                objectName: "exportFolderBrowse"
                compact: true
                iconName: "folder-open"
                visible: ExportController.defaultFolderMode === "fixed"
                text: qsTr("Browse…")
                tooltip: qsTr("Choose the folder every export goes to")
                Accessible.name: qsTr("Choose the default export folder")
                onClicked: exportFolderDialog.open()
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

    // P0 round: opt-in update checks (Updates; docs/release.md "Updates"). Off by default: Wabcut asks the internet nothing
    // until the user turns this on or checks by hand.
    SettingsSection {
        title: qsTr("Updates")

        SettingsRow {
            rowKey: "autoUpdate"
            label: qsTr("Check for updates automatically")
            description: qsTr("Once a day Wabcut asks GitHub whether a newer version is out, and says so. Nothing about you or your projects is sent. Nothing installs without your OK.")
            Toggle {
                objectName: "autoUpdateToggle"
                tooltip: qsTr("Check for updates automatically")
                checked: Updates.automatic
                onToggled: {
                    Updates.automatic = checked
                    checked = Qt.binding(() => Updates.automatic)
                }
            }
        }
        SettingsRow {
            objectName: "checkUpdatesRow"
            rowKey: "checkUpdates"
            //: %1 is this version, e.g. 0.4.0
            label: qsTr("Wabcut %1").arg(Updates.currentVersion)
            description: Updates.lastChecked !== ""
                         //: %1 is a date and time
                         ? qsTr("Last checked %1.").arg(Updates.lastChecked)
                         : qsTr("Not checked for updates yet.")
            PillButton {
                objectName: "checkUpdatesButton"
                compact: true
                iconName: "download"
                text: qsTr("Check for Updates…")
                tooltip: qsTr("Ask for the latest Wabcut release now")
                onClicked: page.actionRequested("updates") // the dialog opens once Settings has closed
            }
        }
    }

    // 0.7 (card 028): export and import all settings as one file; reset to the defaults. Both back the current settings
    // up first and restart Wabcut to apply the change (AppSettings; SettingsBackupDialog).
    SettingsSection {
        objectName: "settingsBackupSection"
        rowKey: "settingsBackup"
        title: qsTr("Backup and reset")

        SettingsRow {
            objectName: "exportSettingsRow"
            label: qsTr("Export settings")
            description: qsTr("Saves your settings, keyboard shortcuts, export and effect presets and graphic templates to one file. API keys are never included: they stay in the system keychain.")
            PillButton {
                objectName: "exportSettingsButton"
                compact: true
                iconName: "download"
                text: qsTr("Export…")
                tooltip: qsTr("Save all settings to a file")
                Accessible.name: qsTr("Export settings")
                onClicked: exportSettingsDialog.open()
            }
        }
        SettingsRow {
            objectName: "importSettingsRow"
            label: qsTr("Import settings")
            description: qsTr("Replaces your settings with a file made by Export settings. Wabcut shows what the file holds first, saves a backup of your settings, and restarts.")
            PillButton {
                objectName: "importSettingsButton"
                compact: true
                iconName: "folder-open"
                text: qsTr("Import…")
                tooltip: qsTr("Load settings from a file")
                Accessible.name: qsTr("Import settings")
                onClicked: importSettingsDialog.open()
            }
        }
        SettingsRow {
            objectName: "resetSettingsRow"
            label: qsTr("Reset all settings")
            description: qsTr("Starts over from the default settings, after saving a backup of the current ones. Projects, media and API keys are not touched.")
            PillButton {
                objectName: "resetSettingsButton"
                compact: true
                text: qsTr("Reset…")
                tooltip: qsTr("Go back to the default settings")
                Accessible.name: qsTr("Reset all settings")
                onClicked: settingsBackupDialog.openReset()
            }
        }
        SettingsRow {
            objectName: "settingsBackupStatusRow"
            visible: page.backupStatus !== ""
            label: page.backupStatus
            description: page.backupStatusDetail
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
        id: exportFolderDialog
        title: qsTr("Default export folder")
        currentFolder: ExportController.defaultFolderUrl.toString() !== "" ? ExportController.defaultFolderUrl : ExportController.lastFolderUrl
        onAccepted: ExportController.defaultFolder = selectedFolder.toString()
    }

    // What the last export said ("Saved 120 settings and 3 files"), under the Backup and reset rows.
    property string backupStatus: ""
    property string backupStatusDetail: ""

    FileDialog {
        id: exportSettingsDialog
        title: qsTr("Export settings")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("Wabcut settings (*.json)")]
        onAccepted: {
            const result = AppSettings.exportSettings(selectedFile)
            page.backupStatus = result.ok ? qsTr("Settings exported") : qsTr("Settings could not be exported")
            page.backupStatusDetail = result.ok
                ? qsTr("%n setting(s) and %1 saved to %2.", "", result.settings).arg(qsTr("%n file(s)", "", result.files)).arg(page.displayPath(selectedFile))
                : result.error
        }
    }
    FileDialog {
        id: importSettingsDialog
        title: qsTr("Import settings")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Wabcut settings (*.json)"), qsTr("All files (*)")]
        onAccepted: settingsBackupDialog.openImport(AppSettings.previewImport(selectedFile), selectedFile)
    }
    SettingsBackupDialog {
        id: settingsBackupDialog
        // Staged: Wabcut closes through the normal path (unsaved changes first) and the new Wabcut starts with the change.
        onConfirmed: AppShell.requestRestart()
    }

    FolderDialog {
        id: backupFolderDialog
        title: qsTr("Folder for autosaves and backups")
        currentFolder: ProjectFile.backupFolder.toString() !== "" ? ProjectFile.backupFolder : Home.defaultFolder
        onAccepted: ProjectFile.backupFolder = selectedFolder
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Default location for new projects")
        currentFolder: Home.defaultFolder
        onAccepted: Home.defaultFolderSetting = selectedFolder
    }
}
