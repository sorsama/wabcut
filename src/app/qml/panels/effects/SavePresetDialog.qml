pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Save Preset (Effect Controls, 0.2, docs/effects.md): Premiere's dialog - a name, the preset type (Scale, Anchor to In
// Point, Anchor to Out Point: how its keyframes land on a clip of another length), a description, and the presets bin
// it goes in. The preset joins the user's Presets bin in the Effects panel (EffectPresets.savePreset, the
// effects.savePreset command); it is not an undo step. Also Preset Properties for a saved preset (`editing`).
WabcutDialog {
    id: dialog
    objectName: "savePresetDialog"

    /// Effect Controls sections to save ("motion", "e12", ...); empty when editing a preset's properties.
    property var sections: []
    /// Preset Properties: the preset being edited ("" = Save Preset).
    property string editing: ""
    /// Or one effect of a clip by its keys (the Color panel's grade): clip key and effect instance id.
    property string clipTarget: ""
    property string effectTarget: ""
    readonly property var types: ["scale", "anchorToInPoint", "anchorToOutPoint"]

    function openForEffect(clipKey, effectId, suggested) {
        openFor([], suggested)
        clipTarget = clipKey
        effectTarget = effectId
    }
    function openFor(sectionList, suggested) {
        editing = ""
        clipTarget = ""
        effectTarget = ""
        sections = sectionList
        nameField.text = EffectPresets.suggestName(suggested)
        typeTabs.currentIndex = 0
        descriptionField.text = ""
        binField.text = ""
        errorText.text = ""
        open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }
    function openProperties(name) {
        const p = EffectPresets.preset(name)
        if (p.name === undefined)
            return
        editing = p.name
        clipTarget = ""
        effectTarget = ""
        sections = []
        nameField.text = p.name
        typeTabs.currentIndex = Math.max(0, types.indexOf(p.type))
        descriptionField.text = p.description
        binField.text = p.bin
        errorText.text = ""
        open()
        nameField.forceActiveFocus()
    }
    function accept() {
        let error = ""
        if (editing !== "") {
            error = EffectPresets.setPresetProperties(editing, nameField.text, types[typeTabs.currentIndex], descriptionField.text)
            const name = nameField.text.trim() !== "" ? nameField.text.trim() : editing
            if (error === "" && binField.text.trim() !== EffectPresets.preset(name).bin)
                error = EffectPresets.movePreset(name, binField.text.trim())
        } else if (clipTarget !== "") {
            error = EffectPresets.saveEffectOfClip(clipTarget, effectTarget, nameField.text, types[typeTabs.currentIndex],
                                                   descriptionField.text, binField.text.trim())
        } else {
            error = EffectPresets.savePreset(sections, nameField.text, types[typeTabs.currentIndex], descriptionField.text,
                                             binField.text.trim())
        }
        if (error === "")
            dialog.close()
        else
            errorText.text = error
    }

    title: editing !== "" ? qsTr("Preset Properties") : qsTr("Save Preset")

    FieldInput {
        id: nameField
        objectName: "savePresetName"
        Layout.fillWidth: true
        placeholderText: qsTr("Name")
        Accessible.name: qsTr("Preset name")
        onAccepted: dialog.accept()
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
    }
    Text {
        Layout.fillWidth: true
        text: qsTr("Type")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
    }
    PillTabs {
        id: typeTabs
        objectName: "savePresetType"
        Layout.fillWidth: true
        compact: true
        model: [qsTr("Scale"), qsTr("Anchor to In Point"), qsTr("Anchor to Out Point")]
        Accessible.name: qsTr("Preset type")
    }
    Text {
        Layout.fillWidth: true
        text: [qsTr("Keyframes keep their place relative to the clip's length."),
               qsTr("Keyframes keep their distance from the clip's In point."),
               qsTr("Keyframes keep their distance from the clip's Out point.")][typeTabs.currentIndex]
        wrapMode: Text.Wrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    FieldInput {
        id: descriptionField
        objectName: "savePresetDescription"
        Layout.fillWidth: true
        placeholderText: qsTr("Description (optional)")
        Accessible.name: qsTr("Preset description")
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
    }
    FieldInput {
        id: binField
        objectName: "savePresetBin"
        Layout.fillWidth: true
        placeholderText: qsTr("Presets bin (optional, e.g. Lower thirds)")
        Accessible.name: qsTr("Presets bin")
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
    }
    Text {
        id: errorText
        objectName: "savePresetError"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "savePresetOk"
            variant: "primary"
            text: dialog.editing !== "" ? qsTr("OK") : qsTr("Save Preset")
            enabled: nameField.text.trim() !== ""
            onClicked: dialog.accept()
        }
    ]
}
