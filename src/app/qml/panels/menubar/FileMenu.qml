pragma ComponentBehavior: Bound
import QtQuick

// File (the menu bar, Premiere's order; the logo opens it too): Home, New, Open (Open Recent: Home's recent projects),
// Close Project, Save / Save As / Save a Copy / Revert, Import, Export, the project's media (relink, recover, collect)
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
    // The project as last saved (after a confirmation): only a saved project with unsaved changes.
    BarMenuItem {
        objectName: "fileMenuRevert"
        text: qsTr("Revert")
        available: ProjectFile.dirty && !ProjectFile.untitled && !ProjectFile.demo
        onTriggered: root.request("revert", null)
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Import…")
        command: "file.import"
    }
    BarMenuItem {
        objectName: "importLottieMenuItem"
        text: qsTr("Import Lottie…")
        onTriggered: root.request("importLottie", null)
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
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Exit")
        command: "file.exit"
    }
}
