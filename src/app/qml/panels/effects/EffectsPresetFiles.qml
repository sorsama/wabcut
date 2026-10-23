pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs

// The Effects panel's presets files (UX rework, Premiere's Import Presets / Export Presets): Import Presets… (into a
// presets bin, or each preset back in the bin it was exported from), Export Presets… of one bin (or all of the user's
// presets) and Export Preset… of one preset. The dialogs open in the folder used last (EffectPresets.presetsFolder),
// name the file "<bin>.wabcut-presets.json" and add that suffix to a typed name. Outcomes go to `status`; `imported`
// carries the names the presets got, so the panel can show the first one.
Item {
    id: root

    /// Tests: prepare a dialog's target without showing the system dialog (they call acceptImport / acceptExport).
    property bool dialogsEnabled: true
    /// Import target: undefined = each preset keeps its bin, a string = that presets bin ("" = the Presets bin).
    property var importBin: undefined
    /// Export: one preset's name, or "" for a bin (`exportBinPath`, "" = all of the user's presets).
    property string exportPreset: ""
    property string exportBinPath: ""
    property string exportTitle: ""

    signal status(string message)
    signal imported(var names)

    /// Import Presets… into presets bin `bin` (a string; "" the Presets bin) or, with `undefined`, each preset's own.
    function importInto(bin: var) {
        root.importBin = bin
        if (root.dialogsEnabled)
            importDialog.open()
    }
    /// Export Presets… of presets bin `path` ("" = every preset of the user's), its file named after `title`.
    function exportBin(path: string, title: string) {
        root.exportPreset = ""
        root.exportBinPath = path
        root.exportTitle = title
        root.openExport(title !== "" ? title : qsTr("Presets"))
    }
    /// Export Preset… of preset `name`.
    function exportOne(name: string) {
        root.exportPreset = name
        root.exportBinPath = ""
        root.exportTitle = name
        root.openExport(name)
    }
    function openExport(fileTitle: string) {
        if (!root.dialogsEnabled)
            return
        exportDialog.currentFolder = EffectPresets.presetsFolder
        exportDialog.selectedFile = EffectPresets.presetsFileUrl(fileTitle)
        exportDialog.open()
    }

    /// The import dialog's outcome (also the tests' way in): every chosen file, into the prepared bin.
    function acceptImport(files: var) {
        const names = []
        const errors = []
        for (let i = 0; i < files.length; ++i) {
            const error = root.importBin === undefined ? EffectPresets.importPresets(files[i])
                                                        : EffectPresets.importPresets(files[i], root.importBin)
            if (error !== "") {
                errors.push(error)
                continue
            }
            const got = EffectPresets.lastImported
            for (let k = 0; k < got.length; ++k)
                names.push(got[k])
        }
        if (errors.length > 0)
            root.status(errors.join(" · "))
        else if (root.importBin !== undefined && root.importBin !== "")
            root.status(names.length === 1 ? qsTr("Imported 1 preset into %1").arg(String(root.importBin).split("/").pop())
                        : qsTr("Imported %1 presets into %2").arg(names.length).arg(String(root.importBin).split("/").pop()))
        else
            root.status(names.length === 1 ? qsTr("Imported 1 preset") : qsTr("Imported %1 presets").arg(names.length))
        if (names.length > 0)
            root.imported(names)
    }
    /// The export dialog's outcome (also the tests' way in).
    function acceptExport(file: url) {
        if (root.exportPreset !== "") {
            const error = EffectPresets.exportPresets(file, [root.exportPreset])
            root.status(error !== "" ? error : qsTr("Exported %1").arg(root.exportPreset))
            return
        }
        const error = EffectPresets.exportPresetsBin(file, root.exportBinPath)
        root.status(error !== "" ? error
                    : root.exportBinPath === "" ? (EffectPresets.userPresetCount === 1 ? qsTr("Exported your preset")
                                                   : qsTr("Exported your %1 presets").arg(EffectPresets.userPresetCount))
                    : qsTr("Exported %1").arg(root.exportTitle))
    }

    FileDialog {
        id: importDialog
        objectName: "effectsImportDialog"
        title: root.importBin !== undefined && root.importBin !== ""
               ? qsTr("Import Presets into %1").arg(String(root.importBin).split("/").pop()) : qsTr("Import Presets")
        fileMode: FileDialog.OpenFiles
        currentFolder: EffectPresets.presetsFolder
        nameFilters: [qsTr("Wabcut presets (*.wabcut-presets.json)"), qsTr("JSON files (*.json)")]
        onAccepted: root.acceptImport(importDialog.selectedFiles)
    }
    FileDialog {
        id: exportDialog
        objectName: "effectsExportDialog"
        title: root.exportPreset !== "" ? qsTr("Export Preset") : qsTr("Export Presets")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "wabcut-presets.json"
        nameFilters: [qsTr("Wabcut presets (*.wabcut-presets.json)")]
        onAccepted: root.acceptExport(exportDialog.selectedFile)
    }
}
