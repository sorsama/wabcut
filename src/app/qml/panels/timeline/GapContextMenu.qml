pragma ComponentBehavior: Bound
import QtQuick

// Right-click menu of a selected gap (0.2 UX, stream T; TimelineView.gapContextMenuRequested - the right-click selected
// the gap: ProjectController.selectedGap): Ripple Delete closes it (one undo step; Delete and Backspace do the same).
// When another sync-locked track has clips in that time, or the track is locked, nothing moves and `failed` says why.
// Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "gapContextMenu"

    /// ProjectController.selectedGap when the menu opened: { track, trackName, start, end, duration }.
    property var gap: ({})

    /// Ripple Delete was refused: `message` is the model's reason.
    signal failed(string message)

    function openAt(target, point) {
        menu.gap = ProjectController.selectedGap
        popup(target, point.x, point.y)
    }

    // Group label: the gap's track and length.
    Item {
        implicitWidth: titleLabel.implicitWidth + Theme.space(6)
        implicitHeight: titleLabel.implicitHeight + Theme.space(2)
        Text {
            id: titleLabel
            objectName: "gapMenuTitle"
            x: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Gap on %1 · %2").arg(menu.gap.trackName ?? "").arg(menu.gap.duration ?? "")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
    }
    ContextMenuItem {
        objectName: "gapMenuRippleDelete"
        text: qsTr("Ripple Delete")
        iconName: "fold-horizontal"
        command: "edit.rippleDelete"
        enabled: (menu.gap.track ?? "") !== ""
        onTriggered: {
            if (!ProjectController.rippleDeleteGap())
                menu.failed(ProjectController.lastError)
        }
    }
}
