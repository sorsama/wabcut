import QtQuick

// Right-click menu of a keyframe in the Effect Controls lane: the interpolation of every selected key (Premiere's
// Temporal Interpolation), or deleting them. The check mark shows the interpolation they share.
ContextMenu {
    id: menu
    objectName: "keyframeMenu"

    readonly property var controls: ProjectController.effectControls
    readonly property string current: controls.selectedKeyInterpolation

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
    ContextMenuSeparator {}
    ContextMenuItem {
        text: menu.controls.selectedKeyCount > 1 ? qsTr("Delete %n Keyframes", "", menu.controls.selectedKeyCount)
                                                 : qsTr("Delete Keyframe")
        iconName: "trash-2"
        onTriggered: menu.controls.deleteSelectedKeys()
    }
}
