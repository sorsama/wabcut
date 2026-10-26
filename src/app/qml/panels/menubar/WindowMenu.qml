pragma ComponentBehavior: Bound
import QtQuick

// Window (the menu bar, Premiere's order): Workspaces (switch, new from a preset, duplicate, rename, delete), Maximize
// Frame, Reset Panel Layout, History, then every panel of the catalog (Workspaces.catalog), checked while it is open in
// this workspace. Choosing a panel opens it where you clicked last (a closed one joins that group as a tab) or brings it
// forward where it is, and focuses it (Main.openPanelFromWindowMenu). Generate is not listed while AI is turned off
// (Settings > AI).
MenuBarMenu {
    id: root
    objectName: "windowMenu"
    title: qsTr("Window")
    key: "window"
    mnemonic: "W"

    /// The docking of the editor's work area (which panels are open, the maximized frame).
    property DockManager docking: null
    /// The current workspace's id.
    property int workspace: 0
    /// The panels' focus keys (Premiere's Window shortcuts), shown on their rows.
    readonly property var panelCommands: ({ library: "window.projects", mediaBrowser: "window.mediaBrowser",
                                             effects: "window.effects", effectControls: "window.effectControls",
                                             source: "window.sourceMonitor", program: "window.programMonitor",
                                             timeline: "window.timeline", mixer: "window.audioTrackMixer",
                                             audioMeters: "window.audioMeters" })
    /// The catalog's panels the menu lists (AI off: no Generate).
    readonly property var panels: Workspaces.catalog.filter(p => p.key !== "generate" || Preferences.aiEnabled)
    /// Where the panels' rows start: after the separator that ends the fixed rows.
    function panelsAt(): int {
        for (let i = 0; i < root.count; ++i) {
            if (root.itemAt(i) === panelsSeparator)
                return i + 1
        }
        return root.count
    }

    MenuBarMenu {
        id: workspacesMenu
        objectName: "windowMenuWorkspaces"
        title: qsTr("Workspaces")
        nested: true
        bar: root.bar

        // The workspaces, in tab order; the current one checked.
        Instantiator {
            model: Workspaces.all
            delegate: BarMenuItem {
                required property var modelData
                objectName: "windowMenuWorkspace_" + Workspaces.keyOf(modelData.id)
                text: modelData.name
                command: Workspaces.commandOf(modelData.id)
                runsCommand: false
                checkable: true
                checked: modelData.id === root.workspace
                onTriggered: {
                    checked = Qt.binding(() => modelData.id === root.workspace)
                    workspacesMenu.request("workspace", modelData.id)
                }
            }
            onObjectAdded: (index, object) => workspacesMenu.insertItem(index, object as BarMenuItem)
            onObjectRemoved: (index, object) => workspacesMenu.removeItem(object as BarMenuItem)
        }
        ContextMenuSeparator {}
        MenuBarMenu {
            id: newWorkspaceMenu
            objectName: "windowMenuNewWorkspace"
            title: qsTr("New Workspace")
            nested: true
            bar: root.bar

            Instantiator {
                model: Workspaces.presetOrder
                delegate: BarMenuItem {
                    required property string modelData
                    objectName: "windowMenuNewWorkspace_" + modelData
                    text: qsTr("From the %1 Preset").arg(Workspaces.presetLabel(modelData))
                    onTriggered: newWorkspaceMenu.request("newWorkspace", modelData)
                }
                onObjectAdded: (index, object) => newWorkspaceMenu.insertItem(index, object as BarMenuItem)
                onObjectRemoved: (index, object) => newWorkspaceMenu.removeItem(object as BarMenuItem)
            }
        }
        BarMenuItem {
            objectName: "windowMenuDuplicateWorkspace"
            text: qsTr("Duplicate Workspace")
            onTriggered: workspacesMenu.request("duplicateWorkspace", root.workspace)
        }
        BarMenuItem {
            objectName: "windowMenuRenameWorkspace"
            text: qsTr("Rename Workspace…")
            onTriggered: workspacesMenu.request("renameWorkspace", root.workspace)
        }
        // The last workspace stays.
        BarMenuItem {
            objectName: "windowMenuDeleteWorkspace"
            text: qsTr("Delete Workspace…")
            available: Workspaces.all.length > 1
            onTriggered: workspacesMenu.request("deleteWorkspace", root.workspace)
        }
    }
    // The group you clicked last takes the whole work area; again (or Shift+`) gives the layout back.
    BarMenuItem {
        objectName: "windowMenuMaximizeFrame"
        text: qsTr("Maximize Frame")
        command: "window.maximizeFrame"
        checkable: true
        checked: (root.docking?.maximizedGroup ?? "") !== ""
        onTriggered: checked = Qt.binding(() => (root.docking?.maximizedGroup ?? "") !== "")
    }
    // The current workspace's panels back where its preset puts them (Premiere's Reset to Saved Layout; the toast offers
    // Undo).
    BarMenuItem {
        objectName: "resetPanelLayoutMenuItem"
        text: qsTr("Reset Panel Layout")
        command: "window.resetToSavedLayout"
    }
    // P.2 (E5): the History popup (not a docked panel).
    BarMenuItem {
        objectName: "historyMenuItem"
        text: qsTr("History")
        command: "window.history"
    }
    ContextMenuSeparator {
        id: panelsSeparator
    }

    // Every panel: checked while it is open in this workspace.
    Instantiator {
        model: root.panels
        delegate: BarMenuItem {
            required property var modelData
            objectName: "windowMenu_" + modelData.key
            text: modelData.label
            command: root.panelCommands[modelData.key] ?? ""
            runsCommand: false
            checkable: true
            checked: root.docking?.isOpen(modelData.key) ?? false
            onTriggered: {
                checked = Qt.binding(() => root.docking?.isOpen(modelData.key) ?? false)
                // The panel takes the focus itself: the focus from before the menu is not put back first.
                root.runMovingFocus(() => root.menuBar?.actionRequested("openPanel", modelData.key))
            }
        }
        onObjectAdded: (index, object) => root.insertItem(root.panelsAt() + index, object as BarMenuItem)
        onObjectRemoved: (index, object) => root.removeItem(object as BarMenuItem)
    }
}
