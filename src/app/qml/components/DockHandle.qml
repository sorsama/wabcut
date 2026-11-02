pragma ComponentBehavior: Bound
import QtQuick

// A docked panel's grip, pop-out and close buttons (0.1.3 panel docking, at the end of the panel's header). Drag the
// grip onto another group's tabs or middle to join it, near its edge to dock beside it, to the work area's edge to run
// along that side, or outside the panels (or with Ctrl held) to give the group a window of its own; a floating group's
// grip docks it back the same way. The pop-out button gives the group a window (or docks it back); the close button
// closes the panel shown (the Window menu opens it again).
Row {
    id: root

    required property DockSlot host
    /// Being dragged or focused (a header-less panel keeps the handle shown meanwhile).
    readonly property bool busy: drag.active || toggle.activeFocus || close.activeFocus
    /// P1 round (#32): the workspace is locked - no grip, no pop-out (a floating group still docks back), no close.
    readonly property bool locked: root.host.manager?.locked ?? false

    spacing: 0

    Item {
        id: grip
        objectName: "dockGrip_" + root.host.currentPanel
        visible: !root.locked
        width: Theme.iconSm + Theme.space(2)
        height: Theme.controlHeightSmall
        // P1 round (#31): a name and a tooltip for the grip.
        Accessible.role: Accessible.Grip
        Accessible.name: qsTr("Move %1").arg(Workspaces.panelLabel(root.host.currentPanel))
        Accessible.description: qsTr("Drag to move this panel group")

        Icon {
            anchors.centerIn: parent
            name: "grip-vertical"
            size: Theme.iconSm
            color: drag.active || gripHover.hovered ? Theme.textPrimary : Theme.textTertiary
        }
        HoverHandler {
            id: gripHover
            cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        }
        Loader {
            objectName: "dockGripTip"
            active: gripHover.hovered && !drag.active
            sourceComponent: Tooltip {
                parent: grip
                visible: true
                text: qsTr("Drag to move this panel group")
            }
        }
        DragHandler {
            id: drag
            target: null
            onActiveChanged: {
                if (active)
                    root.host.beginDrag()
                else
                    root.host.endDrag()
            }
            onCentroidChanged: {
                if (active)
                    root.host.moveDrag(grip.mapToGlobal(centroid.position.x, centroid.position.y), centroid.modifiers)
            }
        }
    }

    IconButton {
        id: toggle
        objectName: "dockToggle_" + root.host.currentPanel
        visible: !root.locked || root.host.floating
        anchors.verticalCenter: parent.verticalCenter
        compact: true
        iconName: root.host.floating ? "minimize-2" : "external-link"
        tooltip: root.host.floating ? qsTr("Dock back into the main window") : qsTr("Open in its own window")
        onClicked: {
            if (root.host.floating)
                root.host.dockBack()
            else
                root.host.popOut()
        }
    }

    IconButton {
        id: close
        objectName: "dockClose_" + root.host.currentPanel
        visible: !root.locked
        anchors.verticalCenter: parent.verticalCenter
        compact: true
        iconName: "x"
        tooltip: qsTr("Close %1").arg(Workspaces.panelLabel(root.host.currentPanel))
        onClicked: root.host.closeCurrent()
    }
}
