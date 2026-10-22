pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The Export workspace's other presets (0.2): the user's own (Save Preset / Import Preset, with Manage… opening the
// preset manager) and Premiere's formats - alpha masters (ProRes 4444, QuickTime Animation, CineForm), DNxHR / DNxHD,
// audio-only files (MP3, AAC, AIFF) and image sequences (PNG, TIFF, DPX, JPEG, Targa, OpenEXR) - folded away until
// opened, so the everyday presets above stay in view.
ColumnLayout {
    id: root

    property string selected: ""
    property bool formatsOpen: false

    signal chosen(string id)
    signal manageRequested()

    readonly property var userPresets: ExportController.presets.filter(p => p.category === "user")
    readonly property var formatPresets: ExportController.presets.filter(p => p.category === "formats")

    spacing: Theme.space(3)

    RowLayout {
        Layout.fillWidth: true
        SectionHeader {
            Layout.fillWidth: true
            text: qsTr("My presets")
        }
        PillButton {
            objectName: "exportManagePresets"
            compact: true
            variant: "ghost"
            iconName: "settings"
            text: qsTr("Manage…")
            tooltip: qsTr("Save Preset, Import Preset, Export Preset, Delete")
            onClicked: root.manageRequested()
        }
    }
    Text {
        Layout.fillWidth: true
        visible: root.userPresets.length === 0
        text: qsTr("Save the settings below as a preset of your own with Manage….")
        wrapMode: Text.Wrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    GridLayout {
        Layout.fillWidth: true
        visible: root.userPresets.length > 0
        columns: 2
        columnSpacing: Theme.space(2)
        rowSpacing: Theme.space(2)
        Repeater {
            model: root.userPresets
            delegate: PresetCard {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                text: modelData.name
                description: modelData.description
                iconName: modelData.icon
                selected: root.selected === modelData.id
                objectName: "exportPreset_" + modelData.id
                onClicked: root.chosen(modelData.id)
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        SectionHeader {
            Layout.fillWidth: true
            text: qsTr("Formats")
        }
        PillButton {
            objectName: "exportFormatsToggle"
            compact: true
            variant: "ghost"
            iconName: root.formatsOpen ? "chevron-up" : "chevron-down"
            text: root.formatsOpen ? qsTr("Fewer") : qsTr("Alpha, audio, image sequences…")
            onClicked: root.formatsOpen = !root.formatsOpen
        }
    }
    GridLayout {
        Layout.fillWidth: true
        visible: root.formatsOpen || root.formatPresets.some(p => p.id === root.selected)
        columns: 2
        columnSpacing: Theme.space(2)
        rowSpacing: Theme.space(2)
        Repeater {
            model: root.formatPresets
            delegate: PresetCard {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                text: modelData.name
                description: modelData.description
                iconName: modelData.icon
                selected: root.selected === modelData.id
                objectName: "exportPreset_" + modelData.id
                onClicked: root.chosen(modelData.id)
            }
        }
    }
}
