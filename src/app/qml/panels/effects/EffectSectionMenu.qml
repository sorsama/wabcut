pragma ComponentBehavior: Bound
import QtQuick

// Effect Controls' right-click menu on a section header (0.2; 0.4: Premiere's full header menu). Cut, Copy and Clear act
// on the selected sections (a right-click on an unselected header selects it alone first: EffectControlsRow); Paste puts
// the copied effects on the selected clips; Effect Enabled, Reset and Move Up / Down act on the header right-clicked.
// Save Preset… keeps that effect, Save All Effects as Preset… every effect of the clip. Every row is one undo step except
// Copy.
Item {
    id: root

    /// The section the menu was opened on ("motion", "e12", ...).
    property string section: ""
    /// That section's header row (EffectControls.row("h:" + section)): effectEnabled, sectionKind, movable.
    property var header: ({})
    readonly property var controls: ProjectController.effectControls
    readonly property bool stackEffect: root.header.sectionKind === "effect"
    /// Taken on each popup, like `header`: a binding on these calls would keep the first popup's answer, since the key
    /// stays the same when the header is right-clicked again after a move or an added effect.
    property bool canMoveUp: false
    property bool canMoveDown: false
    property int clipSectionCount: 0

    function popupAt(target, x, y, sectionKey) {
        section = sectionKey
        header = controls.row("h:" + sectionKey)
        canMoveUp = controls.canMoveSection(sectionKey, -1)
        canMoveDown = controls.canMoveSection(sectionKey, 1)
        clipSectionCount = EffectPresets.sectionsOfClip(sectionKey).length
        menu.popup(target, x, y)
    }

    ContextMenu {
        id: menu
        objectName: "effectSectionMenu"

        ContextMenuItem {
            objectName: "cutEffectsItem"
            text: qsTr("Cut")
            iconName: "scissors"
            command: "edit.cut"
            enabled: root.controls.selectedSections.length > 0
            onTriggered: root.controls.copySelectedSections(true)
        }
        ContextMenuItem {
            objectName: "copyEffectsItem"
            text: qsTr("Copy")
            iconName: "copy"
            command: "edit.copy"
            enabled: root.controls.selectedSections.length > 0
            onTriggered: root.controls.copySelectedSections(false)
        }
        ContextMenuItem {
            objectName: "pasteEffectsItem"
            text: qsTr("Paste")
            iconName: "clipboard-paste"
            command: "edit.paste"
            enabled: root.controls.canPasteEffects
            onTriggered: root.controls.pasteEffects()
        }
        ContextMenuItem {
            objectName: "clearEffectsItem"
            text: qsTr("Clear")
            iconName: "trash-2"
            command: "effectControls.removeSelectedEffect"
            enabled: root.controls.selectedSections.length > 0
            onTriggered: root.controls.removeSelectedSections()
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            objectName: "effectEnabledItem"
            text: qsTr("Effect Enabled")
            iconName: "power"
            visible: root.stackEffect
            height: visible ? implicitHeight : 0
            checkable: true
            checked: root.header.effectEnabled === true
            onTriggered: root.controls.setEffectEnabled(root.section, root.header.effectEnabled !== true)
        }
        ContextMenuItem {
            objectName: "resetEffectItem"
            text: qsTr("Reset")
            iconName: "rotate-ccw"
            enabled: root.header.resettable === true
            onTriggered: root.controls.resetSection(root.section)
        }
        ContextMenuItem {
            objectName: "moveEffectUpItem"
            text: qsTr("Move Up")
            iconName: "chevron-up"
            visible: root.stackEffect
            height: visible ? implicitHeight : 0
            enabled: root.canMoveUp
            onTriggered: root.controls.moveSectionBy(root.section, -1)
        }
        ContextMenuItem {
            objectName: "moveEffectDownItem"
            text: qsTr("Move Down")
            iconName: "chevron-down"
            visible: root.stackEffect
            height: visible ? implicitHeight : 0
            enabled: root.canMoveDown
            onTriggered: root.controls.moveSectionBy(root.section, 1)
        }
        ContextMenuSeparator {}
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
            enabled: root.clipSectionCount > 1
            onTriggered: dialog.openFor(EffectPresets.sectionsOfClip(root.section), ProjectController.effectControls.title)
        }
    }

    SavePresetDialog {
        id: dialog
    }
}
