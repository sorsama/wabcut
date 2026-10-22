pragma ComponentBehavior: Bound
import QtQuick

// Effect Controls' right-click menu on a section header (0.2): Save Preset… for that effect, or for every effect of the
// clip (Premiere saves the effects selected in Effect Controls; Wabcut has no multi-selection there, so it offers both).
Item {
    id: root

    /// The section the menu was opened on ("motion", "e12", ...).
    property string section: ""

    function popupAt(target, x, y, sectionKey) {
        section = sectionKey
        menu.popup(target, x, y)
    }

    ContextMenu {
        id: menu
        objectName: "effectSectionMenu"

        ContextMenuItem {
            objectName: "savePresetItem"
            text: qsTr("Save Preset…")
            iconName: "save"
            onTriggered: dialog.openFor([root.section], EffectPresets.sectionTitle(root.section))
        }
        ContextMenuItem {
            objectName: "saveAllPresetItem"
            text: qsTr("Save All Effects as Preset…")
            iconName: "layers"
            enabled: EffectPresets.sectionsOfClip(root.section).length > 1
            onTriggered: dialog.openFor(EffectPresets.sectionsOfClip(root.section), ProjectController.effectControls.title)
        }
    }

    SavePresetDialog {
        id: dialog
    }
}
