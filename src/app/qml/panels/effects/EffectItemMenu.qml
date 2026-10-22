pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQml.Models

// The Effects panel's right-click menus (0.2, docs/effects.md "Presets, custom bins and favourites"):
//   * on an effect, transition or preset: Add to / Remove from Favorites, Add to a custom bin (or a new one), Remove
//     from this custom bin, Preset Properties… and Delete Preset (the user's presets), Export Preset…;
//   * on a custom bin: Rename…, Delete Custom Bin; on one of the user's presets bins: Rename…, Delete (with its presets).
// Everything goes through EffectPresets (the effects.* commands).
Item {
    id: root

    /// The item: {id, name, kind, item ("effect:<id>" / "preset:<name>"), builtIn}.
    property var item: ({})
    /// The custom bin the item is shown in ("" elsewhere).
    property string customBin: ""
    property string presetsBin: ""
    readonly property bool isPreset: (item.kind ?? "") === "preset"
    readonly property bool userPreset: isPreset && item.builtIn === false
    readonly property bool favorite: EffectPresets.revision >= 0 && (item.item ?? "") !== "" && EffectPresets.isFavorite(item.item)
    signal status(string message)

    function report(error, done) {
        root.status(error !== "" ? error : done)
    }
    function popupForItem(target, x, y, entry, bin) {
        item = entry
        customBin = bin
        itemMenu.popup(target, x, y)
    }
    function popupForCustomBin(target, x, y, bin) {
        customBin = bin
        binMenu.popup(target, x, y)
    }
    function popupForPresetsBin(target, x, y, bin) {
        presetsBin = bin
        presetsBinMenu.popup(target, x, y)
    }

    ContextMenu {
        id: itemMenu
        objectName: "effectItemMenu"

        ContextMenuItem {
            objectName: "favoriteItem"
            text: root.favorite ? qsTr("Remove from Favorites") : qsTr("Add to Favorites")
            iconName: "bookmark"
            onTriggered: root.report(EffectPresets.setFavorite(root.item.item, !root.favorite),
                                     root.favorite ? qsTr("Removed %1 from Favorites").arg(root.item.name)
                                                   : qsTr("Added %1 to Favorites").arg(root.item.name))
        }
        ContextMenu {
            id: addToBin
            title: qsTr("Add to Custom Bin")

            ContextMenuItem {
                objectName: "newCustomBinWithItem"
                text: qsTr("New Custom Bin")
                iconName: "folder-plus"
                onTriggered: {
                    const name = EffectPresets.newCustomBin(root.item.item)
                    root.status(name !== "" ? qsTr("Made %1").arg(name) : qsTr("Could not make a custom bin"))
                }
            }
            Instantiator {
                model: EffectPresets.customBins
                delegate: ContextMenuItem {
                    required property string modelData
                    text: modelData
                    iconName: "folder"
                    onTriggered: root.report(EffectPresets.addToCustomBin(modelData, root.item.item),
                                             qsTr("Added %1 to %2").arg(root.item.name).arg(modelData))
                }
                onObjectAdded: (index, object) => addToBin.insertItem(index + 1, object)
                onObjectRemoved: (index, object) => addToBin.removeItem(object)
            }
        }
        ContextMenuItem {
            objectName: "removeFromCustomBin"
            visible: root.customBin !== ""
            height: visible ? implicitHeight : 0
            text: qsTr("Remove from %1").arg(root.customBin)
            iconName: "x"
            onTriggered: root.report(EffectPresets.deleteCustomItem({ customBin: root.customBin, item: root.item.item }),
                                     qsTr("Removed %1 from %2").arg(root.item.name).arg(root.customBin))
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
            onTriggered: propertiesDialog.openProperties(root.item.name)
        }
        ContextMenuItem {
            objectName: "exportPresetItem"
            visible: root.isPreset
            height: visible ? implicitHeight : 0
            text: qsTr("Export Preset…")
            iconName: "upload"
            onTriggered: exportDialog.open()
        }
        ContextMenuItem {
            objectName: "deletePresetItem"
            visible: root.userPreset
            height: visible ? implicitHeight : 0
            text: qsTr("Delete Preset")
            iconName: "trash-2"
            command: "effects.deleteCustomItem"
            onTriggered: root.report(EffectPresets.deleteCustomItem({ preset: root.item.name }),
                                     qsTr("Deleted %1").arg(root.item.name))
        }
    }

    ContextMenu {
        id: binMenu
        objectName: "customBinMenu"

        ContextMenuItem {
            text: qsTr("Rename…")
            iconName: "pencil"
            onTriggered: names.ask(qsTr("Rename custom bin"), root.customBin, qsTr("Rename"),
                                   text => EffectPresets.renameCustomBin(root.customBin, text))
        }
        ContextMenuItem {
            text: qsTr("Delete Custom Bin")
            iconName: "trash-2"
            command: "effects.deleteCustomItem"
            onTriggered: root.report(EffectPresets.deleteCustomItem({ customBin: root.customBin }),
                                     qsTr("Deleted %1").arg(root.customBin))
        }
    }

    ContextMenu {
        id: presetsBinMenu
        objectName: "presetsBinMenu"

        ContextMenuItem {
            text: qsTr("New Presets Bin Inside…")
            iconName: "folder-plus"
            onTriggered: names.ask(qsTr("New presets bin"), qsTr("Presets Bin"), qsTr("Make"),
                                   text => EffectPresets.newPresetsBin(root.presetsBin + "/" + text))
        }
        ContextMenuItem {
            text: qsTr("Rename…")
            iconName: "pencil"
            onTriggered: names.ask(qsTr("Rename presets bin"), root.presetsBin.split("/").pop(), qsTr("Rename"),
                                   text => EffectPresets.renamePresetsBin(root.presetsBin, text))
        }
        ContextMenuItem {
            text: qsTr("Delete Bin and Its Presets")
            iconName: "trash-2"
            onTriggered: root.report(EffectPresets.deleteCustomItem({ presetsBin: root.presetsBin }),
                                     qsTr("Deleted %1").arg(root.presetsBin))
        }
    }

    SavePresetDialog {
        id: propertiesDialog
    }
    EffectNameDialog {
        id: names
    }
    FileDialog {
        id: exportDialog
        title: qsTr("Export Preset")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("Wabcut presets (*.wabcut-presets.json)")]
        onAccepted: root.report(EffectPresets.exportPresets(selectedFile, [root.item.name]),
                                qsTr("Exported %1").arg(root.item.name))
    }
}
