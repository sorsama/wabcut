pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs

// The Color panel's menu (0.2, Premiere's Lumetri panel menu): Export .cube… writes the grade at the playhead as a
// 33-point 3D LUT (Color.exportCube, the lut.export command; the vignette cannot go in a LUT and is left out), Save
// Preset… keeps the grade as a preset in the Effects panel's Presets bin (EffectPresets). Apply a LUT or look with the
// Input LUT and Look menus of the Basic and Creative sections.
// 0.4: Apply Grade to Selected Clips and Apply Grade to All Clips of "<media>" copy the targeted clip's grade over the
// other clips' grades (color.grade.copy: replaced, not stacked; one undo step; their own masks stay).
Item {
    id: root

    /// What Apply Grade would reach, read when the menu opens: { selection, media, mediaName } (Color.gradeCopyTargets).
    property var targets: ({})

    function popupAt(target, x, y) {
        targets = Color.gradeCopyTargets()
        menu.popup(target, x, y)
    }

    ContextMenu {
        id: menu
        objectName: "colorPanelMenu"

        ContextMenuItem {
            objectName: "applyGradeSelectedItem"
            text: qsTr("Apply Grade to Selected Clips")
            iconName: "copy"
            enabled: Color.hasGrade && (root.targets.selection ?? 0) > 0
            onTriggered: Color.applyGradeTo("selection")
        }
        ContextMenuItem {
            objectName: "applyGradeMediaItem"
            text: (root.targets.mediaName ?? "") !== "" ? qsTr("Apply Grade to All Clips of \"%1\"").arg(root.targets.mediaName)
                                                         : qsTr("Apply Grade to All Clips of This Media")
            iconName: "film"
            enabled: Color.hasGrade && (root.targets.media ?? 0) > 0
            onTriggered: Color.applyGradeTo("media")
        }
        ContextMenuSeparator {}

        ContextMenuItem {
            objectName: "exportCubeItem"
            text: qsTr("Export .cube…")
            iconName: "download"
            enabled: Color.hasGrade
            onTriggered: cubeDialog.open()
        }
        ContextMenuItem {
            objectName: "saveGradePresetItem"
            text: qsTr("Save Preset…")
            iconName: "save"
            enabled: Color.hasGrade
            onTriggered: presetDialog.openForEffect(Color.clip, Color.gradeId(), Color.clipName !== "" ? Color.clipName : qsTr("Color Grade"))
        }
    }

    FileDialog {
        id: cubeDialog
        title: qsTr("Export .cube")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "cube"
        nameFilters: [qsTr("Cube LUTs (*.cube)")]
        onAccepted: {
            const error = Color.exportCube(selectedFile)
            if (error !== "")
                console.warn(error)
        }
    }
    SavePresetDialog {
        id: presetDialog
    }
}
