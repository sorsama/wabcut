import QtQuick

// Right-click menu of a keyframe in the Effect Controls lane: the interpolation of every selected key (Premiere's
// Temporal Interpolation), or deleting them. The check mark shows the interpolation they share. 0.6: Cut / Copy put the
// selected keys on the keyframe clipboard; Paste puts copied keys at the playhead on the clip shown. 0.7: Pin to Clip
// Start / End (checked while the selected keys are pinned there).
ContextMenu {
    id: menu
    objectName: "keyframeMenu"

    readonly property var controls: ProjectController.effectControls
    readonly property string current: controls.selectedKeyInterpolation
    readonly property string pin: controls.selectedKeyPin

    ContextMenuItem {
        text: qsTr("Linear")
        checkable: true
        checked: menu.current === "linear"
        onTriggered: menu.controls.setKeyInterpolation("linear")
    }
    ContextMenuItem {
        text: qsTr("Bezier")
        checkable: true
        checked: menu.current === "bezier"
        onTriggered: menu.controls.setKeyInterpolation("bezier")
    }
    ContextMenuItem {
        text: qsTr("Hold")
        checkable: true
        checked: menu.current === "hold"
        onTriggered: menu.controls.setKeyInterpolation("hold")
    }
    ContextMenuItem {
        text: qsTr("Ease In")
        checkable: true
        checked: menu.current === "easeIn"
        onTriggered: menu.controls.setKeyInterpolation("easeIn")
    }
    ContextMenuItem {
        text: qsTr("Ease Out")
        checkable: true
        checked: menu.current === "easeOut"
        onTriggered: menu.controls.setKeyInterpolation("easeOut")
    }
    ContextMenuItem {
        text: qsTr("Ease In and Out")
        checkable: true
        checked: menu.current === "easeInOut"
        onTriggered: menu.controls.setKeyInterpolation("easeInOut")
    }
    // 0.7 (card 120): pin the key to the clip's start or end: it moves there and follows that end through trims and slips
    // (a fade that starts at the head of a clip still starts at its head after the head is trimmed).
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "keyframePinStart"
        text: qsTr("Pin to Clip Start")
        checkable: true
        checked: menu.pin === "start"
        onTriggered: menu.controls.setKeyPin(menu.pin === "start" ? "none" : "start")
    }
    ContextMenuItem {
        objectName: "keyframePinEnd"
        text: qsTr("Pin to Clip End")
        checkable: true
        checked: menu.pin === "end"
        onTriggered: menu.controls.setKeyPin(menu.pin === "end" ? "none" : "end")
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "keyframeCut"
        text: qsTr("Cut")
        command: "edit.cut"
        enabled: menu.controls.selectedKeyCount > 0
        onTriggered: menu.controls.copySelectedKeys(true)
    }
    ContextMenuItem {
        objectName: "keyframeCopy"
        text: qsTr("Copy")
        command: "edit.copy"
        enabled: menu.controls.selectedKeyCount > 0
        onTriggered: menu.controls.copySelectedKeys(false)
    }
    ContextMenuItem {
        objectName: "keyframePaste"
        text: qsTr("Paste at Playhead")
        command: "edit.paste"
        enabled: menu.controls.canPasteKeys
        onTriggered: menu.controls.pasteKeys()
    }
    ContextMenuItem {
        text: menu.controls.selectedKeyCount > 1 ? qsTr("Delete %n Keyframe(s)", "", menu.controls.selectedKeyCount)
                                                 : qsTr("Delete Keyframe")
        iconName: "trash-2"
        onTriggered: menu.controls.deleteSelectedKeys()
    }
}
