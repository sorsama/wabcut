pragma ComponentBehavior: Bound
import QtQuick

// Right-click menu of a timeline transition (0.2 UX, stream T; TimelineView.transitionContextMenuRequested - the
// right-click selected it): Set Duration... (the dialog), its alignment on the cut (between two clips only), Replace
// with the default transition of its kind, Set as Default Transition, and Clear. Facts from
// ProjectController.transitionInfo (read when it opens); the edits are Effect Controls' transition rows'
// (ProjectController.effectControls) on the selected transition, one undo step each. Edits are off on a locked track.
// Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "transitionContextMenu"

    property string transitionId: ""
    /// ProjectController.transitionInfo(transitionId).
    property var info: ({})
    readonly property bool editable: menu.info.locked !== true
    /// The name of the default transition of this one's kind (Replace with ...).
    readonly property string defaultName: {
        const kind = menu.info.kind ?? "video"
        const choice = Editing.transitionChoices(kind).find(c => c.id === (menu.info.defaultEffect ?? ""))
        return choice ? choice.name : ""
    }
    // "duration" once the menu has closed: Set Duration... opens its dialog then.
    property string pending: ""

    /// Set Duration...: the Tracks panel opens the dialog for `info`.
    signal durationRequested(var info)

    function openAt(target, id, point) {
        menu.transitionId = id
        menu.info = ProjectController.transitionInfo(id)
        menu.pending = ""
        popup(target, point.x, point.y)
    }
    onClosed: {
        if (menu.pending === "duration")
            menu.durationRequested(menu.info)
        menu.pending = ""
    }

    // Group label: the transition and its length.
    Item {
        implicitWidth: titleLabel.implicitWidth + Theme.space(6)
        implicitHeight: titleLabel.implicitHeight + Theme.space(2)
        Text {
            id: titleLabel
            objectName: "transitionMenuTitle"
            x: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%1 · %2").arg(menu.info.name ?? "").arg(menu.info.durationText ?? "")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
    }
    ContextMenuItem {
        objectName: "transitionMenuDuration"
        text: qsTr("Set Duration…")
        iconName: "timer"
        enabled: menu.editable
        onTriggered: menu.pending = "duration"
    }
    // Alignment on the cut: only a transition between two clips has a choice.
    ContextMenuItem {
        id: alignCenter
        objectName: "transitionMenuAlignCenter"
        visible: menu.info.twoSided === true
        height: visible ? implicitHeight : 0
        text: qsTr("Center at Cut")
        checkable: true
        checked: menu.info.alignment === "center"
        enabled: menu.editable
        onTriggered: ProjectController.effectControls.setTransitionAlignment("center")
    }
    ContextMenuItem {
        objectName: "transitionMenuAlignStart"
        visible: menu.info.twoSided === true
        height: visible ? implicitHeight : 0
        text: qsTr("Start at Cut")
        checkable: true
        checked: menu.info.alignment === "startAtCut"
        enabled: menu.editable
        onTriggered: ProjectController.effectControls.setTransitionAlignment("startAtCut")
    }
    ContextMenuItem {
        objectName: "transitionMenuAlignEnd"
        visible: menu.info.twoSided === true
        height: visible ? implicitHeight : 0
        text: qsTr("End at Cut")
        checkable: true
        checked: menu.info.alignment === "endAtCut"
        enabled: menu.editable
        onTriggered: ProjectController.effectControls.setTransitionAlignment("endAtCut")
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "transitionMenuReplaceDefault"
        text: menu.defaultName !== "" ? qsTr("Replace with %1").arg(menu.defaultName) : qsTr("Replace with Default")
        iconName: "repeat"
        enabled: menu.editable && menu.info.isDefault !== true && (menu.info.defaultEffect ?? "") !== ""
        onTriggered: ProjectController.effectControls.setTransitionEffect(menu.info.defaultEffect)
    }
    ContextMenuItem {
        objectName: "transitionMenuSetDefault"
        text: menu.info.isDefault === true ? qsTr("Default Transition") : qsTr("Set as Default Transition")
        iconName: "bookmark"
        checkable: menu.info.isDefault === true
        checked: menu.info.isDefault === true
        enabled: menu.info.isDefault !== true
        onTriggered: Editing.setDefaultTransition(menu.info.effect ?? "")
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "transitionMenuClear"
        text: qsTr("Clear")
        iconName: "trash-2"
        command: "edit.clear"
        enabled: menu.editable
        onTriggered: ProjectController.deleteSelectedTransition()
    }
}
