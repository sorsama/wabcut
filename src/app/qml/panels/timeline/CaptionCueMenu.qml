pragma ComponentBehavior: Bound
import QtQuick

// Right-click menu of a caption cue on the timeline's caption lane (0.5; TimelineView.captionContextMenuRequested - the
// right click selected the cue, so the Captions page shows its track): Split at Playhead (when the playhead is inside
// the cue), Delete, Edit Text (the Captions page with the cue's text open for typing) and Select in Captions (the page
// scrolled to it). Edits are the Captions page's own (Captions), one undo step each. Its own file, importing only
// QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "captionCueMenu"

    /// TimelineView.captionInfo(track, cue) when the menu opened.
    property var info: ({})
    /// "edit" or "show" once the menu has closed: the Captions page comes forward then.
    property string pending: ""

    function openAt(target, view, track, cue, point) {
        menu.info = view.captionInfo(track, cue)
        menu.pending = ""
        if ((menu.info.track ?? "") !== "")
            popup(target, point.x, point.y)
    }
    onClosed: {
        const cue = menu.info.cue ?? -1
        if (menu.pending === "edit")
            Captions.editCue(cue)
        else if (menu.pending === "show")
            Captions.showCue(cue)
        menu.pending = ""
    }

    // Group label: the track and the cue's first line.
    Item {
        implicitWidth: Math.min(titleLabel.implicitWidth, Theme.space(80)) + Theme.space(6)
        implicitHeight: titleLabel.implicitHeight + Theme.space(2)
        Text {
            id: titleLabel
            objectName: "captionMenuTitle"
            x: Theme.space(3)
            width: parent.width - Theme.space(6)
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%1 · %2").arg(menu.info.label ?? "").arg(menu.info.text ?? "")
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
    }
    ContextMenuItem {
        objectName: "captionMenuSplit"
        text: qsTr("Split at Playhead")
        iconName: "scissors"
        enabled: menu.info.splittable === true
        onTriggered: Captions.splitCueAtPlayhead(menu.info.cue ?? -1)
    }
    ContextMenuItem {
        objectName: "captionMenuDelete"
        text: qsTr("Delete")
        iconName: "trash-2"
        onTriggered: Captions.removeCue(menu.info.cue ?? -1)
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "captionMenuEdit"
        text: qsTr("Edit Text")
        iconName: "pencil"
        onTriggered: menu.pending = "edit"
    }
    ContextMenuItem {
        objectName: "captionMenuSelect"
        text: qsTr("Select in Captions")
        iconName: "captions"
        onTriggered: menu.pending = "show"
    }
}
