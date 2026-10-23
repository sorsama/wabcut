pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// ● TRACKS: track headers, timecode ruler, lanes (TimelineLanes → C++ TimelineView), playhead, zoom and minimap,
// all reading the real sequence from ProjectController.
//
// Interaction:
// - click a clip to select it (Shift/Ctrl/⌘ toggles, Alt ignores links); drag on empty space for a marquee
// - press/drag the ruler to scrub
// - wheel scrolls horizontally, Shift+wheel scrolls the tracks, Ctrl/⌘+wheel (or Alt+wheel) zooms at the cursor
// - keyboard: KeymapDispatcher (scope "timeline"); Main.qml routes its commands to runCommand() for the
//   view (zoom, tools, snap) and to ProjectController for the playhead, selection, marks and history
// - tools (M5b, TimelineView/TimelineEditing): V select/move (edge drag trims, Ctrl/Cmd ripple-trims; body drag
//   moves, Ctrl/Cmd inserts, Alt ignores links), A/Shift+A track select, B ripple, N roll, Y slip, U slide, C razor;
//   drags show a live ghost preview and a readout, snap (S) with an accent line, and commit on release (Esc cancels)
// - 0.1.1, Premiere's view tools: H hand (drag scrolls the timeline, nothing is edited), Z zoom (click zooms in at the
//   click, Alt+click zooms out); both are a panel overlay over the lanes, so the view sees no press while they are on
// - right click on a clip: the clip context menu (cut/copy/paste, ripple delete, enable, link, rename, label)
// - M5i: sequence tabs above the ruler (SequenceTabs; Ctrl+Tab cycles), per-track heights (drag a header's bottom edge,
//   Ctrl/Alt +/-, Shift+= / Shift+- expand / minimise all, presets in the display settings), trim mode (Trim: click an
//   edge, Shift+T Trim Edit, Ctrl+Shift+T Toggle Trim Type, Ctrl/⌥+arrows, J/K/L dynamic trims), linked selection
//   and the display settings popover
// - M5o (gaming): the Moments popover (zap button): slow-mo, speed ramps, freeze + replay, fast forward, punch-ins
// - 0.1.3: the tools as Premiere's palette groups with flyouts in the header, the modes (Snap, Linked) as labelled
//   toggles beside the sequence tabs; edit cursors (EditCursors) on hover and while a drag holds; drops from the
//   Library and the Source Monitor snap and show an insertion line (trackDrop)
// - UX rework (2026-10-23, stream T): Snap and the time remapping lines are remembered (Preferences); the corner
//   timecode takes a typed time or a +/- offset (TimecodeEntry); the track header menu for both kinds with its Add
//   Tracks... and Delete Track dialogs (TrackHeaderMenu, here: AddTracksDialog, DeleteTrackDialog); right clicks on a
//   transition (TransitionContextMenu: Set Duration..., alignment, Replace with Default, Set as Default, Clear) and in
//   a selected gap (GapContextMenu: Ripple Delete); Delete on a selected gap closes it, a refusal shows as a toast
//   (messageRequested)
Panel {
    id: root

    objectName: "timelinePanel"
    KeymapScope.scope: "timeline"

    // "select", "trackForward", "trackBackward", "ripple", "roll", "rateStretch", "slip", "slide", "razor", "pen" or "remix"
    // (TimelineView.tool),
    // or the view tools "hand" and "zoom" (this panel's overlay; the view keeps the selection tool meanwhile)
    property string tool: "select"
    readonly property bool viewTool: tool === "hand" || tool === "zoom"
    /// Snap (S) and the time remapping lines: remembered between runs (UX rework, stream T: Preferences, timeline/*).
    /// The toggles write Preferences; a write here goes there too.
    property bool snapping: Preferences.timelineSnapping
    onSnappingChanged: Preferences.timelineSnapping = snapping
    /// Audio clips show their volume as a rubber band with keyframes (on in the Audio workspace).
    property bool showVolume: false
    property bool showSpeed: Preferences.timelineShowSpeed
    onShowSpeedChanged: Preferences.timelineShowSpeed = showSpeed
    // `name`: the tool in menus (the flyouts); `label`: its tooltip, saying what it does.
    readonly property var tools: [
        { key: "select", icon: "mouse-pointer-2", name: qsTr("Selection"), label: qsTr("Selection tool"), command: "tool.selection" },
        { key: "trackForward", icon: "arrow-right-to-line", name: qsTr("Track Select Forward"), label: qsTr("Track select forward tool"), command: "tool.trackSelectForward" },
        { key: "trackBackward", icon: "arrow-left-to-line", name: qsTr("Track Select Backward"), label: qsTr("Track select backward tool"), command: "tool.trackSelectBackward" },
        { key: "ripple", icon: "fold-horizontal", name: qsTr("Ripple Edit"), label: qsTr("Ripple edit tool"), command: "tool.rippleEdit" },
        { key: "roll", icon: "unfold-horizontal", name: qsTr("Rolling Edit"), label: qsTr("Rolling edit tool"), command: "tool.rollingEdit" },
        // 0.1.2: R drags a clip's edge to a new length, the speed follows (Speed/Duration); P edits the volume band.
        { key: "rateStretch", icon: "gauge", name: qsTr("Rate Stretch"), label: qsTr("Rate stretch tool: drag an edge to change the speed"), command: "tool.rateStretch" },
        { key: "slip", icon: "arrow-left-right", name: qsTr("Slip"), label: qsTr("Slip tool"), command: "tool.slip" },
        { key: "slide", icon: "move-horizontal", name: qsTr("Slide"), label: qsTr("Slide tool"), command: "tool.slide" },
        { key: "razor", icon: "scissors", name: qsTr("Razor"), label: qsTr("Razor tool"), command: "tool.razor" },
        { key: "pen", icon: "pencil", name: qsTr("Pen"), label: qsTr("Pen tool: click the volume line to add a keyframe, drag to move one"), command: "tool.pen" },
        // 0.1.2: T, Premiere's Type tool: a click in the Program Monitor adds text there (MonitorPanel.typeTool).
        { key: "type", icon: "type", name: qsTr("Type"), label: qsTr("Type tool: click in the Program Monitor to add text"), command: "tool.type" },
        { key: "hand", icon: "hand", name: qsTr("Hand"), label: qsTr("Hand tool: drag to scroll the timeline"), command: "tool.hand" },
        { key: "zoom", icon: "zoom-in", name: qsTr("Zoom"), label: qsTr("Zoom tool: click to zoom in, Alt+click to zoom out, drag to zoom to a range"), command: "tool.zoom" },
        // M5m, Premiere's Remix tool (no default key): drag a music clip's end to a new length; the music is rearranged
        // by its bars (audio.remix), previewed, then applied.
        { key: "remix", icon: "music", name: qsTr("Remix"), label: qsTr("Remix tool: drag a music clip's end to a new length"), command: "" }
    ]
    readonly property var toolCatalog: {
        const catalog = {}
        for (const t of root.tools)
            catalog[t.key] = t
        return catalog
    }
    // 0.1.3 toolbar: the tools in Premiere's palette groups (one button each, the others in its flyout); `gap` starts
    // a new cluster (pointer tools | edit tools | view tools).
    readonly property var toolGroups: [
        { key: "select", tools: ["select"] },
        { key: "track", tools: ["trackForward", "trackBackward"] },
        { key: "ripple", tools: ["ripple", "roll", "rateStretch", "remix"], gap: true },
        { key: "razor", tools: ["razor"] },
        { key: "slip", tools: ["slip", "slide"] },
        { key: "pen", tools: ["pen"] },
        { key: "hand", tools: ["hand", "zoom"], gap: true },
        { key: "type", tools: ["type"] }
    ]
    /// Each group's face: the tool last used from it (group key -> tool key).
    property var groupShown: ({})
    onToolChanged: {
        const group = root.toolGroups.find(g => g.tools.includes(root.tool))
        if (group && root.groupShown[group.key] !== root.tool) {
            const next = Object.assign({}, root.groupShown)
            next[group.key] = root.tool
            root.groupShown = next
        }
    }
    // The toolbar's density (0.1.3): narrower panels first drop the mode labels, then fold the view toggles into a
    // menu; the palette folds into one button showing the active tool. Never does the whole toolbar disappear. (The
    // header row also holds the dock's grip, pop-out and close buttons.)
    readonly property bool paletteCompact: root.width < Theme.space(128)
    readonly property bool chipLabels: root.width >= Theme.space(158)
    readonly property bool viewTogglesFolded: root.width < Theme.space(118)

    // 0.1.3: a drag from outside the lanes (a Library card, the Source Monitor, a search hit, a graphics template) over
    // the timeline: where it would land (TimelineView.dropPoint: the frame under the pointer, snapped to the clip in
    // front, the playhead, marks and markers when snapping is on), shown as an insertion line in the lanes. (x, y) are
    // in `item`'s coordinates; null off the lanes. Main's drop areas call it on every move and on the drop.
    //
    // 2026-10-15: held still over the ruler (above the top video track) or below the last track for newTrackHoldMs,
    // the drag adds a track there (V3 above V2, A3 below A2; a band fills while it waits) and lands on it while the
    // pointer stays there. One such track per drag: holding at the other end moves it there. If the drag then drops
    // elsewhere or leaves the timeline, the track goes again (TimelineView.releaseDropTrack).
    function trackDrop(item, x, y) {
        const v = lanes.view
        const p = item.mapToItem(v, x, y)
        root.dragPoint = { item: item, x: x, y: y }
        const zone = root.newTrackZoneAt(p)
        root.holdIn(zone)
        if (zone !== "" && root.heldTrack && root.heldTrack.kind === zone)
            return v.showDropIndicatorOnTrack(p.x, root.heldTrack.key)
        if (p.x < 0 || p.y < 0 || p.x > v.width || p.y > v.height) {
            v.clearDropIndicator()
            return null
        }
        return v.showDropIndicator(p.x, p.y)
    }
    /// The drag left the timeline (`at` undefined) or was dropped where trackDrop said (`at`): the insertion line goes,
    /// and so does a track its hold added that the drop does not land on.
    function endDrop(at) {
        holdTimer.stop()
        holdFill.stop()
        root.holdZone = ""
        root.dragPoint = null
        if (root.heldTrack && !(at && at.track === root.heldTrack.key))
            lanes.view.releaseDropTrack(root.heldTrack.key)
        root.heldTrack = null
        lanes.view.clearDropIndicator()
    }

    /// How long a drag waits over the ruler or below the tracks before it adds a track there.
    property int newTrackHoldMs: 2000
    /// Where the drag is waiting for a new track: "video" (over the ruler, above the top video track), "audio" (below
    /// the last track: the empty lanes under it, or the footer) or "".
    property string holdZone: ""
    /// The track this drag's hold added: { key, kind }, until the drag ends.
    property var heldTrack: null
    /// trackDrop's last arguments: the insertion line moves onto a new track without waiting for the pointer.
    property var dragPoint: null

    /// The new-track zone at `p` (the lanes' coordinates): "video", "audio" or "".
    function newTrackZoneAt(p) {
        const v = lanes.view
        if (v.contentHeight <= 0 || p.x < 0 || p.x > v.width)
            return ""
        if (p.y < 0)
            return p.y >= v.mapFromItem(ruler, 0, 0).y ? "video" : ""
        return p.y > v.height || p.y >= v.contentHeight - v.scrollY ? "audio" : ""
    }
    function holdIn(zone) {
        if (zone === root.holdZone)
            return
        root.holdZone = zone
        holdTimer.stop()
        holdFill.stop()
        if (zone !== "" && !(root.heldTrack && root.heldTrack.kind === zone)) {
            holdTimer.start()
            holdFill.restart()
        }
    }
    function addHeldTrack() {
        const zone = root.holdZone
        if (zone === "")
            return
        const v = lanes.view
        if (root.heldTrack) {
            v.releaseDropTrack(root.heldTrack.key)
            root.heldTrack = null
        }
        const key = v.addDropTrack(zone)
        if (key !== "")
            root.heldTrack = { key: key, kind: zone }
        // The pointer may not move again before the drop: the insertion line goes on the new track now.
        const at = root.dragPoint
        if (at)
            root.trackDrop(at.item, at.x, at.y)
    }
    Timer {
        id: holdTimer
        interval: root.newTrackHoldMs
        onTriggered: root.addHeldTrack()
    }
    readonly property TimelineView view: lanes.view
    /// The Timeline list (M5j): the tracks and clips as an accessible list instead of the lanes.
    property bool listMode: false
    onListModeChanged: {
        if (listMode)
            Qt.callLater(() => timelineList.focusList())
        else
            lanesArea.forceActiveFocus(Qt.TabFocusReason)
    }
    readonly property int visibleTrackCount: lanes.view.visibleTrackCount

    // Zoom by `delta` levels keeping the playhead (or the view centre) in place.
    function zoomBy(delta) {
        const v = lanes.view
        const anchor = v.playheadX >= 0 && v.playheadX <= v.width ? v.playheadX : v.width / 2
        v.zoomAt(v.zoomLevel + delta, anchor)
    }

    // Keymap commands the timeline view owns. Returns false for anything else.
    // The empty state's action (Main opens File > Import).
    signal importRequested()
    /// UX rework (stream T): an edit from the panel's menus was refused; Main shows `text` as a toast.
    signal messageRequested(string text)

    /// The keyboard focus is in the Timeline (its keymap scope) of the window the user works in: the main window, or a
    /// floating Timeline's own window unless the main window is the active one (the floating window keeps its focus
    /// item while the user works in the main window).
    function hasKeyboardFocus(): bool {
        const w = root.Window.window
        const item = w ? w.activeFocusItem : null
        if (!item || KeymapDispatcher.scopeAt(item) !== "timeline")
            return false
        const main = KeymapDispatcher.window
        return !main || w === main || w.active || !main.active
    }

    function runCommand(commandId) {
        switch (commandId) {
        // Track heights (M5i)
        case "timeline.increaseVideoTrackHeight": lanes.view.changeTrackHeights("video", 1); return true
        case "timeline.decreaseVideoTrackHeight": lanes.view.changeTrackHeights("video", -1); return true
        case "timeline.increaseAudioTrackHeight": lanes.view.changeTrackHeights("audio", 1); return true
        case "timeline.decreaseAudioTrackHeight": lanes.view.changeTrackHeights("audio", -1); return true
        case "timeline.expandAllTracks": lanes.view.changeTrackHeights("all", 1); return true
        case "timeline.minimizeAllTracks": lanes.view.minimizeAllTracks(); return true
        // Sequence tabs (M5i)
        case "timeline.nextSequenceTab": SequenceTabs.cycle(1); return true
        case "timeline.previousSequenceTab": SequenceTabs.cycle(-1); return true
        case "sequence.zoomIn": root.zoomBy(0.1); return true
        case "sequence.zoomOut": root.zoomBy(-0.1); return true
        case "sequence.zoomToSequence": lanes.view.zoomAt(0, 0); return true
        case "sequence.snap": Preferences.timelineSnapping = !root.snapping; return true
        // UX rework (stream T): Delete, Backspace and Ripple Delete close a selected gap when no clip is selected; when
        // the model refuses (another sync-locked track has clips in that time, a locked track) the panel says why.
        // A gap is selected by any plain click in empty lane space, so Delete and Shift+Delete (application keys) close
        // it only while the Timeline has the keyboard focus: pressed in another panel they do nothing (the command
        // stops here, so ProjectController's Clear does not close the gap either).
        case "edit.clear":
        case "edit.rippleDelete":
        case "timeline.clearSelection":
        case "timeline.rippleDelete":
            if ((ProjectController.selectedGap.track ?? "") === "" || ProjectController.selectedClips.length > 0)
                break
            if (!root.hasKeyboardFocus())
                return true
            if (!ProjectController.rippleDeleteGap())
                root.messageRequested(ProjectController.lastError)
            return true
        }
        const tool = root.tools.find(t => t.command === commandId)
        if (tool) {
            root.tool = tool.key
            return true
        }
        // Trim mode (M5i): Toggle Trim Type, Trim Backward / Forward (Many).
        return Trim.runCommand(commandId)
    }

    /// Keyboard focus to the lanes (or the Timeline list when it is shown). `quiet`: focus the app moved here by itself
    /// (Main.keepFocus at startup or after a panel closes), so no focus ring - the ring marks where the user went.
    function focusLanes(quiet = false) {
        if (root.listMode) {
            timelineList.focusList()
        } else if (quiet) {
            lanesArea.pointerFocus = true
            lanesArea.forceActiveFocus(Qt.OtherFocusReason)
        } else {
            lanesArea.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    // Keeps the playhead in view after keyboard navigation.
    function revealPlayhead() {
        lanes.view.revealPlayhead()
    }

    title: qsTr("Tracks")

    // 0.1.3 toolbar, in three places so tools, modes and actions never mix (one mixed row of icons confused users):
    //   * the header: the edit tools as Premiere's palette, one button per group with the rest in its flyout;
    //   * the right of the sequence tab row: the editing modes as labelled toggles (Snap, Linked), then the view
    //     toggles (volume and speed keys, the Timeline list), then the actions (Moments, Gaming layout, display);
    //   * the corner left of the ruler: the playhead timecode and the frame rate.
    leading: [
        // Design language v3: the palette is a segmented pill group, its buttons on one raised track.
        Item {
            implicitWidth: palette.width + Theme.space(1)
            implicitHeight: palette.height + Theme.space(1)

            Rectangle {
                anchors.fill: parent
                radius: height / 2
                color: Theme.raised
            }

            Row {
                id: palette
                objectName: "timelineToolPalette"
                anchors.centerIn: parent
                spacing: Theme.space(0.5)
                Accessible.role: Accessible.ToolBar
                Accessible.name: qsTr("Edit tools")

                Repeater {
                    model: root.paletteCompact ? [{ key: "all", tools: root.tools.map(t => t.key) }] : root.toolGroups

                    delegate: Item {
                        id: slot
                        required property var modelData
                        readonly property real gap: slot.modelData.gap === true ? Theme.space(2) : 0
                        width: groupButton.width + slot.gap
                        height: groupButton.height

                        // A cluster starts with a hairline.
                        Rectangle {
                            visible: slot.gap > 0
                            x: Math.round(slot.gap / 2) - Theme.space(0.5)
                            anchors.verticalCenter: parent.verticalCenter
                            width: Theme.hairline
                            height: Theme.controlHeightSmall - Theme.space(2)
                            color: Theme.borderStrong
                        }
                        ToolGroupButton {
                            id: groupButton
                            x: slot.gap
                            group: slot.modelData
                            catalog: root.toolCatalog
                            current: root.tool
                            // The compact palette's one button shows the active tool.
                            shown: slot.modelData.key === "all" ? root.tool
                                   : (root.groupShown[slot.modelData.key] ?? slot.modelData.tools[0])
                            onToolChosen: key => root.tool = key
                        }
                    }
                }
            }
        }
    ]

    // ---- Sequence tabs (M5i) and, at the right of their row, the modes, view toggles and actions (0.1.3) ----
    SequenceTabBar {
        id: tabBar
        x: 0
        y: 0
        width: Math.max(0, (crumbs.visible ? crumbs.x : toolbar.x) - Theme.space(2))
        height: Theme.sequenceTabHeight
    }

    Row {
        id: toolbar
        objectName: "timelineToolbar"
        anchors.right: parent.right
        y: Math.round((Theme.sequenceTabHeight - Theme.controlHeightSmall) / 2)
        height: Theme.controlHeightSmall
        spacing: Theme.space(1)
        Accessible.role: Accessible.ToolBar
        Accessible.name: qsTr("Timeline modes and view")

        component Divider: Rectangle {
            anchors.verticalCenter: parent ? parent.verticalCenter : undefined
            width: Theme.hairline
            height: Theme.controlHeightSmall - Theme.space(2)
            color: Theme.borderStrong
        }

        // Editing modes: labelled, an accent wash while on.
        ToggleChip {
            objectName: "snapToggle"
            text: qsTr("Snap")
            iconName: "magnet"
            showLabel: root.chipLabels
            tooltip: qsTr("Snap: clips, edges, drops and the playhead stick to clip edges, marks and markers")
            command: "sequence.snap"
            checked: root.snapping
            onToggled: Preferences.timelineSnapping = checked
        }
        // Premiere's Linked Selection (M5i): clicking a clip selects its linked clips too.
        ToggleChip {
            objectName: "linkedSelectionButton"
            text: qsTr("Linked")
            iconName: Preferences.linkedSelection ? "link" : "unlink"
            showLabel: root.chipLabels
            tooltip: qsTr("Linked selection: a click selects a clip's linked video and audio too (Alt-click for one)")
            checked: Preferences.linkedSelection
            onToggled: Preferences.linkedSelection = checked
        }

        Item { width: Theme.space(1); height: 1 }
        Divider {}
        Item { width: Theme.space(1); height: 1 }

        // View toggles: what the lanes show (folded into a menu on narrow panels).
        IconButton {
            objectName: "volumeKeysToggle"
            visible: !root.viewTogglesFolded
            compact: true
            checkable: true
            checked: root.showVolume
            iconName: "spline"
            tooltip: qsTr("Show audio volume keyframes (click the line to add one, Alt-click a key for Bezier)")
            onToggled: root.showVolume = checked
        }
        IconButton {
            objectName: "speedKeysToggle"
            visible: !root.viewTogglesFolded
            compact: true
            checkable: true
            checked: root.showSpeed
            iconName: "timer"
            tooltip: qsTr("Time remapping: click the speed line for a key, drag its halves for a ramp, Ctrl-drag the line for speed, Alt-drag for a hold")
            onToggled: Preferences.timelineShowSpeed = checked
        }
        // The Timeline list (M5j): tracks and clips as a list for the keyboard and screen readers.
        IconButton {
            objectName: "timelineListButton"
            visible: !root.viewTogglesFolded
            compact: true
            checkable: true
            checked: root.listMode
            iconName: "list"
            tooltip: qsTr("Timeline list (tracks and clips as a list)")
            onToggled: root.listMode = checked
        }
        IconButton {
            id: viewMenuButton
            objectName: "timelineViewMenuButton"
            visible: root.viewTogglesFolded
            compact: true
            checkable: true
            checked: root.showVolume || root.showSpeed || root.listMode
            iconName: "eye"
            tooltip: qsTr("View: volume keyframes, time remapping, the Timeline list")
            onClicked: {
                checked = Qt.binding(() => root.showVolume || root.showSpeed || root.listMode)
                viewMenu.popup(viewMenuButton, 0, viewMenuButton.height + Theme.space(1))
            }

            CheckMenu {
                id: viewMenu
                objectName: "timelineViewMenu"
                options: [
                    { key: "volume", name: qsTr("Volume keyframes"), icon: "spline", checked: root.showVolume },
                    { key: "speed", name: qsTr("Time remapping"), icon: "timer", checked: root.showSpeed },
                    { key: "list", name: qsTr("Timeline list"), icon: "list", checked: root.listMode }
                ]
                onToggled: key => {
                    if (key === "volume")
                        root.showVolume = !root.showVolume
                    else if (key === "speed")
                        Preferences.timelineShowSpeed = !root.showSpeed
                    else
                        root.listMode = !root.listMode
                }
            }
        }

        Item { width: Theme.space(1); height: 1 }
        Divider {}
        Item { width: Theme.space(1); height: 1 }

        // Actions.
        // Moments (M5o, gaming): one-click slow-mo, ramps, freeze + replay, fast forward and punch-ins at the playhead.
        IconButton {
            id: momentsButton
            objectName: "momentsButton"
            compact: true
            checkable: true
            checked: momentsPopover.opened
            iconName: "zap"
            tooltip: qsTr("Moments: slow-mo, speed ramps and punch-ins at the playhead")
            onClicked: {
                checked = Qt.binding(() => momentsPopover.opened)
                if (momentsPopover.opened)
                    momentsPopover.close()
                else
                    momentsPopover.open()
            }

            MomentsPopover {
                id: momentsPopover
                y: momentsButton.height + Theme.space(1)
            }
        }
        // M5o (gaming): the facecam / vertical layout picker (C.15 layout.apply).
        IconButton {
            objectName: "timelineLayoutButton"
            compact: true
            iconName: "layout-grid"
            tooltip: qsTr("Gaming layout")
            onClicked: Layouts.open()
        }
        IconButton {
            id: displayButton
            objectName: "timelineDisplayButton"
            compact: true
            iconName: "sliders-horizontal"
            tooltip: qsTr("Timeline display settings")
            onClicked: displayPopover.opened ? displayPopover.close() : displayPopover.open()

            TimelineDisplayPopover {
                id: displayPopover
                y: displayButton.height + Theme.space(1)
                view: lanes.view
            }
        }
    }

    // M5l: the way back from a nested sequence opened from a clip (Nesting.breadcrumb), outermost first.
    Row {
        id: crumbs
        objectName: "nestedBreadcrumb"
        anchors.right: toolbar.left
        anchors.rightMargin: Theme.space(3)
        y: 0
        height: Theme.sequenceTabHeight
        spacing: Theme.space(1)
        visible: Nesting.breadcrumb.length > 1

        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: "layers"
            size: Theme.iconXs
            color: Theme.nestedStripe
        }
        Repeater {
            model: Nesting.breadcrumb
            delegate: Row {
                id: crumb
                required property var modelData
                required property int index
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: crumb.index > 0
                    name: "chevron-right"
                    size: Theme.iconXs
                    color: Theme.textTertiary
                }
                AbstractButton {
                    id: crumbButton
                    objectName: "nestedCrumb_" + crumb.modelData.key
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: !crumb.modelData.current
                    height: Theme.controlHeightSmall
                    width: crumbText.implicitWidth + Theme.space(3)
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    onClicked: Nesting.goTo(crumb.modelData.key)
                    Accessible.name: crumb.modelData.name
                    contentItem: Text {
                        id: crumbText
                        text: crumb.modelData.name
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        color: crumb.modelData.current ? Theme.textPrimary : Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        font.weight: crumb.modelData.current ? Font.DemiBold : Font.Normal
                    }
                    background: Rectangle {
                        radius: Theme.radiusField
                        color: crumbButton.hovered ? Theme.hover : "transparent"
                        FocusRing {
                            targetRadius: Theme.radiusField
                            visible: crumbButton.visualFocus
                        }
                    }
                    HoverHandler {
                        enabled: crumbButton.enabled
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }
        }
    }

    // ---- Ruler row ----
    Item {
        id: corner
        x: 0
        y: tabBar.height + Theme.space(1)
        width: Theme.trackHeaderWidth
        height: Theme.rulerHeight

        // The playhead timecode. UX rework (stream T): click it (or Tab to it and type) to go to a time: a timecode,
        // frames, or + / - an offset (TimecodeEntry, ProjectController.seekToText); Esc cancels.
        TimecodeEntry {
            id: cornerTimecode
            objectName: "timelineTimecode"
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            height: Theme.controlHeightSmall
            pill: false
            text: ProjectController.playheadText
            label: qsTr("Playhead")
            pixelSize: Theme.textLg + Theme.space(0.5)
            seek: typed => {
                const moved = ProjectController.seekToText(typed)
                if (moved)
                    root.revealPlayhead()
                return moved
            }
            preview: typed => ProjectController.timeEntryText(typed)
        }
        // The sequence's frame rate (0.1.3: moved here from the header, which now holds the tools), only where it
        // fits beside the timecode (the title bar shows it too).
        Text {
            objectName: "timelineFrameRate"
            anchors.right: parent.right
            anchors.rightMargin: Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            visible: cornerTimecode.x + cornerTimecode.implicitWidth + Theme.space(2)
                     <= corner.width - Theme.space(2) - implicitWidth
            text: ProjectController.frameRateText
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
    }

    TimelineRuler {
        id: ruler
        x: lanesArea.x
        y: tabBar.height + Theme.space(1)
        width: lanesArea.width
        height: Theme.rulerHeight
        view: lanes.view
    }

    // Playhead line between the ruler and the lanes (the view draws it inside the lanes).
    Rectangle {
        readonly property real px: lanes.view.playheadX
        x: lanesArea.x + Math.round(px - width / 2)
        y: ruler.y + ruler.height - Theme.space(1)
        width: Theme.playheadWidth
        height: tracksArea.y - y
        color: Theme.playhead // the playhead line's top (white, like the line the view draws)
        visible: px >= 0 && px <= lanesArea.width
    }

    // ---- Tracks: headers + lanes, vertically scrolled by the view ----
    Item {
        id: tracksArea
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: ruler.bottom
        anchors.bottom: footer.top
        anchors.topMargin: Theme.space(2)
        anchors.bottomMargin: Theme.space(2)
        clip: true

        Column {
            id: headers
            y: -lanes.view.scrollY
            width: Theme.trackHeaderWidth
            spacing: Theme.trackGap

            Repeater {
                model: ProjectController.tracks
                delegate: TrackHeader {
                    width: headers.width
                    view: lanes.view
                    // UX rework (stream T): the header menu's dialogs and refusals.
                    onAddTracksRequested: trackId => addTracksDialog.openFor(trackId)
                    onDeleteRequested: info => deleteTrackDialog.openFor(info)
                    onMessageRequested: text => root.messageRequested(text)
                }
            }

            // The track a drop below the last track would create (shown while the drag previews it).
            Rectangle {
                objectName: "newTrackHeader"
                visible: lanes.view.newTrackLabel !== ""
                width: headers.width
                height: Theme.trackHeightAudio
                radius: Theme.radiusClip
                color: Theme.accentWash
                border.width: Theme.hairline
                border.color: Theme.accentStroke

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space(3)
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("+ New track %1").arg(lanes.view.newTrackLabel)
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
            }
        }

        FocusScope {
            id: lanesArea
            objectName: "timelineLanesArea"
            x: Theme.trackHeaderWidth + Theme.space(2)
            width: parent.width - x
            height: parent.height
            activeFocusOnTab: true

            // Focus ring only for keyboard focus (plain Items have no visualFocus).
            property bool pointerFocus: false
            function focusByPointer() {
                pointerFocus = true
                forceActiveFocus(Qt.MouseFocusReason)
            }
            onActiveFocusChanged: {
                if (!activeFocus)
                    pointerFocus = false
            }

            Accessible.role: Accessible.Pane
            Accessible.name: qsTr("Timeline")
            // Esc leaves trim mode (a drag in progress takes Esc first, in the view).
            Keys.onEscapePressed: event => {
                event.accepted = Trim.active
                Trim.clear()
            }

            TimelineLanes {
                id: lanes
                anchors.fill: parent
                tool: root.viewTool || root.tool === "type" ? "select" : root.tool
                snapping: root.snapping
                showVolume: root.showVolume
                showSpeed: root.showSpeed
                onPressed: lanesArea.focusByPointer()
            }

            // The hand and zoom tools (0.1.1; the zoom marquee 0.2; their cursors 0.1.3): TimelineZoomToolArea.qml.
            TimelineZoomToolArea {
                id: viewToolArea
                objectName: "timelineViewToolArea"
                z: 5
                anchors.fill: lanes
                view: lanes.view
                tool: root.viewTool ? root.tool : ""
                onPressedByPointer: lanesArea.focusByPointer()
            }

            // One panel overlay for the active speed gesture; the lanes remain a bare scene-graph host.
            Rectangle {
                z: 10
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.space(2)
                width: speedReadout.implicitWidth + Theme.space(4)
                height: Theme.controlHeight
                radius: Theme.radiusField
                color: Theme.raised
                border.width: Theme.hairline
                border.color: Theme.accentStroke
                visible: lanes.view.speedReadout !== ""
                Text {
                    id: speedReadout
                    anchors.centerIn: parent
                    text: lanes.view.speedReadout
                    color: Theme.textPrimary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }

            // No clips yet (design language v2 empty state). clipCount follows sequence switches; the undo text makes
            // it re-read after every edit.
            EmptyState {
                objectName: "timelineEmptyState"
                anchors.centerIn: parent
                width: Math.min(parent.width - Theme.space(8), Theme.space(110))
                visible: {
                    ProjectController.undoText
                    return ProjectController.clipCount === 0
                }
                illustration: "timeline"
                text: qsTr("Drag clips here from the Library or the Source Monitor, or press , to insert the source.")
                actionText: qsTr("Import media")
                actionIcon: "import"
                onActionTriggered: root.importRequested()
            }

            // Effects and transitions dragged from the Effects panel (M5d): the clip or edit point under the pointer
            // lights up with what the drop would do. A transition is dry-run first: "using the available handles"
            // (amber) when the media is short, the model's reason (red) when it cannot go there. 0.2: a Text Style from
            // the Library ("textStyle:<name>") goes on a graphic clip.
            DropArea {
                id: effectDrop
                objectName: "timelineEffectDropArea"
                anchors.fill: parent
                keys: ["application/x-wabcut-effect", "application/x-wabcut-text-style"]

                property string effectId: ""
                property var target: ({})
                property point pointer
                readonly property rect area: {
                    const t = target
                    if (!(t.width > 0))
                        return Qt.rect(0, 0, 0, 0)
                    const p = mapFromItem(lanes.view, t.x, t.y)
                    return Qt.rect(p.x, p.y, t.width, t.height)
                }

                function track(x, y) {
                    pointer = Qt.point(x, y)
                    const p = mapToItem(lanes.view, x, y)
                    target = lanes.view.effectDropTarget(p.x, p.y, effectId)
                }

                onEntered: drag => {
                    effectId = drag.formats.indexOf("application/x-wabcut-text-style") >= 0
                        ? "textStyle:" + drag.getDataAsString("application/x-wabcut-text-style")
                        : drag.getDataAsString("application/x-wabcut-effect")
                    track(drag.x, drag.y)
                    drag.accepted = effectId !== ""
                }
                onPositionChanged: drag => track(drag.x, drag.y)
                onExited: {
                    target = ({})
                    effectId = ""
                }
                onDropped: drop => {
                    const p = mapToItem(lanes.view, drop.x, drop.y)
                    // M5m: an audio plugin goes through Plugins.apply, which also stores its name (the missing badge).
                    const clip = effectId.startsWith("plugin.") ? lanes.view.clipAt(p.x, p.y) : ""
                    const error = clip !== "" ? Plugins.apply(effectId, clip) : lanes.view.dropEffect(p.x, p.y, effectId)
                    target = ({})
                    effectId = ""
                    if (error === "")
                        drop.accept(Qt.CopyAction)
                }
            }
            Rectangle {
                objectName: "effectDropHighlight"
                visible: effectDrop.containsDrag && effectDrop.area.width > 0
                x: effectDrop.area.x
                y: effectDrop.area.y
                width: effectDrop.area.width
                height: effectDrop.area.height
                radius: Theme.radiusClip
                color: !(effectDrop.target.ok ?? false) ? Qt.alpha(Theme.danger, 0.16)
                     : (effectDrop.target.warning ?? false) ? Qt.alpha(Theme.warning, 0.18) : Theme.accentWash
                border.width: Theme.selectionWidth
                border.color: !(effectDrop.target.ok ?? false) ? Theme.danger
                            : (effectDrop.target.warning ?? false) ? Theme.warning : Theme.accentStroke
            }
            Rectangle {
                objectName: "effectDropTip"
                visible: effectDrop.containsDrag && (effectDrop.target.message ?? "") !== ""
                x: Math.round(Math.min(lanesArea.width - width, Math.max(0, effectDrop.pointer.x + Theme.space(3))))
                y: Math.round(Math.max(0, effectDrop.pointer.y - height - Theme.space(2)))
                width: dropTipText.implicitWidth + Theme.space(4)
                height: dropTipText.implicitHeight + Theme.space(2)
                radius: Theme.radiusClip
                color: Theme.scrim

                Text {
                    id: dropTipText
                    anchors.centerIn: parent
                    text: effectDrop.target.message ?? ""
                    color: (effectDrop.target.ok ?? false) ? Theme.scrimText : Theme.warning
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }

            // Drag readout: the delta (or the slip source In/Out) next to the pointer.
            Rectangle {
                objectName: "gestureReadout"
                visible: lanes.view.gestureActive && lanes.view.gestureReadout !== ""
                x: Math.round(Math.min(lanesArea.width - width, Math.max(0, lanes.view.gesturePoint.x + Theme.space(3))))
                y: Math.round(Math.max(0, lanes.view.gesturePoint.y - height - Theme.space(2)))
                width: readoutText.implicitWidth + Theme.space(3)
                height: readoutText.implicitHeight + Theme.space(1.5)
                radius: Theme.radiusClip
                color: Theme.scrim

                Text {
                    id: readoutText
                    anchors.centerIn: parent
                    text: lanes.view.gestureReadout
                    color: Theme.scrimText
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }

            Connections {
                target: lanes.view
                function onContextMenuRequested(clipId, x, y) {
                    clipMenu.openAt(lanes, clipId, Qt.point(x, y))
                }
                // M5l: a double-click on a nested or multi-camera clip opens its sequence in a tab.
                function onClipDoubleClicked(clipId) {
                    if (Nesting.clipInfo(clipId).nested === true)
                        Nesting.openNested(clipId)
                }
                // UX rework (stream T): right clicks on a transition and in a gap (each selected it first).
                function onTransitionContextMenuRequested(transitionId, x, y) {
                    transitionMenu.openAt(lanes, transitionId, Qt.point(x, y))
                }
                function onGapContextMenuRequested(trackId, x, y) {
                    gapMenu.openAt(lanes, Qt.point(x, y))
                }
                // M5m: the Remix tool proposed a length: preview the remix (audio.remix, dry run) and show it.
                function onRemixRequested(clipId, seconds, x, y) {
                    EssentialSound.preview(clipId, seconds)
                    remixPopover.openAt(lanes, Qt.point(x, y))
                }
            }

            RemixPopover {
                id: remixPopover
            }

            ClipContextMenu {
                id: clipMenu
                onRenameRequested: (clipId, point) => renamePopover.openFor(clipId, point, lanesArea.width)
                onNestRequested: nestDialog.openForSelection()
            }

            NestDialog {
                id: nestDialog
            }

            // UX rework (stream T): the transition menu (Set Duration..., alignment, Replace with Default, Set as Default
            // Transition, Clear) and the gap menu (Ripple Delete).
            TransitionContextMenu {
                id: transitionMenu
                onDurationRequested: info => transitionDurationDialog.openFor(info)
            }
            GapContextMenu {
                id: gapMenu
                onFailed: message => root.messageRequested(message)
            }

            RenameClipPopover {
                id: renamePopover
            }

            FocusRing {
                anchors.margins: 0            // drawn on the lane edge: the tracks area clips anything outside
                targetRadius: Theme.radiusClip
                visible: lanesArea.activeFocus && !lanesArea.pointerFocus
            }
        }

        // ---- Keyboard and screen-reader access to the clips ----
        // The lanes are one scene-graph item; this list is their semantic twin (ProjectController.clipList): each
        // row is a clip with a spoken name. Its rows draw nothing and it takes no pointer input, so it lies over the
        // lanes invisibly; it takes keyboard focus (Tab after the lanes) and rings the lanes while it has it: ←/→ previous/next clip on the track, ↑/↓ the nearest clip on the track above/below, [ and ]
        // choose the head or tail edge, Ctrl+←/→ (⌥←/→ on macOS) trim that edge one frame, with Shift five frames
        // (Premiere's Trim Backward/Forward). Esc clears the chosen edge.
        FocusScope {
            id: clipAccess
            objectName: "timelineClipAccess"
            x: lanesArea.x
            width: lanesArea.width
            height: parent.height
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Timeline clips")

            FocusRing {
                anchors.margins: 0
                targetRadius: Theme.radiusClip
                visible: clipAccess.activeFocus
            }

            ListView {
                id: clipList
                objectName: "timelineClipList"
                anchors.fill: parent
                focus: true
                interactive: false
                keyNavigationEnabled: false
                model: ProjectController.clipList
                currentIndex: ProjectController.clipList.currentRow
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Timeline clips")
                Accessible.description: qsTr("Arrow keys move between clips; [ and ] choose an edge; Ctrl+Left/Right trims it")

                readonly property int trimModifier: Qt.platform.os === "osx" ? Qt.AltModifier : Qt.ControlModifier

                function reveal() {
                    const clip = ProjectController.selectedClip
                    if (clip.id !== undefined)
                        lanes.view.revealClip(clip.id)
                }
                function handles(event) {
                    const mods = event.modifiers & ~Qt.KeypadModifier
                    const arrows = event.key === Qt.Key_Left || event.key === Qt.Key_Right
                    if (arrows && (mods === Qt.NoModifier || mods === trimModifier || mods === (trimModifier | Qt.ShiftModifier)))
                        return true
                    if ((event.key === Qt.Key_Up || event.key === Qt.Key_Down) && mods === Qt.NoModifier)
                        return true
                    return event.key === Qt.Key_BracketLeft || event.key === Qt.Key_BracketRight
                           || (event.key === Qt.Key_Escape && ProjectController.selectedEdge !== "")
                }

                onActiveFocusChanged: {
                    if (activeFocus && ProjectController.clipList.currentRow < 0) {
                        ProjectController.selectAdjacentClip(1)
                        reveal()
                    }
                }
                Keys.onShortcutOverride: event => event.accepted = clipList.handles(event)
                Keys.onPressed: event => {
                    if (!clipList.handles(event))
                        return
                    const mods = event.modifiers & ~Qt.KeypadModifier
                    const trimming = (mods & clipList.trimModifier) !== 0
                    const step = (mods & Qt.ShiftModifier) ? 5 : 1
                    switch (event.key) {
                    case Qt.Key_Left:
                        if (trimming)
                            ProjectController.trimSelectedEdge(-step)
                        else
                            ProjectController.selectAdjacentClip(-1)
                        break
                    case Qt.Key_Right:
                        if (trimming)
                            ProjectController.trimSelectedEdge(step)
                        else
                            ProjectController.selectAdjacentClip(1)
                        break
                    case Qt.Key_Up: ProjectController.selectClipOnAdjacentTrack(-1); break
                    case Qt.Key_Down: ProjectController.selectClipOnAdjacentTrack(1); break
                    case Qt.Key_BracketLeft: ProjectController.selectedEdge = "head"; break
                    case Qt.Key_BracketRight: ProjectController.selectedEdge = "tail"; break
                    case Qt.Key_Escape: ProjectController.selectedEdge = ""; break
                    }
                    clipList.reveal()
                    event.accepted = true
                }

                delegate: Item {
                    required property int index
                    required property string accessibleName
                    required property bool selected

                    width: ListView.view.width
                    height: Theme.space(6)
                    Accessible.role: Accessible.ListItem
                    Accessible.name: accessibleName
                    Accessible.selected: selected
                    Accessible.focusable: true
                    Accessible.focused: ListView.isCurrentItem && ListView.view.activeFocus
                }
            }
        }

        // The Timeline list (M5j), over the headers and lanes while it is shown.
        TimelineListView {
            id: timelineList
            anchors.fill: parent
            z: 5
            visible: root.listMode
            onCloseRequested: root.listMode = false
            onRevealRequested: clipId => {
                ProjectController.goToSelectedClipStart()
                lanes.view.revealClip(clipId)
            }
        }

        ThinScrollBar {
            anchors.right: parent.right
            height: parent.height
            orientation: Qt.Vertical
            size: lanes.view.height / Math.max(1, lanes.view.contentHeight)
            position: lanes.view.scrollY / Math.max(1, lanes.view.contentHeight)
            onPositionChanged: {
                if (pressed)
                    lanes.view.scrollY = position * lanes.view.contentHeight
            }
        }
    }

    // A drag waiting for a new track (trackDrop): over the ruler for a video track, below the tracks for an audio
    // track, a band names the track and fills until the hold adds it.
    Rectangle {
        id: holdBand
        objectName: "newTrackHoldBand"
        readonly property bool audio: root.holdZone === "audio"
        readonly property real bandHeight: Math.min(Theme.trackHeightAudio, tracksArea.height)
        property real progress: 0
        z: 20
        visible: holdTimer.running
        x: lanesArea.x
        y: audio ? tracksArea.y + Math.max(0, Math.min(lanes.view.contentHeight - lanes.view.scrollY + Theme.trackGap,
                                                       tracksArea.height - bandHeight))
                 : ruler.y
        width: lanesArea.width
        height: audio ? bandHeight : tracksArea.y - ruler.y
        radius: Theme.radiusClip
        color: Theme.accentWash
        border.width: Theme.hairline
        border.color: Theme.accentStroke
        clip: true

        NumberAnimation {
            id: holdFill
            target: holdBand
            property: "progress"
            from: 0
            to: 1
            duration: root.newTrackHoldMs
        }
        Rectangle {
            width: holdBand.width * holdBand.progress
            height: holdBand.height
            radius: holdBand.radius
            color: Theme.accentWash
        }
        Text {
            objectName: "newTrackHoldLabel"
            anchors.centerIn: parent
            text: root.holdZone === "" ? "" : qsTr("Hold to add track %1").arg(lanes.view.nextTrackLabel(root.holdZone))
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
    }

    WheelHandler {
        id: wheel
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            // Alt swaps the wheel axis on some platforms, so read whichever delta is set.
            const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x
            const v = lanes.view
            if (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.AltModifier)) {
                v.zoomAt(v.zoomLevel + delta / 1200, Math.max(0, wheel.point.position.x - lanesArea.x))
            } else if (event.modifiers & Qt.ShiftModifier) {
                v.scrollBy(0, -delta / 2)
            } else {
                v.scrollBy(-delta / 2, 0)
            }
        }
    }

    // UX rework (stream T): the dialogs of the track header and transition menus (over the window: Overlay.overlay).
    AddTracksDialog {
        id: addTracksDialog
    }
    DeleteTrackDialog {
        id: deleteTrackDialog
    }
    TransitionDurationDialog {
        id: transitionDurationDialog
    }

    // ---- Footer: zoom + minimap ----
    Item {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.controlHeightSmall

        Row {
            id: zoomRow
            width: Theme.trackHeaderWidth
            anchors.verticalCenter: parent.verticalCenter
            spacing: 0

            IconButton {
                compact: true
                iconName: "zoom-out"
                iconSize: Theme.iconSm
                tooltip: qsTr("Zoom out")
                command: "sequence.zoomOut"
                onClicked: root.zoomBy(-0.1)
            }
            ValueSlider {
                objectName: "timelineZoomSlider"
                anchors.verticalCenter: parent.verticalCenter
                width: zoomRow.width - 2 * Theme.controlHeightSmall
                from: 0
                to: 1
                value: lanes.view.zoomLevel
                Accessible.name: qsTr("Timeline zoom")
                onMoved: root.zoomBy(value - lanes.view.zoomLevel)
            }
            IconButton {
                compact: true
                iconName: "zoom-in"
                iconSize: Theme.iconSm
                tooltip: qsTr("Zoom in")
                command: "sequence.zoomIn"
                onClicked: root.zoomBy(0.1)
            }
        }

        TimelineMinimap {
            x: lanesArea.x
            width: parent.width - x
            anchors.verticalCenter: parent.verticalCenter
            view: lanes.view
        }
    }
}
