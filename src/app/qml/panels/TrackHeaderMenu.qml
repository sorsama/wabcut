pragma ComponentBehavior: Bound
import QtQuick

// The right-click menu of a track header (0.2 UX, stream T; Premiere's track header menu), for video and audio tracks
// (ProjectController.trackInfo, read when it opens): Rename... (the header's inline field), Add Video Track Above /
// Add Audio Track Below (one track next to this one), Add Tracks... (the dialog), Delete Track (and its clips: the
// header asks first when it has some), Delete Empty Tracks and Sync Lock - one undo step each. An audio track adds its
// channel format (A.9, M5n), Premiere's track type (mono, stereo, 5.1, adaptive: track.setFormat, one undo step).
// Clips are mixed to it (mono averages, 5.1 to stereo is ITU-R BS.775), and it to the master's format; adaptive passes
// channels through one to one. The items run once the menu has closed: a field or a dialog would lose its focus to the
// closing menu, and adding or deleting a track rebuilds the headers (this menu's header with them). Its own file,
// importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
// 0.7 (card 077): Label, the track's colour (Premiere's 16 label colours with their names, the current one checked, and No
// Label): a bar on the header, a wash on the lane and the mixer strip's colour; one undo step "Track Color".
ContextMenu {
    id: menu
    objectName: "trackHeaderMenu"

    property string trackId: ""
    property string shortName: ""
    /// What the track is (ProjectController.trackInfo), read when the menu opens.
    property var info: ({})
    readonly property bool audio: (menu.info.kind ?? "") === "audio"
    readonly property bool locked: menu.info.locked === true
    readonly property string format: AudioChannels.trackFormats[trackId] ?? "stereo"
    /// Why the last choice was refused ("" after a good one).
    property string error: ""
    // The item to run once the menu has closed: "rename", "addTrack", "addTracks", "delete" or "deleteEmpty"; 0.7: "groupTargeted",
    // "groupNew", "groupAdd" (the group in `pendingGroup`) or "groupRemove".
    property string pending: ""
    property string pendingGroup: ""

    /// Rename...: the header opens its name field.
    signal renameRequested()
    /// Add Tracks...: the Tracks panel opens the dialog.
    signal addTracksRequested()
    /// Delete Track on a track with clips: the Tracks panel asks first.
    signal deleteRequested(var info)
    /// 0.7 (card #139): Video Track Effects...: the Tracks panel opens the track's effect stack.
    signal trackEffectsRequested()
    /// An edit was refused: `message` says why (the Tracks panel shows it).
    signal failed(string message)

    function openFor(target, track, name, point) {
        trackId = track
        shortName = name
        info = ProjectController.trackInfo(track)
        error = ""
        pending = ""
        popup(target, point.x, point.y)
    }
    function choose(format) {
        menu.error = AudioChannels.setTrackFormat(menu.trackId, format)
    }
    function report(ok) {
        menu.error = ok ? "" : ProjectController.lastError
        if (!ok && menu.error !== "")
            menu.failed(menu.error)
    }
    function addOne() {
        const spec = menu.audio ? { audio: 1, audioPlacement: "after", track: menu.trackId }
                                : { video: 1, videoPlacement: "after", track: menu.trackId }
        menu.report(ProjectController.addTracks(spec).length > 0)
    }
    function deleteTrack() {
        if ((menu.info.clipCount ?? 0) > 0)
            menu.deleteRequested(menu.info)
        else
            menu.report(ProjectController.deleteTrack(menu.trackId))
    }

    onClosed: {
        const action = menu.pending
        menu.pending = ""
        switch (action) {
        case "rename": menu.renameRequested(); break
        case "addTrack": menu.addOne(); break
        case "addTracks": menu.addTracksRequested(); break
        case "trackEffects": menu.trackEffectsRequested(); break
        // 0.7 (card 067): Move Track Up / Down, one place up or down the stack as the header is drawn.
        case "moveUp": menu.report(ProjectController.moveTrackBy(menu.trackId, menu.audio ? -1 : 1)); break
        case "moveDown": menu.report(ProjectController.moveTrackBy(menu.trackId, menu.audio ? 1 : -1)); break
        case "delete": menu.deleteTrack(); break
        case "deleteEmpty": menu.report(ProjectController.deleteEmptyTracks("") > 0); break
        // 0.7 (card 124): track groups
        case "groupTargeted": menu.report(ProjectController.groupTargetedTracks(menu.audio ? "audio" : "video") !== ""); break
        case "groupNew": menu.report(ProjectController.createTrackGroup([menu.trackId], "") !== ""); break
        case "groupAdd": menu.report(ProjectController.addTracksToTrackGroup(menu.pendingGroup, [menu.trackId])); break
        case "groupRemove": menu.report(ProjectController.removeTracksFromTrackGroups([menu.trackId])); break
        }
    }

    // Group label: the track, and its own name when it has one.
    Item {
        implicitWidth: titleLabel.implicitWidth + Theme.space(6)
        implicitHeight: titleLabel.implicitHeight + Theme.space(2)
        Text {
            id: titleLabel
            objectName: "trackMenuTitle"
            x: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            text: (menu.info.customName ?? "") !== "" ? qsTr("%1 · %2").arg(menu.shortName).arg(menu.info.customName)
                                                      : (menu.info.name ?? menu.shortName)
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
    }
    ContextMenuItem {
        objectName: "trackMenuRename"
        text: qsTr("Rename…")
        iconName: "pencil"
        onTriggered: menu.pending = "rename"
    }
    ContextMenu {
        id: labelMenu
        objectName: "trackMenuLabel"
        title: qsTr("Label")
        Instantiator {
            model: Library.labelNames
            delegate: ContextMenuItem {
                id: colour
                required property string modelData
                required property int index
                objectName: "trackMenuLabel_" + index
                text: modelData
                iconName: "square"
                iconColor: Theme.clipColor(index)
                checkable: true
                checked: (menu.info.colorIndex ?? -1) === index
                onTriggered: {
                    colour.checked = Qt.binding(() => (menu.info.colorIndex ?? -1) === colour.index)
                    menu.report(ProjectController.setTrackColor(menu.trackId, colour.index))
                }
            }
            onObjectAdded: (index, object) => labelMenu.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => labelMenu.removeItem(object as ContextMenuItem)
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            id: noLabel
            objectName: "trackMenuLabelNone"
            text: qsTr("No Label")
            checkable: true
            checked: (menu.info.colorIndex ?? -1) < 0
            onTriggered: {
                noLabel.checked = Qt.binding(() => (menu.info.colorIndex ?? -1) < 0)
                menu.report(ProjectController.setTrackColor(menu.trackId, -1))
            }
        }
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "trackMenuAddTrack"
        text: menu.audio ? qsTr("Add Audio Track Below") : qsTr("Add Video Track Above")
        iconName: "plus"
        onTriggered: menu.pending = "addTrack"
    }
    ContextMenuItem {
        objectName: "trackMenuAddTracks"
        text: qsTr("Add Tracks…")
        iconName: "layers"
        onTriggered: menu.pending = "addTracks"
    }
    // 0.7 (card #139): the effects that apply to every clip on this video track at once.
    ContextMenuItem {
        objectName: "trackMenuEffects"
        visible: !menu.audio
        height: visible ? implicitHeight : 0
        text: qsTr("Video Track Effects…")
        iconName: "sparkles"
        onTriggered: menu.pending = "trackEffects"
    }
    // 0.7 (card 067): move the track one place up or down its stack (dragging the name chip does it too).
    ContextMenuItem {
        objectName: "trackMenuMoveUp"
        readonly property int index: menu.info.index ?? 0
        readonly property int count: menu.info.kindCount ?? 1
        text: qsTr("Move Track Up")
        iconName: "chevron-up"
        enabled: menu.audio ? index > 0 : index < count - 1
        onTriggered: menu.pending = "moveUp"
    }
    ContextMenuItem {
        objectName: "trackMenuMoveDown"
        readonly property int index: menu.info.index ?? 0
        readonly property int count: menu.info.kindCount ?? 1
        text: qsTr("Move Track Down")
        iconName: "chevron-down"
        enabled: menu.audio ? index < count - 1 : index > 0
        onTriggered: menu.pending = "moveDown"
    }
    // 0.7 (card 124): track groups. Group Targeted Tracks (two or more targeted tracks of this kind in no group), New Group
    // from This Track, Add to Group (the groups of this kind), Remove from Group.
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "trackMenuGroupTargeted"
        text: (menu.info.targetedFree ?? 0) >= 2 ? qsTr("Group Targeted Tracks (%1)").arg(menu.info.targetedFree)
                                                  : qsTr("Group Targeted Tracks")
        iconName: "folder-plus"
        enabled: (menu.info.targetedFree ?? 0) >= 2
        hint: qsTr("Makes a group of the targeted tracks that are in no group")
        onTriggered: menu.pending = "groupTargeted"
    }
    ContextMenuItem {
        objectName: "trackMenuGroupNew"
        text: qsTr("New Group from This Track")
        iconName: "folder"
        enabled: (menu.info.groupId ?? "") === ""
        onTriggered: menu.pending = "groupNew"
    }
    ContextMenu {
        id: groupMenu
        objectName: "trackMenuGroupAdd"
        title: qsTr("Add to Group")
        Instantiator {
            model: menu.info.groups ?? []
            delegate: ContextMenuItem {
                required property var modelData
                objectName: "trackMenuGroupAdd_" + modelData.id
                text: modelData.name
                iconName: "folders"
                onTriggered: {
                    menu.pendingGroup = modelData.id
                    menu.pending = "groupAdd"
                }
            }
            onObjectAdded: (index, object) => groupMenu.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => groupMenu.removeItem(object as ContextMenuItem)
        }
    }
    ContextMenuItem {
        objectName: "trackMenuGroupRemove"
        text: (menu.info.groupName ?? "") !== "" ? qsTr("Remove from Group “%1”").arg(menu.info.groupName)
                                                 : qsTr("Remove from Group")
        iconName: "minus"
        enabled: (menu.info.groupId ?? "") !== ""
        onTriggered: menu.pending = "groupRemove"
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "trackMenuDelete"
        text: (menu.info.clipCount ?? 0) > 0 ? qsTr("Delete Track…") : qsTr("Delete Track")
        iconName: "trash-2"
        enabled: menu.info.canDelete === true
        onTriggered: menu.pending = "delete"
    }
    // Why Delete Track is off (a locked track, the last one of its kind).
    Item {
        objectName: "trackMenuDeleteReason"
        visible: menu.info.canDelete !== true && (menu.info.deleteReason ?? "") !== ""
        implicitWidth: reasonLabel.implicitWidth + Theme.space(7)
        implicitHeight: visible ? reasonLabel.implicitHeight + Theme.space(1) : 0
        Text {
            id: reasonLabel
            x: Theme.space(3.5)
            anchors.verticalCenter: parent.verticalCenter
            text: menu.info.deleteReason ?? ""
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }
    ContextMenuItem {
        objectName: "trackMenuDeleteEmpty"
        text: (menu.info.emptyTracks ?? 0) > 0 ? qsTr("Delete Empty Tracks (%1)").arg(menu.info.emptyTracks)
                                                : qsTr("Delete Empty Tracks")
        iconName: "minus"
        enabled: (menu.info.emptyTracks ?? 0) > 0
        onTriggered: menu.pending = "deleteEmpty"
    }
    ContextMenuSeparator {}
    // Premiere's sync lock: ripple edits and inserts on other tracks shift this track's clips too.
    ContextMenuItem {
        id: syncLock
        objectName: "trackMenuSyncLock"
        text: qsTr("Sync Lock")
        iconName: "link"
        checkable: true
        checked: menu.info.syncLocked === true
        onTriggered: {
            const on = menu.info.syncLocked !== true
            syncLock.checked = Qt.binding(() => menu.info.syncLocked === true)
            ProjectController.setTrackFlag(menu.trackId, "syncLocked", on)
        }
    }

    // ---- Audio tracks: the channel format ----
    ContextMenuSeparator {
        visible: menu.audio
        height: visible ? implicitHeight : 0
    }
    Item {
        visible: menu.audio
        implicitWidth: groupLabel.implicitWidth + Theme.space(6)
        implicitHeight: visible ? groupLabel.implicitHeight + Theme.space(2) : 0
        Text {
            id: groupLabel
            x: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%1 channel format").arg(menu.shortName)
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
    }
    // The formats: checked while the track has it (re-bound after a click, which would break the binding).
    ContextMenuItem {
        id: format0
        objectName: "trackFormat_mono"
        visible: menu.audio
        height: visible ? implicitHeight : 0
        text: qsTr("Mono")
        checkable: true
        checked: menu.format === "mono"
        onTriggered: {
            format0.checked = Qt.binding(() => menu.format === "mono")
            menu.choose("mono")
        }
    }
    ContextMenuItem {
        id: format1
        objectName: "trackFormat_stereo"
        visible: menu.audio
        height: visible ? implicitHeight : 0
        text: qsTr("Stereo")
        checkable: true
        checked: menu.format === "stereo"
        onTriggered: {
            format1.checked = Qt.binding(() => menu.format === "stereo")
            menu.choose("stereo")
        }
    }
    ContextMenuItem {
        id: format2
        objectName: "trackFormat_5.1"
        visible: menu.audio
        height: visible ? implicitHeight : 0
        text: qsTr("5.1")
        checkable: true
        checked: menu.format === "5.1"
        onTriggered: {
            format2.checked = Qt.binding(() => menu.format === "5.1")
            menu.choose("5.1")
        }
    }
    ContextMenuItem {
        id: format71
        objectName: "trackFormat_7.1"
        visible: menu.audio
        height: visible ? implicitHeight : 0
        text: qsTr("7.1")
        checkable: true
        checked: menu.format === "7.1"
        onTriggered: {
            format71.checked = Qt.binding(() => menu.format === "7.1")
            menu.choose("7.1")
        }
    }
    ContextMenuItem {
        id: format3
        objectName: "trackFormat_adaptive"
        visible: menu.audio
        height: visible ? implicitHeight : 0
        text: qsTr("Adaptive")
        checkable: true
        checked: menu.format === "adaptive"
        onTriggered: {
            format3.checked = Qt.binding(() => menu.format === "adaptive")
            menu.choose("adaptive")
        }
    }
}
