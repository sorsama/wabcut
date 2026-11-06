pragma ComponentBehavior: Bound
import QtQuick

// File (the menu bar, Premiere's order; the logo opens it too): Home, New, Open (Open Recent: Home's recent projects),
// Close Project, Save / Save As / Save a Copy / Revert, Import (0.4: from the Media Browser, files, a folder as bins),
// Export, the project's media (relink, recover, collect)
// and Exit. Rows with a keymap command run it (Main.runCommand); the others ask the window (AppMenuBar.actionRequested:
// Main.menuAction), once the menu has closed.
MenuBarMenu {
    id: root
    objectName: "fileMenu"
    title: qsTr("File")
    key: "file"
    mnemonic: "F"

    BarMenuItem {
        objectName: "fileMenuHome"
        text: qsTr("Home")
        onTriggered: root.request("home", null)
    }
    ContextMenuSeparator {}
    BarMenuItem {
        objectName: "fileMenuNewProject"
        text: qsTr("New Project…")
        command: "file.newProject"
    }
    BarMenuItem {
        text: qsTr("New Sequence…")
        command: "file.newSequence"
    }
    // P.2 (E5): Premiere's File > New > Sequence From Clip.
    BarMenuItem {
        objectName: "newSequenceFromClipMenuItem"
        text: qsTr("New Sequence From Clip")
        command: "file.newSequenceFromClip"
        available: Library.mediaSelected
    }
    // 0.7 (card 055): with several clips selected in the Library, one sequence each, or all in one.
    BarMenuItem {
        objectName: "newSequencePerClipMenuItem"
        text: qsTr("New Sequence per Clip")
        command: "file.newSequencesFromClips"
        available: Library.mediaSelected && Library.selectionCount > 1
    }
    BarMenuItem {
        objectName: "newSequenceFromAllClipsMenuItem"
        text: qsTr("New Sequence From All Selected Clips")
        command: "file.newSequenceFromClipsTogether"
        available: Library.mediaSelected && Library.selectionCount > 1
    }
    ContextMenuSeparator {}
    BarMenuItem {
        objectName: "fileMenuOpen"
        text: qsTr("Open Project…")
        command: "file.openProject"
    }
    // The recent projects (Home's list, newest first; a missing one greyed out) and Clear Recent.
    MenuBarMenu {
        id: recentMenu
        objectName: "fileMenuRecent"
        title: qsTr("Open Recent")
        nested: true
        bar: root.bar
        readonly property var entries: Home.recent.slice(0, 10)
        onAboutToShow: Home.refreshRecent()

        Instantiator {
            model: recentMenu.entries
            delegate: BarMenuItem {
                required property var modelData
                required property int index
                objectName: "fileMenuRecent_" + index
                text: modelData.name
                // Where it is: its folder's name, in the shortcut's place.
                shortcut: String(modelData.folder ?? "").split(/[\\/]/).filter(s => s !== "").pop() ?? ""
                available: modelData.missing !== true
                Accessible.description: modelData.missing === true ? qsTr("The file is missing") : String(modelData.folder ?? "")
                onTriggered: recentMenu.request("openRecent", modelData.url)
            }
            onObjectAdded: (index, object) => recentMenu.insertItem(index, object as BarMenuItem)
            onObjectRemoved: (index, object) => recentMenu.removeItem(object as BarMenuItem)
        }
        BarMenuItem {
            objectName: "fileMenuRecentNone"
            text: qsTr("No Recent Projects")
            visible: recentMenu.entries.length === 0
            height: visible ? implicitHeight : 0
            available: false
        }
        ContextMenuSeparator {}
        BarMenuItem {
            objectName: "fileMenuClearRecent"
            text: qsTr("Clear Recent")
            available: recentMenu.entries.length > 0
            onTriggered: recentMenu.run(() => Home.clearRecent())
        }
    }
    // 0.2 (P.3): several projects open at once (the Production panel lists them and switches between them).
    BarMenuItem {
        objectName: "fileMenuOpenAlongside"
        text: qsTr("Open Project Next to This One…")
        onTriggered: root.request("openAlongside", null)
    }
    ContextMenuSeparator {}
    // Premiere's File > Close: the panel the group you clicked last shows (Main.runCommand "file.close").
    BarMenuItem {
        objectName: "fileMenuClosePanel"
        text: qsTr("Close Panel")
        command: "file.close"
    }
    // With one project open too: it closes (after the unsaved-changes prompt) and Home shows, the demo behind it.
    BarMenuItem {
        objectName: "fileMenuCloseProject"
        text: qsTr("Close Project")
        command: "file.closeProject"
    }
    BarMenuItem {
        objectName: "fileMenuSave"
        text: qsTr("Save")
        command: "file.save"
    }
    BarMenuItem {
        text: qsTr("Save As…")
        command: "file.saveAs"
    }
    BarMenuItem {
        objectName: "fileMenuSaveCopy"
        text: qsTr("Save a Copy…")
        command: "file.saveCopy"
    }
    // 0.7 (card 059): a copy an older Wabcut opens (what it cannot hold is left out and listed).
    BarMenuItem {
        objectName: "fileMenuSaveForOlderVersion"
        text: qsTr("Save for an Older Version…")
        command: "file.saveForOlderVersion"
    }
    // 0.7 (card 006): a named version of the project's history, and that history (Review > Versions: restore, compare).
    // The history lives next to the saved project file: an untitled project has none.
    BarMenuItem {
        objectName: "fileMenuSaveVersion"
        text: qsTr("Save Version…")
        command: "file.saveVersion"
        available: History.available
    }
    BarMenuItem {
        objectName: "fileMenuVersionHistory"
        text: qsTr("Version History…")
        command: "file.versionHistory"
        available: History.available
    }
    // The project as last saved (after a confirmation): only a saved project with unsaved changes.
    BarMenuItem {
        objectName: "fileMenuRevert"
        text: qsTr("Revert")
        available: ProjectFile.dirty && !ProjectFile.untitled && !ProjectFile.demo
        onTriggered: root.request("revert", null)
    }
    ContextMenuSeparator {}
    // 0.4: Premiere's order - Import from Media Browser (Ctrl+Alt+I: imports the Media Browser's selection, else shows
    // it), Import… (into the bin the Library shows), Import Folder… (a folder as a bin, its subfolders nested).
    BarMenuItem {
        objectName: "fileMenuImportFromMediaBrowser"
        text: qsTr("Import from Media Browser")
        command: "file.importFromMediaBrowser"
    }
    BarMenuItem {
        objectName: "fileMenuImport"
        text: qsTr("Import…")
        command: "file.import"
    }
    BarMenuItem {
        objectName: "fileMenuImportFolder"
        text: qsTr("Import Folder…")
        command: "file.importFolder"
    }
    BarMenuItem {
        objectName: "importLottieMenuItem"
        text: qsTr("Import Lottie…")
        onTriggered: root.request("importLottie", null)
    }
    // 0.7 (card 078): record from a camera, a capture card or a tape deck the system exposes as one (Windows in this version).
    BarMenuItem {
        objectName: "fileMenuCapture"
        text: qsTr("Capture…")
        command: "file.capture"
        available: !ProjectFile.demo
    }
    // 0.7 (card 035): keep a project on storage you own, and share a review copy with a page that collects comments.
    BarMenuItem {
        objectName: "fileMenuCloudProject"
        text: qsTr("Cloud Project…")
        command: "file.cloudProject"
        available: !ProjectFile.demo
    }
    BarMenuItem {
        objectName: "fileMenuReviewLinks"
        text: qsTr("Share Review Link…")
        command: "file.reviewLinks"
        available: !ProjectFile.demo
    }
    MenuBarMenu {
        objectName: "fileMenuExport"
        title: qsTr("Export")
        nested: true
        bar: root.bar

        BarMenuItem {
            text: qsTr("Media…")
            command: "file.exportMedia"
        }
        BarMenuItem {
            text: qsTr("Frame…")
            command: "file.exportFrame"
        }
        BarMenuItem {
            text: qsTr("Send to Export Queue")
            command: "file.sendToMediaEncoder"
        }
        ContextMenuSeparator {}
        // 0.1.1: Premiere's File > Export > Final Cut Pro XML / EDL / OTIO (interchange.export, on a worker).
        BarMenuItem {
            objectName: "exportTimelineMenuItem"
            text: qsTr("Timeline (XML, EDL, OTIO)…")
            available: ProjectController.sequenceNames.length > 0
            onTriggered: root.request("exportTimeline", null)
        }
        // P1 (#25, 2C): every source the sequence uses, with record and source timecodes (clearances, credits).
        BarMenuItem {
            objectName: "mediaUsageReportMenuItem"
            text: qsTr("Media Usage Report…")
            available: ProjectController.sequenceNames.length > 0
            onTriggered: root.request("mediaUsageReport", null)
        }
        // 0.7 (card 179): the clearance log for archival footage, with the end-credits roll.
        BarMenuItem {
            objectName: "rightsLogMenuItem"
            text: qsTr("Rights and Clearance Log…")
            available: ProjectController.sequenceNames.length > 0
            onTriggered: root.request("rightsLog", null)
        }
    }
    // 0.6 (1D, #39): Premiere's File > Get Properties for > Selection (Ctrl/Cmd+Shift+H): the Properties dialog.
    BarMenuItem {
        text: qsTr("Get Properties for Selection…")
        command: "file.getProperties"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        objectName: "relinkMenuItem"
        text: qsTr("Relink Offline Media…")
        available: ProjectFile.offlineCount > 0
        onTriggered: root.request("relink", null)
    }
    // I.4: every saved/autosaved/backup version (the recovery dashboard), and the portable project capsule.
    BarMenuItem {
        objectName: "recoverMenuItem"
        text: qsTr("Recover…")
        onTriggered: root.request("recover", null)
    }
    BarMenuItem {
        objectName: "collectFilesMenuItem"
        text: qsTr("Collect Files…")
        available: !ProjectFile.demo
        onTriggered: root.request("collectFiles", null)
    }
    // 0.7 (card 050): a Python script run against the open project (docs/python.md).
    BarMenuItem {
        objectName: "runScriptMenuItem"
        text: qsTr("Run Python Script…")
        onTriggered: root.request("runScript", null)
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Exit")
        command: "file.exit"
    }
}
