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
    // The item to run once the menu has closed: "rename", "addTrack", "addTracks", "delete" or "deleteEmpty".
    property string pending: ""

    /// Rename...: the header opens its name field.
    signal renameRequested()
    /// Add Tracks...: the Tracks panel opens the dialog.
    signal addTracksRequested()
    /// Delete Track on a track with clips: the Tracks panel asks first.
    signal deleteRequested(var info)
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
        case "delete": menu.deleteTrack(); break
        case "deleteEmpty": menu.report(ProjectController.deleteEmptyTracks("") > 0); break
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
