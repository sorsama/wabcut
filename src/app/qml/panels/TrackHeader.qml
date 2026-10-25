pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Track header row (a ProjectController.tracks delegate): Premiere's two columns, then output/lock (video) or
// mute/solo/record-arm/lock (audio) toggles. Each toggle is an undoable model edit (edit::setTrackState; record arm:
// track.setRecordArm, A.8).
//   * Source patching (left, M5h): the loaded source's track indicator (V1, A1, ...) patched to this track, filled
//     when it takes part in Insert/Overwrite, an outline when switched off. Click an empty slot to patch the source
//     here, drag an indicator to another track to repatch, Alt-click to switch it off (Space for the keyboard).
//     View state of the Source Monitor (SourceMonitor.patchMap), checked by sequence.setPatches.
//   * Track targeting (the name chip): which tracks Lift, Extract, paste, Up/Down and Match Frame use
//     (sequence.setTargets, one undo step).
//   * Track height (M5i): the row follows TimelineView's height for the track; drag the bottom edge to resize it,
//     double-click the edge to reset it.
//   * Channel format (A.9, M5n): an audio track that is not stereo shows its format beside the name chip (1.0 mono,
//     5.1, 1:1 adaptive), set in the header menu (track.setFormat).
//   * UX rework (stream T): right-click a header of either kind (or the Menu key / Shift+F10 on the chip) for the track
//     menu (TrackHeaderMenu: Rename..., Add Track, Add Tracks..., Delete Track, Delete Empty Tracks, Sync Lock, the
//     channel format). A custom name shows beside the chip where it fits (below it on a tall row); double-click the
//     header or Rename... edits it in place (Enter keeps it, Esc cancels; empty = back to "Video n").
//   * 0.4: the toggles show their state as the mixers do: the speaker is crossed only while the track is muted
//     ("Mute A1" / "Unmute A1"); mute amber, solo green, record arm red; objectNames trackOutput_/trackMute_/
//     trackSolo_/trackLock_ + the short name.
Item {
    id: root

    required property string trackId
    required property string shortName
    required property string name
    required property string kind
    required property int colorIndex
    required property bool trackEnabled
    required property bool locked
    required property bool muted
    required property bool solo
    required property bool targeted

    readonly property bool audio: kind === "audio"
    /// A.9: "mono" | "stereo" | "5.1" | "adaptive".
    readonly property string channelFormat: audio ? (AudioChannels.trackFormats[trackId] ?? "stereo") : ""

    /// The track's own name ("" while it has the default "Video n" / "Audio n"; re-read when the name changes).
    readonly property string customName: {
        root.name
        return ProjectController.trackInfo(root.trackId).customName ?? ""
    }
    /// The name field is open (Rename...).
    property bool renaming: false

    /// Add Tracks...: the Tracks panel opens its dialog for this track.
    signal addTracksRequested(string trackId)
    /// Delete Track on a track with clips (`info`: ProjectController.trackInfo): the Tracks panel asks first.
    signal deleteRequested(var info)
    /// An edit from the menu was refused: the Tracks panel shows `text`.
    signal messageRequested(string text)

    /// The track menu (created on first use), for both kinds; the audio channel formats at its end.
    function openMenu(target, point) {
        formatMenu.active = true
        const menu = formatMenu.item as TrackHeaderMenu
        menu.openFor(target, root.trackId, root.shortName, point)
    }
    /// The menu, as A.9 named it (an audio track's format menu): kept for callers and tests.
    function openFormatMenu(target, point) {
        root.openMenu(target, point)
    }
    function startRename() {
        root.renaming = true
    }
    /// Closes the name field: `keep` renames (one undo step; "" = the default name), `refocus` puts focus back on the
    /// chip (Enter, Esc; not when focus went elsewhere).
    function finishRename(name: string, keep: bool, refocus: bool) {
        if (!root.renaming)
            return
        root.renaming = false
        if (keep && name.trim() !== root.customName && !ProjectController.renameTrack(root.trackId, name))
            root.messageRequested(ProjectController.lastError)
        if (refocus)
            chip.forceActiveFocus(Qt.OtherFocusReason)
    }
    /// The lanes (TimelinePanel sets it): the row height comes from there.
    property TimelineView view: null

    height: {
        if (!root.view)
            return Theme.trackHeight(root.kind)
        root.view.layoutRevision // re-read when rows change
        return root.view.trackHeightOf(root.trackId)
    }

    // Design language v3: the row is a pill on the raised surface, inset a little from the lane's height (like the
    // reference's track headers), so neighbouring rows read as separate pills.
    Rectangle {
        id: plate
        anchors.fill: parent
        anchors.topMargin: root.height >= Theme.space(8) ? Theme.space(0.75) : Theme.hairline
        anchors.bottomMargin: anchors.topMargin
        radius: height / 2
        color: Theme.raised
    }
    // Right-click on a header: the track menu. A double-click on its background (or the name) renames the track.
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: eventPoint => root.openMenu(root, eventPoint.position)
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onDoubleTapped: eventPoint => {
            // Only on the background or the name: the chip, the toggles and the resize edge keep their own clicks.
            const hit = root.childAt(eventPoint.position.x, eventPoint.position.y)
            if (hit === null || hit === plate || hit === nameLabel)
                root.startRename()
        }
    }
    Loader {
        id: formatMenu
        objectName: "trackHeaderMenuLoader"
        active: false
        sourceComponent: TrackHeaderMenu {
            onRenameRequested: root.startRename()
            onAddTracksRequested: root.addTracksRequested(root.trackId)
            onDeleteRequested: info => root.deleteRequested(info)
            onFailed: message => root.messageRequested(message)
        }
    }

    // ---- Source patching column ----
    Item {
        id: patchSlot
        objectName: "sourcePatch"
        readonly property var patch: SourceMonitor.patchMap[root.trackId] ?? null
        readonly property bool patched: patch !== null
        readonly property bool patchOn: patched && patch.enabled
        // p02 audio: the source stream as named routing calls it ("A1 (Game)"), its position alone otherwise.
        readonly property string sourceLabel: !patched ? "" : (patch.name ? qsTr("%1 (%2)").arg(patch.source).arg(patch.name) : patch.source)
        // The loaded source has a track of this kind to patch here.
        readonly property bool canPatch: SourceMonitor.indicators.some(i => i.kind === root.kind)
        property bool dragging: false
        property real dragOffset: 0

        anchors.left: parent.left
        anchors.leftMargin: Theme.space(1)
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.sourcePatchWidth
        height: Theme.space(5)
        visible: SourceMonitor.loaded
        z: dragging ? 2 : 0

        activeFocusOnTab: visible && (patched || canPatch)
        Accessible.role: Accessible.Button
        Accessible.name: patched ? qsTr("Source %1 patched to %2").arg(sourceLabel).arg(root.shortName)
                                 : qsTr("Patch the source to %1").arg(root.shortName)
        Accessible.checked: patchOn
        Keys.onSpacePressed: activate(false)

        function activate(alt) {
            if (patched && (alt || patchOn === false))
                SourceMonitor.togglePatch(patch.source)
            else if (!patched && canPatch)
                SourceMonitor.patchTrack(root.trackId)
        }

        // The indicator (follows the pointer while dragged to another track).
        Rectangle {
            id: indicator
            visible: patchSlot.patched
            y: patchSlot.dragOffset
            width: parent.width
            height: parent.height
            radius: height / 2
            color: patchSlot.patchOn ? Theme.accentFill : "transparent"
            border.width: patchSlot.patchOn ? 0 : Theme.hairline * 1.5
            border.color: Theme.textTertiary
            opacity: patchSlot.dragging ? 0.85 : 1

            Text {
                anchors.centerIn: parent
                text: patchSlot.patched ? patchSlot.patch.source : ""
                color: patchSlot.patchOn ? Theme.accentOnFill : Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
                font.weight: Font.Bold
                font.strikeout: !patchSlot.patchOn
            }
        }
        // An empty slot shows where a click would patch.
        Rectangle {
            anchors.fill: parent
            visible: !patchSlot.patched && patchSlot.canPatch && patchMouse.containsMouse
            radius: height / 2
            color: Theme.accentWash
            border.width: Theme.hairline
            border.color: Theme.accentStroke
        }

        MouseArea {
            id: patchMouse
            anchors.fill: parent
            hoverEnabled: true
            preventStealing: true
            cursorShape: patchSlot.patched || patchSlot.canPatch ? Qt.PointingHandCursor : Qt.ArrowCursor
            property real pressY: 0
            onPressed: mouse => {
                pressY = mouse.y
                patchSlot.dragging = false
            }
            onPositionChanged: mouse => {
                if (!pressed || !patchSlot.patched)
                    return
                if (!patchSlot.dragging && Math.abs(mouse.y - pressY) >= Theme.space(1))
                    patchSlot.dragging = true
                if (patchSlot.dragging)
                    patchSlot.dragOffset = mouse.y - pressY
            }
            onReleased: mouse => {
                if (patchSlot.dragging) {
                    // The header under the pointer takes the indicator.
                    const column = root.parent
                    const p = mapToItem(column, mouse.x, mouse.y)
                    for (let i = 0; i < column.children.length; ++i) {
                        const header = column.children[i]
                        if (header.trackId !== undefined && header !== root && p.y >= header.y
                                && p.y < header.y + header.height) {
                            SourceMonitor.movePatch(patchSlot.patch.source, header.trackId)
                            break
                        }
                    }
                } else {
                    patchSlot.activate((mouse.modifiers & Qt.AltModifier) !== 0)
                }
                patchSlot.dragging = false
                patchSlot.dragOffset = 0
            }
            onCanceled: {
                patchSlot.dragging = false
                patchSlot.dragOffset = 0
            }
        }
        Loader {
            active: patchMouse.containsMouse && !patchSlot.dragging && (patchSlot.patched || patchSlot.canPatch)
            sourceComponent: Tooltip {
                parent: patchSlot
                visible: true
                text: patchSlot.patched
                      ? (patchSlot.patchOn ? qsTr("Source %1 goes to %2 in Insert and Overwrite. Drag to another track to repatch; Alt-click to switch it off.").arg(patchSlot.sourceLabel).arg(root.shortName)
                                           : qsTr("Source %1 is switched off: click to switch it on.").arg(patchSlot.sourceLabel))
                      : qsTr("Click to patch the source to %1").arg(root.shortName)
            }
        }
        FocusRing {
            anchors.fill: parent
            targetRadius: height / 2
            visible: patchSlot.activeFocus
        }
    }

    // The name chip is also the track-target toggle (Premiere: edits, pastes and Up/Down use targeted tracks).
    // Targeted = the accent pill with a ring around it and a "▸" in front of the name: shape and text, not colour
    // alone; untargeted chips sit in the well.
    AbstractButton {
        id: chip
        objectName: "trackTarget"
        anchors.left: patchSlot.right
        anchors.leftMargin: Theme.space(1.5)
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.space(9) + (root.targeted ? Theme.space(2) : 0)
        height: Theme.space(5)
        checkable: true
        checked: root.targeted
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.role: Accessible.CheckBox
        Accessible.name: root.customName !== "" ? qsTr("Target %1 (%2)").arg(root.shortName).arg(root.customName)
                                                : qsTr("Target %1").arg(root.shortName)
        Accessible.description: qsTr("Menu key: the track menu; F2: rename")
        Accessible.checked: root.targeted
        onClicked: {
            checked = Qt.binding(() => root.targeted)
            SourceMonitor.setTargeted(root.trackId, !root.targeted)
        }
        // The context menu key (or Shift+F10) opens the track menu from the keyboard; F2 renames the track.
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                root.openMenu(chip, Qt.point(0, chip.height))
                event.accepted = true
            } else if (event.key === Qt.Key_F2 && event.modifiers === Qt.NoModifier) {
                root.startRename()
                event.accepted = true
            }
        }

        background: Rectangle {
            radius: height / 2
            color: root.targeted ? Theme.accentFill : chip.hovered ? Theme.hover : Theme.well
            opacity: root.trackEnabled && !root.muted ? 1 : Theme.disabledOpacity

            Rectangle {
                visible: root.targeted
                anchors.fill: parent
                anchors.margins: -Theme.space(0.75)
                radius: height / 2
                color: "transparent"
                border.width: Theme.selectionWidth
                border.color: Theme.textPrimary
            }
        }
        contentItem: Text {
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: root.targeted ? "▸" + root.shortName : root.shortName
            color: root.targeted ? Theme.accentOnFill : Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
            font.weight: Font.Bold
        }

        Tooltip {
            visible: chip.hovered
            text: (root.customName !== "" ? root.customName + "\n" : "")
                  + (root.targeted ? qsTr("%1 is targeted: Lift, Extract, paste, Up/Down and Match Frame use it").arg(root.shortName)
                                   : qsTr("Target %1 (Lift, Extract, paste, Up/Down and Match Frame)").arg(root.shortName))
        }
        FocusRing {
            anchors.fill: parent
            targetRadius: height / 2
            visible: chip.visualFocus
        }
        HoverHandler {
            enabled: chip.enabled
            cursorShape: Qt.PointingHandCursor
        }
    }
    // The channel format of an audio track that is not stereo (Premiere's track type icon), a click opens the menu.
    Rectangle {
        id: formatBadge
        objectName: "trackFormat_" + root.shortName
        readonly property string glyph: root.channelFormat === "mono" ? qsTr("1.0")
                                      : root.channelFormat === "5.1" ? qsTr("5.1")
                                      : root.channelFormat === "adaptive" ? qsTr("1:1") : ""
        anchors.left: chip.right
        anchors.leftMargin: -Theme.space(1.5)
        anchors.top: chip.top
        anchors.topMargin: -Theme.space(1)
        visible: root.audio && glyph !== ""
        z: 1
        implicitWidth: badgeText.implicitWidth + Theme.space(1.5)
        implicitHeight: badgeText.implicitHeight + Theme.space(0.5)
        width: implicitWidth
        height: implicitHeight
        radius: height / 2
        color: Theme.well
        border.width: Theme.hairline
        border.color: Theme.borderStrong
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("%1 is a %2 track: change its channel format")
                             .arg(root.shortName)
                             .arg(root.channelFormat === "mono" ? qsTr("mono") : root.channelFormat === "5.1" ? qsTr("5.1") : qsTr("adaptive"))
        Accessible.onPressAction: root.openFormatMenu(formatBadge, Qt.point(0, formatBadge.height))

        Text {
            id: badgeText
            anchors.centerIn: parent
            text: formatBadge.glyph
            color: Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
            font.weight: Font.Bold
        }
        TapHandler {
            onTapped: root.openFormatMenu(formatBadge, Qt.point(0, formatBadge.height))
        }
        HoverHandler {
            id: badgeHover
            cursorShape: Qt.PointingHandCursor
        }
        Loader {
            active: badgeHover.hovered
            sourceComponent: Tooltip {
                parent: formatBadge
                visible: true
                text: formatBadge.Accessible.name
            }
        }
    }

    // UX rework (stream T): the track's own name - beside the chip where it fits, else under it on a tall row (a short
    // row shows it in the chip's tooltip only). A double-click renames.
    Text {
        id: nameLabel
        objectName: "trackName_" + root.shortName
        readonly property real inlineX: chip.x + chip.width + (formatBadge.visible ? formatBadge.width : 0) + Theme.space(1.5)
        readonly property bool fitsInline: toggles.x - Theme.space(1) - inlineX >= Theme.space(8)
        readonly property bool fitsBelow: chip.y + chip.height + Theme.space(1) + implicitHeight <= root.height - Theme.space(1)
        x: fitsInline ? inlineX : chip.x + Theme.space(1)
        y: fitsInline ? Math.round((root.height - height) / 2) : chip.y + chip.height + Theme.space(1)
        width: fitsInline ? toggles.x - Theme.space(1) - x : root.width - x - Theme.space(3)
        visible: root.customName !== "" && !root.renaming && (fitsInline || fitsBelow)
        text: root.customName
        elide: Text.ElideRight
        color: root.trackEnabled && !root.muted ? Theme.textSecondary : Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
    }
    // Rename...: a field over the header (Enter keeps the name, Esc cancels, leaving it keeps it too).
    Loader {
        id: renameField
        objectName: "trackRenameLoader"
        active: root.renaming
        x: chip.x
        width: root.width - chip.x - Theme.space(1)
        anchors.verticalCenter: parent.verticalCenter
        z: 3
        sourceComponent: FieldInput {
            objectName: "trackRenameField"
            text: root.customName
            placeholderText: root.name
            Accessible.name: qsTr("Name of %1").arg(root.shortName)
            Component.onCompleted: {
                forceActiveFocus()
                selectAll()
            }
            onAccepted: root.finishRename(text, true, true)
            onActiveFocusChanged: {
                if (!activeFocus)
                    root.finishRename(text, true, false)
            }
            Keys.onEscapePressed: event => {
                root.finishRename("", false, true)
                event.accepted = true
            }
            Keys.onShortcutOverride: event => event.accepted = true
        }
    }

    Row {
        id: toggles
        anchors.right: parent.right
        anchors.rightMargin: Theme.space(1)
        anchors.verticalCenter: parent.verticalCenter
        spacing: 0
        visible: !root.renaming

        IconButton {
            objectName: "trackOutput_" + root.shortName
            visible: !root.audio
            compact: true
            iconName: root.trackEnabled ? "eye" : "eye-off"
            iconSize: Theme.iconSm
            tooltip: qsTr("Toggle output of %1").arg(root.shortName)
            Accessible.role: Accessible.CheckBox
            Accessible.checked: root.trackEnabled
            onClicked: ProjectController.setTrackFlag(root.trackId, "enabled", !root.trackEnabled)
        }
        // 0.4: the toggles look like their state and use the mixers' colours (MixerStrip, ClipMixerStrip): a plain
        // speaker while the track plays, the crossed one only when muted; mute amber, solo green, clipInk glyphs.
        IconButton {
            objectName: "trackMute_" + root.shortName
            visible: root.audio
            compact: true
            checkable: true
            checked: root.muted
            iconName: root.muted ? "volume-x" : "volume-2"
            iconSize: Theme.iconSm
            checkedColor: Theme.warning
            checkedIconColor: Theme.clipInk
            tooltip: root.muted ? qsTr("Unmute %1").arg(root.shortName) : qsTr("Mute %1").arg(root.shortName)
            onToggled: ProjectController.setTrackFlag(root.trackId, "muted", checked)
        }
        IconButton {
            objectName: "trackSolo_" + root.shortName
            visible: root.audio
            compact: true
            checkable: true
            checked: root.solo
            iconName: "headphones"
            iconSize: Theme.iconSm
            checkedColor: Theme.success
            checkedIconColor: Theme.clipInk
            tooltip: qsTr("Solo %1").arg(root.shortName)
            onToggled: ProjectController.setTrackFlag(root.trackId, "solo", checked)
        }
        // Voice-over (A.8, M5m): the microphone arms the track; a pass records onto the first armed audio track.
        RecordArmButton {
            objectName: "recordArm_" + root.shortName
            visible: root.audio
            track: root.trackId
            trackLabel: root.shortName
        }
        IconButton {
            objectName: "trackLock_" + root.shortName
            compact: true
            checkable: true
            checked: root.locked
            iconName: root.locked ? "lock" : "lock-open"
            iconSize: Theme.iconSm
            tooltip: qsTr("Lock %1").arg(root.shortName)
            onToggled: ProjectController.setTrackFlag(root.trackId, "locked", checked)
        }
    }
    // Track height (M5i): drag the bottom edge; a double-click resets the track to the default height.
    // 0.1.3: the height follows the pointer at most once a frame (every change relays out the lanes, and a fast mouse
    // sends many more moves than there are frames); the last one lands on release.
    MouseArea {
        id: resizeHandle
        objectName: "trackResizeHandle"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.trackResizeZone
        enabled: root.view !== null
        property real startHeight: 0
        property real startY: 0
        // The height the pointer asks for and the lanes have not had yet (-1: none).
        property real wanted: -1
        function apply() {
            if (wanted >= 0 && root.view)
                root.view.setTrackHeight(root.trackId, wanted)
            wanted = -1
        }
        onPressed: mouse => {
            startHeight = root.height
            startY = mapToItem(null, mouse.x, mouse.y).y
            wanted = -1
        }
        onPositionChanged: mouse => {
            if (pressed)
                wanted = Math.max(0, startHeight + mapToItem(null, mouse.x, mouse.y).y - startY)
        }
        onReleased: apply()
        onCanceled: apply()
        onDoubleClicked: root.view.setTrackHeight(root.trackId, Theme.trackHeight(root.kind))

        // 0.1.3: the Boxicons resize cursor (EditCursors), like the gutters'.
        CursorArea {
            anchors.fill: parent
            shape: "resizeV"
        }
    }
    FrameAnimation {
        running: resizeHandle.pressed
        onTriggered: resizeHandle.apply()
    }
}
