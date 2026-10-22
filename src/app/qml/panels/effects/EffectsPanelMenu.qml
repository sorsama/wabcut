pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs

// The Effects panel's menu (0.2): Premiere's New Custom Bin (Ctrl+/), New Presets Bin, Import Presets… and Export
// Presets… (every preset of yours), through EffectPresets.
Item {
    id: root

    signal status(string message)

    function popupAt(target, x, y) {
        menu.popup(target, x, y)
    }
    function newCustomBin() {
        const name = EffectPresets.newCustomBin("")
        root.status(name !== "" ? qsTr("Made %1").arg(name) : qsTr("Could not make a custom bin"))
    }

    ContextMenu {
        id: menu
        objectName: "effectsPanelMenu"

        ContextMenuItem {
            objectName: "newCustomBinItem"
            text: qsTr("New Custom Bin")
            iconName: "folder-plus"
            command: "effects.newCustomBin"
            onTriggered: root.newCustomBin()
        }
        ContextMenuItem {
            objectName: "newPresetsBinItem"
            text: qsTr("New Presets Bin…")
            iconName: "folder-plus"
            onTriggered: names.ask(qsTr("New presets bin"), qsTr("Presets Bin"), qsTr("Make"),
                                   text => EffectPresets.newPresetsBin(text))
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            objectName: "importPresetsItem"
            text: qsTr("Import Presets…")
            iconName: "import"
            onTriggered: importDialog.open()
        }
        ContextMenuItem {
            objectName: "exportPresetsItem"
            text: qsTr("Export Presets…")
            iconName: "upload"
            onTriggered: exportDialog.open()
        }
    }

    EffectNameDialog {
        id: names
    }
    FileDialog {
        id: importDialog
        title: qsTr("Import Presets")
        nameFilters: [qsTr("Wabcut presets (*.wabcut-presets.json)"), qsTr("JSON (*.json)")]
        onAccepted: {
            const error = EffectPresets.importPresets(selectedFile)
            root.status(error !== "" ? error : qsTr("Imported the presets"))
        }
    }
    FileDialog {
        id: exportDialog
        title: qsTr("Export Presets")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("Wabcut presets (*.wabcut-presets.json)")]
        onAccepted: {
            const error = EffectPresets.exportPresets(selectedFile, [])
            root.status(error !== "" ? error : qsTr("Exported your presets"))
        }
    }
}
