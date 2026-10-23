pragma ComponentBehavior: Bound
import QtQuick
import QtQml.Models

// The Effects panel's right-click menus (0.2; UX rework, docs/effects.md "The Effects panel"):
//   * on an effect, transition, preset or audio plug-in: Apply to Selected Clips, Set as Default Transition
//     (transitions), Add to / Remove from Favorites, Add to Custom Bin ▸ (or a new one), Remove from <bin> (a copy in a
//     custom bin), Preset Properties… (the user's presets), Export Preset… (presets), Delete… (the user's presets, not
//     their copies in custom bins);
//   * on a folder: New Presets Bin, Import Presets… and Export Presets… on the Presets folder and the user's presets
//     bins; Rename and Delete… on custom bins and the user's presets bins; the grouping, Rescan and the settings on Audio
//     Plug-ins; Expand All / Collapse All on every folder.
// Library changes go through EffectPresets (the effects.* commands). What needs the panel (a confirmation, an in-place
// rename, the file dialogs) is asked for with the signals, after the menu has closed.
Item {
    id: root

    /// The row the menu is for: an EffectsTree row map (rowType "folder" or "item").
    property var row: ({})
    /// The panel's tree (Expand All / Collapse All of a folder, Apply to Selected Clips).
    property EffectsTree tree: null

    readonly property string itemId: row.itemId ?? ""
    readonly property string itemRef: row.item ?? ""
    readonly property string itemName: row.name ?? ""
    readonly property string kind: row.kind ?? ""
    readonly property bool isPreset: kind === "preset"
    readonly property bool userPreset: isPreset && !(row.builtIn ?? true)
    readonly property bool isTransition: kind === "videoTransition" || kind === "audioTransition"
    readonly property bool isDefault: isTransition
                                      && (Editing.defaultVideoTransition === itemId || Editing.defaultAudioTransition === itemId)
    readonly property bool favorite: row.favorite ?? false
    readonly property bool usable: (row.problem ?? "") === ""
    readonly property string folderKind: row.folderKind ?? ""
    readonly property bool holdsPresets: row.canHoldPresets ?? false

    signal status(string message)
    signal renameRequested(string key)
    signal deleteRequested(string key)
    signal newCustomBinRequested(string item)
    signal newPresetsBinRequested(string parentPath)
    signal importRequested(string bin)
    signal exportBinRequested(string path, string title)
    signal exportPresetRequested(string name)

    function report(error: string, done: string) {
        root.status(error !== "" ? error : done)
    }
    function popupFor(entry: var, target: Item, x: real, y: real) {
        root.row = entry
        if ((entry.rowType ?? "") === "folder")
            folderMenu.popup(target, x, y)
        else
            itemMenu.popup(target, x, y)
    }

    // Runs after the menu has closed (a dialog or an in-place rename then keeps the focus).
    property var pending: null
    function later(action: var) {
        root.pending = action
    }
    function runPending() {
        const run = root.pending
        root.pending = null
        if (run)
            Qt.callLater(run)
    }

    ContextMenu {
        id: itemMenu
        objectName: "effectItemMenu"
        onClosed: root.runPending()

        ContextMenuItem {
            objectName: "applyToSelectedItem"
            text: qsTr("Apply to Selected Clips")
            iconName: "check"
            enabled: root.usable
            onTriggered: {
                const result = root.tree ? root.tree.applyToSelection(root.itemId) : ({})
                root.status(result.message ?? result.error ?? "")
            }
        }
        ContextMenuItem {
            objectName: "defaultTransitionItem"
            visible: root.isTransition
            height: visible ? implicitHeight : 0
            enabled: !root.isDefault
            text: root.isDefault ? qsTr("Default Transition") : qsTr("Set as Default Transition")
            iconName: root.isDefault ? "check" : "blend"
            onTriggered: root.report(Editing.setDefaultTransition(root.itemId) ? "" : qsTr("%1 cannot be the default transition").arg(root.itemName),
                                     root.kind === "audioTransition"
                                         ? qsTr("%1 is the default audio transition").arg(root.itemName)
                                         : qsTr("%1 is the default video transition").arg(root.itemName))
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            objectName: "favoriteItem"
            text: root.favorite ? qsTr("Remove from Favorites") : qsTr("Add to Favorites")
            iconName: "bookmark"
            onTriggered: {
                const adding = !root.favorite
                root.report(EffectPresets.setFavorite(root.itemRef, adding),
                            adding ? qsTr("Added %1 to Favorites").arg(root.itemName)
                                   : qsTr("Removed %1 from Favorites").arg(root.itemName))
            }
        }
        ContextMenu {
            id: addToBin
            title: qsTr("Add to Custom Bin")

            ContextMenuItem {
                objectName: "newCustomBinWithItem"
                text: qsTr("New Custom Bin")
                iconName: "folder-plus"
                onTriggered: {
                    const item = root.itemRef
                    root.later(() => root.newCustomBinRequested(item))
                }
            }
            Instantiator {
                model: EffectPresets.customBins
                delegate: ContextMenuItem {
                    required property string modelData
                    text: modelData
                    iconName: "folder"
                    onTriggered: root.report(EffectPresets.addToCustomBin(modelData, root.itemRef),
                                             qsTr("Added %1 to %2").arg(root.itemName).arg(modelData))
                }
                onObjectAdded: (index, object) => addToBin.insertItem(index + 1, object)
                onObjectRemoved: (index, object) => addToBin.removeItem(object)
            }
        }
        ContextMenuItem {
            objectName: "removeFromCustomBin"
            visible: root.row.removable ?? false
            height: visible ? implicitHeight : 0
            text: qsTr("Remove from %1").arg(root.row.customBin ?? "")
            iconName: "x"
            onTriggered: root.report(EffectPresets.deleteCustomItem({ customBin: root.row.customBin, item: root.itemRef }),
                                     qsTr("Removed %1 from %2").arg(root.itemName).arg(root.row.customBin))
        }
        ContextMenuSeparator {
            visible: root.isPreset
            height: visible ? implicitHeight : 0
        }
        ContextMenuItem {
            objectName: "presetPropertiesItem"
            visible: root.userPreset
            height: visible ? implicitHeight : 0
            text: qsTr("Preset Properties…")
            iconName: "pencil"
            onTriggered: {
                const name = root.itemName
                root.later(() => propertiesDialog.openProperties(name))
            }
        }
        ContextMenuItem {
            objectName: "exportPresetItem"
            visible: root.isPreset
            height: visible ? implicitHeight : 0
            text: qsTr("Export Preset…")
            iconName: "upload"
            onTriggered: {
                const name = root.itemName
                root.later(() => root.exportPresetRequested(name))
            }
        }
        // Not on a copy in a custom bin: there Delete (the key too) takes it out of the bin, which "Remove from" says.
        ContextMenuItem {
            objectName: "deletePresetItem"
            visible: root.userPreset && !(root.row.removable ?? false)
            height: visible ? implicitHeight : 0
            text: qsTr("Delete…")
            iconName: "trash-2"
            onTriggered: {
                const key = root.row.rowKey ?? ""
                root.later(() => root.deleteRequested(key))
            }
        }
    }

    ContextMenu {
        id: folderMenu
        objectName: "effectFolderMenu"
        onClosed: root.runPending()

        // The Presets folder and the user's presets bins.
        ContextMenuItem {
            objectName: "folderNewPresetsBin"
            visible: root.holdsPresets
            height: visible ? implicitHeight : 0
            text: (root.row.presetsBin ?? "") === "" ? qsTr("New Presets Bin") : qsTr("New Presets Bin Inside")
            iconName: "folder-plus"
            onTriggered: {
                const parentPath = root.row.presetsBin ?? ""
                root.later(() => root.newPresetsBinRequested(parentPath))
            }
        }
        ContextMenuItem {
            objectName: "folderImportPresets"
            visible: root.holdsPresets
            height: visible ? implicitHeight : 0
            text: qsTr("Import Presets…")
            iconName: "import"
            onTriggered: {
                const bin = root.row.presetsBin ?? ""
                root.later(() => root.importRequested(bin))
            }
        }
        ContextMenuItem {
            objectName: "folderExportPresets"
            visible: root.holdsPresets
            height: visible ? implicitHeight : 0
            enabled: (root.row.userPresets ?? 0) > 0
            text: qsTr("Export Presets…")
            iconName: "upload"
            onTriggered: {
                const path = root.row.presetsBin ?? ""
                const title = path === "" ? "" : (root.row.title ?? "")
                root.later(() => root.exportBinRequested(path, title))
            }
        }
        // Audio Plug-ins: the grouping of its folders, a rescan and the settings.
        ContextMenuItem {
            id: byVendor
            objectName: "folderGroupByVendor"
            visible: root.folderKind === "plugins"
            height: visible ? implicitHeight : 0
            text: qsTr("Group by Vendor")
            checkable: true
            checked: Plugins.groupBy !== "category"
            onTriggered: {
                Plugins.groupBy = "vendor"
                byVendor.checked = Qt.binding(() => Plugins.groupBy !== "category")
            }
        }
        ContextMenuItem {
            id: byCategory
            objectName: "folderGroupByCategory"
            visible: root.folderKind === "plugins"
            height: visible ? implicitHeight : 0
            text: qsTr("Group by Category")
            checkable: true
            checked: Plugins.groupBy === "category"
            onTriggered: {
                Plugins.groupBy = "category"
                byCategory.checked = Qt.binding(() => Plugins.groupBy === "category")
            }
        }
        ContextMenuItem {
            objectName: "folderRescanPlugins"
            visible: root.folderKind === "plugins"
            height: visible ? implicitHeight : 0
            enabled: !Plugins.scanning
            text: qsTr("Rescan Audio Plug-ins")
            iconName: "rotate-cw"
            onTriggered: {
                const message = Plugins.rescan()
                root.status(message !== "" ? message : qsTr("Scanning the audio plug-ins"))
            }
        }
        ContextMenuItem {
            objectName: "folderPluginSettings"
            visible: root.folderKind === "plugins"
            height: visible ? implicitHeight : 0
            text: qsTr("Audio Plug-in Settings…")
            iconName: "settings"
            onTriggered: root.later(() => Plugins.openSettings())
        }
        ContextMenuSeparator {
            visible: root.holdsPresets || root.folderKind === "plugins"
            height: visible ? implicitHeight : 0
        }
        // Custom bins and the user's presets bins.
        ContextMenuItem {
            objectName: "folderRename"
            visible: root.row.renamable ?? false
            height: visible ? implicitHeight : 0
            text: qsTr("Rename")
            iconName: "pencil"
            onTriggered: {
                const key = root.row.rowKey ?? ""
                root.later(() => root.renameRequested(key))
            }
        }
        ContextMenuItem {
            objectName: "folderDelete"
            visible: root.row.deletable ?? false
            height: visible ? implicitHeight : 0
            text: qsTr("Delete…")
            iconName: "trash-2"
            onTriggered: {
                const key = root.row.rowKey ?? ""
                root.later(() => root.deleteRequested(key))
            }
        }
        ContextMenuSeparator {
            visible: (root.row.renamable ?? false) || (root.row.deletable ?? false)
            height: visible ? implicitHeight : 0
        }
        ContextMenuItem {
            objectName: "folderExpandAll"
            text: qsTr("Expand All")
            iconName: "unfold-vertical"
            onTriggered: root.tree?.expandAll(root.row.rowKey ?? "")
        }
        ContextMenuItem {
            objectName: "folderCollapseAll"
            text: qsTr("Collapse All")
            iconName: "fold-vertical"
            onTriggered: root.tree?.collapseAll(root.row.rowKey ?? "")
        }
    }

    SavePresetDialog {
        id: propertiesDialog
    }
}
