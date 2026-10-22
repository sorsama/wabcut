pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Premiere's Preset Manager (0.2): Save Preset (the Export workspace's current preset with its settings on top, under a
// name of your own), the user's presets with Export Preset… and Delete, and Import Preset… for a preset file someone
// shared. Presets are Wabcut's own JSON files ("Export Presets" in the user's config folder), checked like any
// untrusted file when they are read.
WabcutDialog {
    id: dialog
    objectName: "exportPresetManager"

    /// What Save Preset stores: the Export workspace's preset and its overrides.
    property string basePreset: "match-source"
    property var baseOverrides: ({})
    property string message: ""
    property bool failed: false
    property string exportingId: ""
    readonly property var userPresets: ExportController.presets.filter(p => p.user === true)

    signal presetSaved(string id)

    function openWith(presetId, overrides) {
        basePreset = presetId
        baseOverrides = overrides
        message = ""
        failed = false
        nameField.text = ""
        descriptionField.text = ""
        open()
    }
    function report(ok, text) {
        failed = !ok
        message = text
    }

    title: qsTr("Export Presets")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(140), Theme.space(140))

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    GroupLabel { text: qsTr("Save Preset") }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        FieldInput {
            id: nameField
            objectName: "exportPresetName"
            Layout.fillWidth: true
            placeholderText: qsTr("Preset name")
            Accessible.name: qsTr("Preset name")
        }
        FieldInput {
            id: descriptionField
            Layout.fillWidth: true
            placeholderText: qsTr("Description (optional)")
            Accessible.name: qsTr("Preset description")
        }
        PillButton {
            objectName: "exportPresetSave"
            variant: "primary"
            compact: true
            iconName: "save"
            text: qsTr("Save")
            enabled: nameField.text.trim() !== ""
            onClicked: {
                const replacing = dialog.userPresets.some(p => p.name === nameField.text.trim())
                const r = ExportController.savePreset(nameField.text, descriptionField.text, dialog.basePreset,
                                                      dialog.baseOverrides, replacing)
                dialog.report(r.ok, r.ok ? qsTr("Saved \"%1\".").arg(r.name) : r.error)
                if (r.ok)
                    dialog.presetSaved(r.id)
            }
        }
    }

    GroupLabel { text: qsTr("Your presets") }
    Text {
        Layout.fillWidth: true
        visible: dialog.userPresets.length === 0
        text: qsTr("No presets of your own yet. Save the current settings, or import a preset file.")
        wrapMode: Text.Wrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    Repeater {
        objectName: "exportUserPresets"
        model: dialog.userPresets
        delegate: RowLayout {
            id: presetRow
            required property var modelData
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Icon {
                name: presetRow.modelData.icon
                size: Theme.iconSm
                color: Theme.textSecondary
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: presetRow.modelData.name
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: presetRow.modelData.description
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
            }
            PillButton {
                compact: true
                variant: "ghost"
                text: qsTr("Export…")
                onClicked: {
                    dialog.exportingId = presetRow.modelData.id
                    exportDialog.open()
                }
            }
            IconButton {
                objectName: "exportPresetDelete_" + presetRow.modelData.id
                compact: true
                iconName: "trash-2"
                tooltip: qsTr("Delete preset")
                onClicked: dialog.report(ExportController.deletePreset(presetRow.modelData.id),
                                         ExportController.lastError)
            }
        }
    }

    Text {
        objectName: "exportPresetMessage"
        Layout.fillWidth: true
        visible: dialog.message !== ""
        text: dialog.message
        wrapMode: Text.Wrap
        color: dialog.failed ? Theme.danger : Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    FileDialog {
        id: importDialog
        title: qsTr("Import Preset")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Wabcut export presets (*.json)")]
        onAccepted: {
            const r = ExportController.importPreset(selectedFile.toString(), false)
            dialog.report(r.ok, r.ok ? qsTr("Imported \"%1\".").arg(r.name) : r.error)
        }
    }
    FileDialog {
        id: exportDialog
        title: qsTr("Export Preset")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("Wabcut export presets (*.json)")]
        onAccepted: {
            const ok = ExportController.exportPreset(dialog.exportingId, selectedFile.toString())
            dialog.report(ok, ok ? qsTr("Preset written.") : ExportController.lastError)
        }
    }

    footerData: [
        PillButton {
            objectName: "exportPresetImport"
            variant: "secondary"
            iconName: "download"
            text: qsTr("Import Preset…")
            onClicked: importDialog.open()
        },
        PillButton {
            variant: "ghost"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
