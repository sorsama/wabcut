import QtQuick
import QtQuick.Dialogs
import QtQuick.Window

// Wabcut main window (design language v3): the title bar (TopBar) with the menu bar (File … Help; UX rework: it replaced
// the left rail) and the workspace tabs, and a docked workbench of panels with gutters you can drag
// (docs/design-language.md, "Geometry"). Main also hosts the Settings window (openSettings) and what the menus ask of
// the window (menuAction).
//
// Frame: frameless with custom chrome by default; Preferences.nativeFrame switches to the platform's decorations.
// Density: below Theme.compactBelowHeight the shell turns compact (tighter panel chrome and track heights) and gives
// the reclaimed height to the timeline.
// Workspaces (0.1.3, panels/workspace/Workspaces.qml): every workspace is the user's, made from a preset (Edit, Color,
// Audio, Captions, Export) or as a copy of another, and each keeps its own panel layout: a tree of panel groups
// (DockManager) the user rearranges by dragging a group's grip or a panel's tab (beside another group, into its tabs,
// along an edge, or into a window of its own) and changes by closing panels (their tabs' ×, the close button in their
// header) and opening them again (the Window menu, the panel focus keys). The panels are declared once
// (panelPool) and move into the group they belong to, so they keep their state; the title bar's tabs create, rename,
// reorder, reset and delete workspaces.
Window {
    id: window

    // 0.1.3 workspaces: the current workspace's id (100 up). Setting one of the former built-in ids (0 Edit, 1 Audio,
    // 2 Export, 3 Color, 4 Captions: the workspace keys, the welcome, Settings, the tour) opens the first workspace made
    // from that preset, making one when there is none (openPreset).
    property int workspace: Workspaces.firstId
    readonly property var workspaceSpec: Workspaces.resolve(workspace)
    /// The former built-in id of the current workspace's preset (the welcome and the tour compare with it).
    readonly property int workspacePreset: Workspaces.legacyIdOf(workspace)
    readonly property bool timelineShown: docking.isOpen("timeline")
    /// The browser panel shown in the group of the browser panels (the left column), else "" (the tests read it; the
    /// name is the former left rail's).
    readonly property string railPanel: {
        const g = docking.groupOfColumn("left")
        const key = g ? docking.shownPanel(g.group) : ""
        return Workspaces.isBrowserPanel(key) ? key : ""
    }
    /// The browser panel shown (railPanel).
    readonly property string browserPanel: railPanel
    onWorkspaceChanged: {
        if (Workspaces.find(workspace) === null) {
            // A former built-in id: the first workspace made from its preset; anything else (deleted): the first one.
            const preset = Workspaces.legacyPresets[workspace]
            if (preset !== undefined)
                window.openPreset(preset)
            else
                window.workspace = Workspaces.all[0].id
            return
        }
        window.loadWorkspace()
        sliceTransition.play()
        Qt.callLater(window.keepFocus)
    }
    /// Opens the first workspace made from `preset` ("edit", "color", "audio", "captions", "export"), making one when
    /// there is none.
    function openPreset(preset: string) {
        let id = Workspaces.firstOf(preset)
        if (id < 0)
            id = Workspaces.createFromPreset(preset, "")
        window.workspace = id
    }
    // Lays the current workspace out: its panels (DockManager), its floating windows (now, or once the editor shows)
    // and the timeline's volume lanes.
    function loadWorkspace() {
        const floating = docking.load(workspace)
        if (firstFrameShown && !home.opened)
            docking.restoreFloating(floating)
        else
            pendingFloating = floating
        const lanes = Workspaces.resolve(workspace).volumeLanes
        timeline.showVolume = lanes
        if (lanes)
            Qt.callLater(() => timeline.view.revealAudioTracks())
    }
    Connections {
        target: Workspaces
        function onAllChanged() {
            // The current workspace was deleted (its tab's menu): the first one.
            const w = Workspaces.find(window.workspace)
            if (w === null) {
                window.workspace = Workspaces.all[0].id
                return
            }
            // Its layout changed from elsewhere (Reset): lay it out again. The docking's own changes are there already.
            if (!docking.saving && JSON.stringify(w.layout) !== JSON.stringify(docking.tree))
                window.loadWorkspace()
        }
    }
    // M5j: focus is never lost. After a workspace or panel switch, an item that is now hidden (or nothing) gives focus
    // to the timeline when it is open, else to the workspace tabs.
    function keepFocus() {
        const f = window.activeFocusItem
        if (f && f.visible && f.enabled && f !== window.contentItem)
            return
        if (window.timelineShown && timeline.visible)
            timeline.focusLanes(true)
        else
            topBar.focusWorkspaceTabs()
    }
    readonly property bool maximized: visibility === Window.Maximized || visibility === Window.FullScreen
    // The launch Home screen is open; the first-run welcome comes when it closes (M5j).
    property bool welcomeAfterHome: false
    // The first-run performance check is offered once after Home and the welcome (I.7; performanceOffer).
    property bool performanceCheckDue: false
    readonly property bool customFrame: !Preferences.nativeFrame

    // Explain this frame (M5k): a panel of its own, opened next to Effect Controls.
    function showFrameInspector() {
        window.revealPanel("frameInspector")
    }

    // 0.1.3 panel docking: shows `key` (its tab), opening it where it fits when it is closed (DockManager.openPanel).
    function revealPanel(key: string) {
        docking.openPanel(key)
    }

    function showBrowserPanel(key) {
        window.focusPanel(key)
    }

    // UX rework: shows the panel `key` (its tab; opened where it fits when it is closed: DockManager.openPanel), makes
    // its group the active one (DockManager.activeGroup) and puts keyboard focus in it: a browser panel's own focus
    // target (focusContent), the monitor, the timeline's lanes, else the panel itself (its keymap scope applies; Tab
    // goes on into it). The panel focus keys, the Window menu and showBrowserPanel come here.
    function focusPanel(key: string) {
        if (key === "source" || key === "program") {
            monitorArea.show(key)
            return
        }
        window.revealPanel(key)
        docking.setActiveGroup(docking.groupOfPanel(key)?.group ?? "")
        Qt.callLater(() => {
            switch (key) {
            case "library":
                libraryPanel.focusContent()
                return
            case "timeline":
                timeline.focusLanes()
                return
            case "effectControls":
                inspectorPanel.forceActiveFocus(Qt.OtherFocusReason)
                return
            }
            // The other panels are LazyPanels (M5n): focused once created.
            window.panelLoaders[key]?.withItem(panel => {
                if (typeof panel.focusContent === "function")
                    panel.focusContent()
                else
                    panel.forceActiveFocus(Qt.OtherFocusReason)
            })
        })
    }
    readonly property var panelLoaders: ({ effects: effectsLoader, captions: captionsLoader, audio: audioToolsLoader,
                                           markers: markersLoader, shorts: shortsLoader, generate: generateLoader,
                                           takes: takesLoader, essential: essentialLoader, review: reviewLoader,
                                           graphics: graphicsLoader, highlights: highlightsLoader,
                                           mediaBrowser: mediaBrowserLoader, metadata: metadataLoader,
                                           production: productionLoader, scopes: scopesLoader,
                                           captionStyle: captionStyleLoader, frameInspector: frameInspectorLoader,
                                           mixer: mixerLoader, export: exportLoader, color: colorLoader })
    // UX rework (Premiere's Window menu): a closed panel opens as a tab of the active group (the group clicked last;
    // none: where it fits, DockManager.openPanel) and shows; an open one comes to the front where it is. Either way it
    // gets keyboard focus.
    function openPanelFromWindowMenu(key: string) {
        docking.openPanelInActiveGroup(key)
        window.focusPanel(key)
    }
    // UX rework: Maximize / Restore Frame for the group `gid` (with a group maximized, any toggle restores; keyboard focus
    // follows, see below). No group to maximize: the toast says how to pick one.
    function toggleMaximizeFrame(gid: string) {
        if (docking.maximizedGroup === "" && gid === "") {
            toast.show(qsTr("Click a panel first: Maximize Frame works on the panel group you clicked last"))
            return
        }
        docking.toggleMaximize(gid)
    }
    // UX rework: whatever maximized a group (Shift+`, `, a double-click on a tab, the tab menu), keyboard focus left in a
    // group that hides now moves into the maximized one: hidden panels keep their focus, and keys would edit what the
    // user cannot see.
    Connections {
        target: docking

        function onMaximizedGroupChanged() {
            const gid = docking.maximizedGroup
            if (gid !== "" && docking.groupOfItem(window.activeFocusItem) !== gid)
                window.focusPanel(docking.shownPanel(gid))
        }
    }
    // UX rework: Window > Workspaces > Reset to Saved Layout - the current workspace's preset layout again (its floating
    // windows dock back); Undo on the toast puts the layout as it was back.
    function resetToSavedLayout() {
        const id = window.workspace
        const before = Workspaces.find(id)
        if (!before)
            return
        const layout = Workspaces.copy(before.layout)
        const floating = Workspaces.copy(before.floating)
        docking.reset()
        toast.show(qsTr("The %1 workspace is back to its saved layout").arg(before.name), undefined, {
            actionText: qsTr("Undo"),
            action: () => {
                if (Workspaces.find(id))
                    Workspaces.update(id, { layout: layout, floating: floating })
            }
        })
    }
    // UX rework: a problem the user must see (an open or save that failed): a warning toast, or Home's message strip
    // while Home is open (`options`: Toast.show's, e.g. a Save As… action; Home's strip offers it too).
    function showProblem(text: string, options: var) {
        const message = text !== "" ? text : qsTr("Something went wrong, and Wabcut could not say what.")
        window.notify(message, Theme.warning, options)
    }
    // UX rework: a message that must not go unseen (a problem, background work that ended): the editor's toast
    // (Toast.show's arguments), or Home's message strip while Home covers the editor - the toast would run out unseen
    // behind it - with the same action. The colour gives the strip's tone (danger, warning, else info).
    function notify(message: string, color: var, options: var) {
        if (!home.opened) {
            toast.show(message, color, options)
            return
        }
        const tone = color === undefined ? "info" : Qt.colorEqual(color, Theme.danger) ? "danger"
                                                  : Qt.colorEqual(color, Theme.warning) ? "warning" : "info"
        home.showMessageWithAction(message, tone, options)
    }
    // UX rework: Save failed (`then`: what was to follow): say why and offer Save As….
    function saveFailed(then: var) {
        window.showProblem(ProjectFile.lastError, {
            actionText: qsTr("Save As…"),
            action: () => {
                saveAsDialog.then = then
                saveAsDialog.open()
            }
        })
    }

    width: Theme.windowDefaultWidth
    height: Theme.windowDefaultHeight
    minimumWidth: Theme.windowMinWidth
    minimumHeight: Theme.windowMinHeight
    visible: true
    // "● name — Wabcut" while there are unsaved changes (ProjectFile, I.1).
    title: ProjectFile.windowTitle
    flags: customFrame ? Qt.Window | Qt.FramelessWindowHint | Qt.WindowMinMaxButtonsHint : Qt.Window
    color: Theme.canvas

    // The demo project's library and the TopBar readouts are still mock (MockProject); the sequence, selection,
    // playhead, history, project file (ProjectFile) and playback (Playback) are real.
    MockProject {
        id: project
        objectName: "mockProject"
        // Real transport state for the panels that still read the mock (I.1).
        playing: Playback.playing
        shuttleSpeed: Playback.speed
    }

    // M5n startup: the panels of the other browser tabs and workspaces (LazyPanel) are created after the first frame,
    // in the background between frames, or when first shown, and then kept. An engine that cannot create in the
    // background (no incubation controller: a plain QQmlEngine, as in the tests and the e2e suites) creates them all up
    // front, as before.
    readonly property bool deferPanels: UiProfile.backgroundLoading()
    property bool preloadPanels: !deferPanels
    readonly property var lazyPanels: [effectsLoader, captionsLoader, audioToolsLoader, markersLoader, generateLoader,
                                       shortsLoader, takesLoader, essentialLoader, reviewLoader, scopesLoader,
                                       captionStyleLoader, frameInspectorLoader, mixerLoader, exportLoader, colorLoader,
                                       graphicsLoader, highlightsLoader, mediaBrowserLoader, metadataLoader,
                                       productionLoader]
    readonly property bool panelsReady: lazyPanels.every(p => p.ready)
    onPanelsReadyChanged: {
        if (panelsReady)
            UiProfile.mark("panels ready")
    }
    // The first frame shown: the startup timings, then the deferred panels.
    property bool firstFrameShown: false
    Connections {
        target: window
        enabled: !window.firstFrameShown
        function onFrameSwapped() {
            window.firstFrameShown = true
            UiProfile.mark("first frame")
            Qt.callLater(() => window.preloadPanels = true)
        }
    }
    // 0.1.3 panel docking: the panels that were in windows of their own at the last exit get them back once the editor
    // shows (after Home), not over the Home screen.
    property var pendingFloating: null
    readonly property bool floatingDue: pendingFloating !== null && firstFrameShown && !home.opened
    onFloatingDueChanged: {
        if (floatingDue) {
            const floating = pendingFloating
            pendingFloating = null
            docking.restoreFloating(floating)
        }
    }
    // A window that shows no frame for a while (minimised at start, a platform that renders nothing): the panels come
    // anyway.
    Timer {
        interval: 2000
        running: window.deferPanels && !window.preloadPanels
        onTriggered: window.preloadPanels = true
    }
    // UX rework (stream L): Settings > General "Reopen the last project" at launch. Opening is synchronous and nothing
    // paints while it runs, so the window shows first: Home with "Opening project" (the chase loader, from the first
    // frame), then the project opens like Home's Open (home.runBusy) once a frame is on screen (or after 2 s without
    // one). Home closes when it is open, else stays with the reason.
    property url launchReopen
    function reopenAtLaunch(file: url) {
        home.busyText = qsTr("Opening project")
        window.launchReopen = file
        if (window.firstFrameShown)
            window.runLaunchReopen()
    }
    function runLaunchReopen() {
        const file = window.launchReopen
        if (file.toString() === "")
            return
        window.launchReopen = ""
        home.runBusy(qsTr("Opening project"), () => {
            if (ProjectFile.open(file))
                home.close()
            else
                home.showMessage(ProjectFile.lastError, "danger")
        })
    }
    Connections {
        target: window
        function onFirstFrameShownChanged() {
            window.runLaunchReopen()
        }
    }
    Timer {
        interval: 2000
        running: window.launchReopen.toString() !== "" && !window.firstFrameShown
        onTriggered: window.runLaunchReopen()
    }

    // ---- Keyboard: KeymapDispatcher (docs/keymap.md, "App integration") ----
    // Panels declare their KeymapScope; the dispatcher resolves keys against the focused panel and emits
    // commands, routed here: project file commands (ProjectFile), playback (Play/Stop, J/K/L), the timeline view
    // (zoom, tools, snap), then ProjectController (history, selection, playhead, marks).
    // 0.2: Selection Follows Playhead is remembered between sessions (Premiere's menu switch).
    Connections {
        target: ProjectController
        function onSelectionFollowsPlayheadChanged() {
            Preferences.selectionFollowsPlayhead = ProjectController.selectionFollowsPlayhead
        }
    }

    Component.onCompleted: {
        UiProfile.mark("Main.qml completed") // M5n: startup timings (printed with WABCUT_UI_PROFILE)
        KeymapDispatcher.window = window
        ProjectController.selectionFollowsPlayhead = Preferences.selectionFollowsPlayhead // 0.2
        // The workspace chosen in the welcome (M5j) or in Settings: that one, or the first one made from the preset it
        // chose; never one made from Export. Else the first workspace. Its floating windows come once the editor shows.
        let start = Workspaces.startId(Preferences.startWorkspace)
        if (start < 0 || Workspaces.find(start).preset === "export")
            start = Workspaces.all.find(w => w.preset !== "export")?.id ?? Workspaces.all[0].id
        if (window.workspace === start)
            window.loadWorkspace()
        else
            window.workspace = start
        // The app opens on the Home screen (main() asks for it; the tests start on the editor). On first run the
        // welcome follows it (M5j). UX rework: Settings > General "When Wabcut starts: Reopen the last project" opens
        // the newest recent project instead (AppSettings.projectToReopen; never after a crash or with autosaves to
        // recover, and a project Wabcut was started with wins): Home shows "Opening project" until it is open
        // (reopenAtLaunch); if it cannot open, Home stays with the reason.
        if (Home.showAtLaunch) {
            window.performanceCheckDue = PerformanceCheck.offerAtStartup
            const reopen = ProjectFile.launchProject().toString() === "" && !CrashNotice.pending
                           && ProjectFile.recoveryFiles.length === 0 ? AppSettings.projectToReopen(Home.recent) : ""
            home.open()
            window.welcomeAfterHome = true
            if (reopen.toString() !== "")
                window.reopenAtLaunch(reopen)
        }
        // After a crash or an unclean exit, the crash notice first (I.7; it leads to the Recovery dashboard); else the
        // work an unclean exit left in autosaves (ProjectFile scans at startup).
        if (CrashNotice.pending)
            crashNoticeDialog.open()
        else if (ProjectFile.recoveryFiles.length > 0)
            recoveryDialog.open()
        // A .wabcut the app was started with (the installer's opt-in file association): opened like File > Open, with
        // its recovery check; unknown arguments are ignored (I.7).
        const launched = ProjectFile.launchProject()
        if (launched.toString() !== "")
            window.openLaunchedProject(launched)
        // 0.1.3: with AI turned off (Settings > AI), its offline mode stays on, also if something reset it since (the
        // left rail did this before the UX rework).
        if (!Preferences.aiEnabled && !AiController.offlineMode)
            AiController.offlineMode = true
        CommandPalette.setShellActions(window.paletteShellRows) // the palette's shell rows (paletteShellRows)
    }
    // A project file named from outside File > Open's dialog: the launch arguments, a .wabcut opened in Explorer while
    // Wabcut runs (single instance: AppShell.openRequested), File > Open Recent and the palette's "Open Recent" rows.
    // The project the panels show is not read again (its undo history, selection and playhead stay; only Home closes,
    // as Home's own list does); one open next to it comes into the panels with its unsaved changes (nothing closes, so
    // no prompt); any other opens like File > Open, after the unsaved-changes prompt (and the sample project's edits
    // prompt). A failure shows on Home or as a toast.
    function openLaunchedProject(file: url) {
        const open = ProjectFile.indexOfProject(file)
        if (open >= 0) {
            if (open === ProjectFile.activeProject || ProjectFile.switchTo(open))
                home.close()
            else
                window.showProblem(ProjectFile.lastError)
            return
        }
        window.whenSaved(() => window.openProjectNow(file))
    }
    Connections {
        target: AppShell

        function onOpenRequested(files) {
            if (files.length > 0)
                window.openLaunchedProject(files[0])
        }
    }

    // The Source Monitor's keymap scope (Premiere): with it focused, marks, transport and Reverse Match Frame act on
    // the source (M5h).
    function sourceHasFocus() {
        return window.activeFocusItem !== null && KeymapDispatcher.scopeAt(window.activeFocusItem) === "sourceMonitor"
    }
    function timelineHasFocus() {
        return window.activeFocusItem !== null && KeymapDispatcher.scopeAt(window.activeFocusItem) === "timeline"
    }
    // Insert / Overwrite from the Source Monitor. A four-point edit whose ranges differ asks how to fit, unless
    // "Always use this choice" was ticked (Preferences.fitClipChoice).
    function sourceEdit(insert, fit) {
        const result = SourceMonitor.edit(insert, fit === undefined ? Preferences.fitClipChoice : fit)
        if (result === "fit")
            fitClipDialog.ask(insert)
        else if (result === "")
            timeline.revealPlayhead()
    }

    function runCommand(commandId) {
        if (commandId.startsWith("ext:"))
            return // Extensions runs these (C.12: KeymapDispatcher.commandTriggered)
        if (window.sourceHasFocus() && SourceMonitor.runCommand(commandId))
            return
        // P.2 (E6): the monitors' keys (Loop, Play In to Out, Nudge Selected Object, ...): the Monitors bridge.
        if (Monitors.runCommand(commandId, window.sourceHasFocus()))
            return
        // M5o (C.13): New Layer (Ctrl+T ...), Bring / Send, Select Next / Previous Layer.
        if (commandId.startsWith("graphics.") && Graphics.runCommand(commandId)) {
            if (commandId.startsWith("graphics.new"))
                window.showBrowserPanel("graphics")
            return
        }
        // Gaming (M5o): Slow-mo / Punch-in at Playhead.
        if (commandId.startsWith("moments.") && Moments.runCommand(commandId))
            return
        // P.2 (E5): Premiere's editing and sequence commands (Video Options, Preferences, ...): the Editing bridge.
        if (Editing.runCommand(commandId))
            return
        switch (commandId) {
        // 0.2: Premiere's Show Video / Audio Fade Handles (Timeline Display Settings).
        case "timeline.showVideoFadeHandles":
            Preferences.showVideoFadeHandles = !Preferences.showVideoFadeHandles
            return
        case "timeline.showAudioFadeHandles":
            Preferences.showAudioFadeHandles = !Preferences.showAudioFadeHandles
            return
        // 0.1.1: Sequence > Render In to Out (Enter); Enter again while rendering stops it.
        case "sequence.renderInToOut":
            if (MediaReadiness.rendering)
                MediaReadiness.cancelRender()
            else
                MediaReadiness.renderInToOut()
            return
        // Source editing and the Program Monitor's buttons (M5h)
        case "clip.insert":
            window.sourceEdit(true)
            return
        case "clip.overwrite":
            window.sourceEdit(false)
            return
        case "sequence.lift":
            ProgramTools.lift()
            return
        case "sequence.extract":
            ProgramTools.extract()
            return
        // Markers 2.0 (M5i): M marks the selected clip under the playhead from the Timeline, else the sequence.
        case "marker.add":
            Markers.add(window.timelineHasFocus())
            return
        case "marker.goToNext":
        case "marker.goToPrevious":
            Markers.runCommand(commandId)
            timeline.revealPlayhead()
            return
        case "marker.clearSelected":
        case "marker.clearAll":
            Markers.runCommand(commandId)
            return
        // Trim Edit (M5i): the edit points nearest the playhead, in the Trim view.
        case "sequence.trimEdit":
            Trim.runCommand(commandId)
            return
        case "sequence.matchFrame":
            SourceMonitor.matchFrame()
            return
        case "sequence.reverseMatchFrame":
            if (SourceMonitor.reverseMatchFrame())
                timeline.revealPlayhead()
            return
        // Panel focus keys (M5j, keyboard-only workflows): Premiere's Shift+1/3/7 move focus to that panel.
        case "window.projects":
            window.showBrowserPanel("library")
            return
        case "window.effects":
            window.showBrowserPanel("effects")
            return
        case "window.mediaBrowser": // 0.2: Shift+8
            window.showBrowserPanel("mediaBrowser")
            return
        // 0.2: the Effects panel's custom items (EffectPresets: effects.newCustomBin / effects.deleteCustomItem).
        case "effects.newCustomBin":
            EffectPresets.newCustomBin("")
            return
        case "effects.deleteCustomItem": // the Effects panel asks first (deleteCurrentItemRequested)
            EffectPresets.requestDeleteCurrentItem()
            return
        case "window.timeline":
            window.focusPanel("timeline")
            return
        case "window.sourceMonitor":
            monitorArea.show("source")
            return
        case "window.programMonitor":
            monitorArea.show("program")
            return
        // UX rework (stream S): Shift+5, Effect Controls.
        case "window.effectControls":
            window.focusPanel("effectControls")
            return
        // UX rework (stream S): Premiere's Window > Maximize Frame (Shift+`): the active group (else the one with
        // keyboard focus) takes the whole work area; again restores. ` alone: the group under the pointer (a Shortcut).
        case "window.maximizeFrame":
            window.toggleMaximizeFrame(docking.activeGroup !== "" ? docking.activeGroup
                                                                  : docking.groupOfItem(window.activeFocusItem))
            return
        // UX rework (stream S): Window > Workspaces > Reset to Saved Layout (Alt+Shift+0): the workspace's preset
        // layout again, with Undo on the toast.
        case "window.resetToSavedLayout":
            window.resetToSavedLayout()
            return
        // UX rework (stream S): Premiere's File > Close (Ctrl+W) closes the active panel: the one the active group shows
        // (else the one with keyboard focus).
        case "file.close": {
            const gid = docking.activeGroup !== "" ? docking.activeGroup : docking.groupOfItem(window.activeFocusItem)
            const key = docking.shownPanel(gid)
            if (key !== "") {
                docking.closePanel(key)
                Qt.callLater(window.keepFocus)
            }
            return
        }
        // p02 audio: Shift+6 / Shift+9, the Audio Track Mixer and the Audio Clip Mixer (the mixer panel's two views).
        case "window.audioTrackMixer":
        case "window.audioClipMixer":
            window.focusPanel("mixer")
            mixerLoader.withItem(panel => panel.view = commandId === "window.audioClipMixer" ? 1 : 0)
            return
        // Premiere's workspace keys (Alt+Shift+1..9) onto the first workspace made from the matching preset.
        case "window.workspace3":
            window.openPreset("audio")
            return
        case "window.workspace4":
            window.openPreset("captions") // Captions and Graphics
            return
        case "window.workspace5":
            window.openPreset("color")
            return
        case "window.workspace1":
        case "window.workspace2":
        case "window.workspace6":
        case "window.workspace7":
        case "window.workspace8":
        case "window.workspace9":
            window.openPreset("edit") // All Panels, Assembly, Editing, Effects, Essential, Learning: Edit
            return
        case "transport.playStop":
            Playback.togglePlay()
            return
        case "file.exportMedia":
            window.openPreset("export")
            return
        case "file.sendToMediaEncoder": // 0.2: into the export queue, waiting for Start Queue
            window.openPreset("export")
            exportLoader.withItem(panel => panel.sendToQueue())
            return
        case "clip.speedDuration":
            speedDialog.openDialog()
            return
        // 0.1.2: Premiere's G (Audio Gain…) and Shift+G (Audio Channels…) on the selected clips.
        case "clip.audioGain":
            audioGainDialog.active = true;
            (audioGainDialog.item as AudioGainDialog)?.openFor(ClipAudio.selectedClips())
            return
        case "clip.audioChannels":
            AudioChannels.openDialog(ClipAudio.selectedClips())
            return
        // C.11 (M5m): Paste Attributes… / Remove Attributes… open the attributes dialog (bridge `Clipboard`).
        case "edit.pasteAttributes":
            Clipboard.openAttributes("paste")
            return
        case "edit.removeAttributes":
            Clipboard.openAttributes("remove")
            return
        case "file.newProject":
            window.newProject()
            return
        case "file.openProject":
            window.openProject()
            return
        case "file.save":
            window.saveProject(null)
            return
        case "file.saveAs":
            saveAsDialog.then = null
            saveAsDialog.open()
            return
        // UX rework (stream S): File > Save a Copy… (Ctrl+Alt+S): the project as it is now, to another file.
        case "file.saveCopy":
            window.saveCopy()
            return
        // UX rework (stream S): File > Close Project (Ctrl+Shift+W), with one project open too (Home, the demo behind).
        case "file.closeProject":
            window.closeCurrentProject()
            return
        case "file.import":
            importDialog.open()
            return
        case "file.exit":
            window.close()
            return
        case "edit.keyboardShortcuts":
            shortcutsDialog.openDialog()
            return
        // UX rework (streams L, S): Edit > Settings… (Ctrl+, / ⌘,: the keymap's key, so it can be rebound).
        case "edit.preferences":
            window.openSettings("", "")
            return
        // Voice-over (M5m, A.8): a pass records onto the first armed audio track.
        case "record.toggle":
            VoiceOver.toggle()
            return
        case "record.start":
            VoiceOver.start()
            return
        case "record.stop":
            VoiceOver.stop()
            return
        // The Library (M5k): Find focuses its search bar; Make Subclip asks for a name.
        case "edit.find":
            window.showBrowserPanel("library")
            Qt.callLater(() => libraryPanel.focusFind())
            return
        case "clip.makeSubclip":
            window.showBrowserPanel("library")
            libraryPanel.makeSubclip()
            return
        }
        if (commandId.startsWith("project.") && Library.runCommand(commandId))
            return
        // M5l: the camera keys act while the Program Monitor shows the multi-camera view (Shift+0 toggles it).
        if (commandId.startsWith("multicam.") && Multicam.runCommand(commandId))
            return
        if (timeline.runCommand(commandId))
            return
        if (ProjectController.runCommand(commandId)
                && (commandId.startsWith("transport.") || commandId.startsWith("marker.goTo")
                    || commandId === "edit.paste" || commandId === "edit.pasteInsert"))
            timeline.revealPlayhead()
    }

    // Command palette: Ctrl/⌘+Shift+P (free in Premiere's Windows and macOS default key sets; docs/keymap.md).
    // A window shortcut until the keymap registry has a command for it.
    Shortcut {
        sequences: ["Ctrl+Shift+P"]
        onActivated: palette.openPalette()
    }
    // UX rework (Premiere): ` maximizes the panel group under the pointer (else the active one); again restores. A window
    // shortcut: the keymap leaves the key unbound (Shift+` is window.maximizeFrame). Text fields keep the character.
    Shortcut {
        objectName: "maximizeFrameShortcut"
        sequence: "`"
        enabled: !home.opened
        onActivated: window.toggleMaximizeFrame(docking.groupUnderPointer() || docking.activeGroup)
    }

    PaletteOverlay {
        id: palette
    }
    AssistantPanel {} // AI editing assistant (AgentController), opened from the palette's "Ask AI…" row

    // Clip Speed / Duration (Ctrl/Cmd+R, the clip menu, the palette).
    SpeedDurationDialog {
        id: speedDialog
    }

    // Keyboard Shortcuts (Ctrl+Alt+K / ⌥⌘K, the File menu, the palette) (M5f).
    KeyboardShortcutsDialog {
        id: shortcutsDialog
    }

    // P.2 (E6): the monitors' windows and dialogs (qml/monitor/): full screen and transmit output.
    MonitorHost {}

    // Fit Clip (M5h): a four-point Insert/Overwrite whose source and sequence ranges differ.
    // P.2 (E5): the dialogs of Premiere's editing and sequence commands (qml/editing/).
    EditingHost {
        onMessageShown: message => toast.show(message)
    }

    FitClipDialog {
        id: fitClipDialog
        onChosen: (fit, remember) => {
            if (remember)
                Preferences.fitClipChoice = fit
            window.sourceEdit(fitClipDialog.insert, fit)
        }
    }

    Connections {
        target: KeymapDispatcher

        function onCommandTriggered(commandId) {
            window.runCommand(commandId)
        }
        function onTransportIntent(kind, speedNum, speedDen, frames) {
            if (window.sourceHasFocus()) {
                SourceMonitor.applyTransportIntent(kind, speedNum, speedDen, frames)
                return
            }
            // Dynamic trimming (M5i): J/K/L with edit points selected trim with playback.
            if (Trim.active) {
                Trim.transportIntent(kind, speedNum, speedDen, frames)
                timeline.revealPlayhead()
                return
            }
            Playback.applyTransportIntent(kind, speedNum, speedDen, frames)
            if (kind === KeymapDispatcher.StepFrames)
                timeline.revealPlayhead()
        }
    }

    // ---- Playback (Playback singleton, I.1) ----
    // J/L climb the ladder from the speed playback really runs at; the timeline keeps the playhead in view.
    Connections {
        target: Playback

        function onTransportChanged() {
            KeymapDispatcher.syncTransportSpeed(Playback.speedNum, Playback.speedDen)
        }
    }
    Connections {
        target: ProjectController
        enabled: Playback.playing

        function onPlayheadChanged() {
            timeline.revealPlayhead()
        }
    }

    // ---- Project file (ProjectFile singleton, I.1): New / Open / Save / Save As / Import, unsaved changes ----
    property var pendingAction: null   // continues New / Open / Quit after the unsaved-changes prompt
    property bool closeConfirmed: false
    // UX rework: the sample project's question was answered with Discard. While the action that follows runs, what it
    // leads to (an import that holds a project, the open after it) does not ask about the same edits again.
    property bool demoAnswered: false
    // UX rework (stream S): a whenSaved request that came while the prompt (or the Save As it led to) waited on another
    // action - a project handed over from Explorer while "Save changes?" asks about a Quit. That action stays (a Quit
    // stays a Quit); this one runs once the prompt is done (asking again if there still are unsaved changes), unless
    // the window is closing. The latest request wins.
    property var deferredAction: null
    function promptBusy(): bool {
        return window.pendingAction !== null && (unsavedDialog.visible || saveAsDialog.visible)
    }
    function runDeferred() {
        if (window.deferredAction === null || window.promptBusy())
            return
        const action = window.deferredAction
        window.deferredAction = null
        if (!window.closeConfirmed)
            window.whenSaved(action)
    }

    // Runs `action` now, or after the user saved or discarded the unsaved changes (Cancel drops it).
    // UX rework (stream S): leaving the sample project after edits (it is never "unsaved": ProjectController.canUndo
    // tells) asks "Discard your changes to the sample project?" (Save As… / Discard / Cancel) the same way.
    // A request while the prompt is busy with another action (deferredAction) waits its turn instead of replacing it.
    function whenSaved(action) {
        if (window.promptBusy()) {
            window.deferredAction = action
            return
        }
        const demoEdits = ProjectFile.demo && ProjectController.canUndo && !window.demoAnswered
        if (!ProjectFile.dirty && !demoEdits) {
            action()
            return
        }
        pendingAction = action
        unsavedDialog.demo = !ProjectFile.dirty && demoEdits
        unsavedDialog.open()
    }
    function continuePending() {
        const action = pendingAction
        pendingAction = null
        if (!action)
            return
        // Still the sample project after the answer: that was Discard (Save As… made it a project of its own).
        window.demoAnswered = ProjectFile.demo
        try {
            action()
        } finally {
            window.demoAnswered = false
        }
    }
    // Saves (asking for a file while untitled), then runs `then`.
    function saveProject(then) {
        if (ProjectFile.untitled || ProjectFile.demo) {
            saveAsDialog.then = then
            saveAsDialog.open()
            return
        }
        if (!ProjectFile.save()) {
            window.saveFailed(then) // never silent (UX rework): why, and Save As…
            return
        }
        if (then)
            then()
    }
    // File > New Project (Ctrl+Alt+N): Home's New project page (formats, name, location, Advanced); its Create runs the
    // unsaved-changes prompt (UX rework). Opened from the editor: Cancel and Esc go back to it, the sample project too.
    function newProject() {
        home.openFromEditor("new")
    }
    function openProject() {
        whenSaved(() => openDialog.open())
    }
    // 0.2 (P.3): a project chosen in the Production panel (or File > Open Project Next to This One) opens next to the
    // ones open and comes into the panels; the others stay loaded with their undo history and unsaved changes. UX
    // rework: over the sample project it takes the sample's place (ProjectFile.openAlongside), so the sample's edits
    // are asked about first.
    function openProjectFile(url) {
        const open = () => {
            if (!ProjectFile.openAlongside(url))
                window.showProblem(ProjectFile.lastError) // never silent (UX rework, stream S)
        }
        if (ProjectFile.demo)
            window.whenSaved(open)
        else
            open()
    }
    // Closes open project `index`, asking first about its unsaved changes (it comes into the panels for that).
    function closeOpenProject(index) {
        const project = ProjectFile.openProjects[index]
        if (!project)
            return
        if (!project.dirty) {
            ProjectFile.closeProject(index)
            return
        }
        if (ProjectFile.switchTo(index))
            whenSaved(() => ProjectFile.closeProject(ProjectFile.activeProject))
    }
    // UX rework (stream S): File > Close Project. With other projects open, the active one closes (closeOpenProject);
    // the only one is replaced by the sample project (ProjectFile.openDemo) and Home shows, after the unsaved-changes
    // prompt.
    function closeCurrentProject() {
        if (ProjectFile.openProjects.length > 1) {
            window.closeOpenProject(ProjectFile.activeProject)
            return
        }
        window.whenSaved(() => {
            if (ProjectFile.openDemo())
                home.open()
            else
                window.showProblem(ProjectFile.lastError)
        })
    }
    // UX rework (stream S): File > Save a Copy… - the file dialog, "<name> copy" in the project's folder suggested.
    function saveCopy() {
        const name = ProjectFile.name !== "" ? ProjectFile.name : qsTr("Untitled project")
        saveCopyDialog.selectedFile = ProjectFile.folder.toString() + "/" + encodeURIComponent(qsTr("%1 copy").arg(name))
                                      + ".wabcut"
        saveCopyDialog.open()
    }
    // File > Home: Back and Esc go back to the project the user was in, the sample project too (UX rework).
    function showHome() {
        home.openFromEditor("home")
    }
    // File > Import and drops on the Library. A .wabcut project among the files is opened instead. One argument: tests
    // and callers reach it through invokeMethod, which needs the exact count (importUrlsThen takes the callback).
    function importUrls(urls) {
        return window.importUrlsThen(urls, null)
    }
    // importUrls with `after` (the Source Monitor's drop opens the first file with it): it runs with the import's result
    // once the import has run: at once, or after the unsaved-changes answer (then { pending: true } is returned).
    function importUrlsThen(urls, after) {
        // C.13 (M5o): Lottie animations become graphic clips, with their own report. 0.2: Photoshop files open Import
        // Layered File (in turn when several come together).
        const rest = []
        const lottie = []
        const layered = []
        for (const url of urls) {
            if (GraphicsImport.isLottie(url))
                lottie.push(url)
            else if (Library.isLayeredFile(url))
                layered.push(url)
            else
                rest.push(url)
        }
        // UX rework (stream S): media, timelines or a project imported over the edited sample project take its place (a
        // new project, or the one opened): its edits are asked about first (whenSaved), before anything is imported, and
        // the whole import runs after the answer.
        if (rest.length > 0 && ProjectFile.demo && ProjectController.canUndo && !window.demoAnswered) {
            whenSaved(() => window.importUrlsThen(urls, after))
            return ({ pending: true })
        }
        // Those go first (over the sample project they start the new project), then the animations and the layered
        // files go into the project that is open then.
        const result = rest.length > 0 ? window.importFilesNow(rest) : ({})
        for (const url of lottie)
            GraphicsImport.importLottie(url)
        if (layered.length > 0)
            importLayeredFileDialog.openFor(layered)
        if (after)
            after(result)
        return result
    }
    function importFilesNow(urls) {
        const result = ProjectFile.importFiles(urls)
        if (result.projects && result.projects.length > 0)
            whenSaved(() => window.openProjectNow(result.projects[0]))
        return result
    }
    // Opens the project `file` (File > Open and the other ways in); Home closes, or (never silent) the reason shows.
    function openProjectNow(file: url): bool {
        if (ProjectFile.open(file)) {
            home.close()
            return true
        }
        window.showProblem(ProjectFile.lastError)
        return false
    }

    // Whether the window may close now. 0.2 (P.3): else every open project with unsaved changes is asked about in turn
    // (it comes into the panels first); each answer closes it and tries to close the window again.
    function readyToClose() {
        if (window.closeConfirmed)
            return true
        const pending = ProjectFile.firstDirtyProject()
        if (pending < 0)
            return true
        if (!ProjectFile.switchTo(pending))
            return false
        window.whenSaved(() => {
            if (ProjectFile.openProjects.length > 1)
                ProjectFile.closeProject(ProjectFile.activeProject)
            else
                window.closeConfirmed = true
            window.close()
        })
        return false
    }
    onClosing: close => {
        // A voice-over pass ends first and keeps its take: it goes into the project, so the save question covers it.
        if (VoiceOver.recording)
            VoiceOver.stop()
        if (!window.readyToClose())
            close.accepted = false
    }

    Connections {
        target: ProjectFile

        function onImportReport(report) {
            importReportDialog.show(report)
        }
        function onOfflineMediaFound(count) {
            relinkDialog.open()
        }
        function onRecoveryAvailable() {
            recoveryDialog.open()
        }
    }

    // ---- The menu bar (UX rework, stream S): the title bar's menus (panels/menubar/, AppMenuBar) run keymap commands
    // (runCommand above); what only the window can do comes here, once the menus have closed ----
    /// A menu bar action (AppMenuBar.actionRequested): `argument` is its url, panel key, workspace id or Help item.
    function menuAction(action: string, argument: var) {
        switch (action) {
        case "home":
            window.showHome()
            return
        case "openRecent":
            window.openLaunchedProject(argument) // the project open now is not read again
            return
        case "openAlongside":
            openAlongsideDialog.open()
            return
        case "revert":
            menuConfirm.ask(qsTr("Revert to the saved version?"),
                            qsTr("Your unsaved changes and the undo history are lost; the project opens as it was last saved."),
                            qsTr("Revert"), () => {
                if (!ProjectFile.revert())
                    window.showProblem(ProjectFile.lastError)
            })
            return
        case "importLottie":
            lottieDialog.open()
            return
        case "exportTimeline":
            exportTimelineDialog.openForCurrent()
            return
        case "relink":
            relinkDialog.open()
            return
        case "recover":
            recoveryDialog.openDashboard()
            return
        case "collectFiles":
            collectFilesDialog.active = true;
            (collectFilesDialog.item as CollectFilesDialog)?.open()
            return
        case "openPanel":
            window.openPanelFromWindowMenu(String(argument))
            return
        case "workspace":
            window.workspace = Number(argument)
            return
        case "newWorkspace": {
            const id = Workspaces.createFromPreset(String(argument), "")
            window.workspace = id
            topBar.renameWorkspace(id)
            return
        }
        case "duplicateWorkspace": {
            const id = Workspaces.create(Number(argument), "")
            window.workspace = id
            topBar.renameWorkspace(id)
            return
        }
        case "renameWorkspace":
            topBar.renameWorkspace(Number(argument))
            return
        case "deleteWorkspace": {
            const id = Number(argument)
            menuConfirm.ask(qsTr("Delete the %1 workspace?").arg(Workspaces.label(id)),
                            qsTr("Its panel layout goes with it. Your projects are not touched."),
                            qsTr("Delete"), () => Workspaces.remove(id))
            return
        }
        case "help":
            window.showHelp(String(argument))
            return
        case "sample":
            window.whenSaved(() => {
                if (!ProjectFile.openDemo())
                    window.showProblem(ProjectFile.lastError)
            })
            return
        case "about":
            aboutDialog.open()
            return
        }
    }
    MenuConfirmDialog {
        id: menuConfirm
    }
    AboutDialog {
        id: aboutDialog
    }
    // File > Save a Copy… (saveCopy()): the project as it is now goes to another file; this one stays open as it was.
    FileDialog {
        id: saveCopyDialog
        objectName: "saveCopyDialog"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "wabcut"
        nameFilters: [qsTr("Wabcut projects (*.wabcut)")]
        currentFolder: ProjectFile.folder
        onAccepted: {
            if (ProjectFile.saveCopy(selectedFile))
                toast.show(qsTr("Copy saved: %1").arg(decodeURIComponent(selectedFile.toString().split("/").pop())))
            else
                window.showProblem(ProjectFile.lastError)
        }
    }

    // ---- The Settings window (0.1.3; hosted here since the UX rework removed the left rail) ----
    // Created on first use, then kept (its pages remember where they were scrolled). Opened from Edit > Settings…
    // (edit.preferences: Ctrl+, / ⌘,), Home's Settings button, the transcript's "set up transcription" (AI) and the
    // command palette's Settings rows (AppSettings.openRequested: a page and a row). It is a popup over the window, so it
    // works while Home covers the editor.
    Loader {
        id: settingsLoader
        objectName: "settingsWindowLoader"
        active: false
        sourceComponent: SettingsWindow {}
    }
    // Its Audio "Open…" and the Keyboard page's shortcuts editor and trainer (after it has closed).
    Connections {
        target: settingsLoader.item as SettingsWindow

        function onAudioSettingsRequested() {
            window.openAudioSettings()
        }
        function onHelpRequested(what: string) {
            window.showHelp(what)
        }
    }
    /// Opens the Settings window on page `key` ("" keeps the page it showed last; SettingsWindow.categories) and, with a
    /// `row` (an AppSettings.index row key, "" for none), shows that setting.
    function openSettings(key: string, row: string) {
        settingsLoader.active = true
        // Its own statement: a line that starts with "(" would continue the one above ("true(...)").
        const settings = settingsLoader.item as SettingsWindow
        if (!settings)
            return
        if (row !== "")
            settings.revealSetting(key, row)
        else
            settings.openAt(key)
    }
    /// Settings > AI (the transcript panel's "set up transcription").
    function openAiPreferences() {
        window.openSettings("ai", "")
    }
    Connections {
        target: AppSettings

        function onOpenRequested(page, row) {
            window.openSettings(page, row)
        }
    }
    // Settings > General's autosave interval (0: ProjectFile's own). From startup, not with the Settings window.
    Binding {
        target: ProjectFile
        property: "autosaveIntervalSeconds"
        value: Preferences.autosaveSeconds
        when: Preferences.autosaveSeconds > 0
        restoreMode: Binding.RestoreNone
    }

    FileDialog {
        id: openDialog
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Wabcut projects (*.wabcut)"), qsTr("All files (*)")]
        currentFolder: ProjectFile.folder
        onAccepted: window.openProjectNow(selectedFile)
    }
    FileDialog {
        id: openAlongsideDialog
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Wabcut projects (*.wabcut)"), qsTr("All files (*)")]
        currentFolder: ProjectFile.folder
        onAccepted: window.openProjectFile(selectedFile)
    }
    FileDialog {
        id: saveAsDialog
        property var then: null
        fileMode: FileDialog.SaveFile
        defaultSuffix: "wabcut"
        nameFilters: [qsTr("Wabcut projects (*.wabcut)")]
        currentFolder: ProjectFile.folder
        onAccepted: {
            const next = then
            then = null
            Qt.callLater(window.runDeferred) // a request that waited on the prompt's Save As (whenSaved)
            if (!ProjectFile.saveAs(selectedFile)) {
                window.saveFailed(next) // never silent (UX rework): why, and Save As… again (another place)
                return
            }
            if (next)
                next()
        }
        onRejected: {
            then = null
            Qt.callLater(window.runDeferred)
        }
    }
    FileDialog {
        id: importDialog
        fileMode: FileDialog.OpenFiles
        nameFilters: ProjectFile.importNameFilters
        currentFolder: ProjectFile.folder
        onAccepted: window.importUrls(selectedFiles)
    }
    UnsavedChangesDialog {
        id: unsavedDialog
        onSaveChosen: window.saveProject(() => window.continuePending())
        onDiscardChosen: window.continuePending()
        onClosed: Qt.callLater(window.runDeferred) // a request that waited on the prompt (whenSaved)
    }
    ImportLayeredFileDialog {
        id: importLayeredFileDialog
    }
    ImportReportDialog {
        id: importReportDialog
    }
    ExportTimelineDialog {
        id: exportTimelineDialog
        onExported: report => importReportDialog.show(report)
    }
    RelinkDialog {
        id: relinkDialog
    }
    RecoveryDialog {
        id: recoveryDialog
    }
    // "Wabcut closed unexpectedly" (I.7): Recovery, the crash reports folder and a diagnostic summary. Closed without
    // Recovery, the autosave prompt still follows when there is work to recover.
    CrashNoticeDialog {
        id: crashNoticeDialog
        onRecoveryRequested: recoveryDialog.openDashboard()
        onClosed: {
            if (!recoveryDialog.visible && ProjectFile.recoveryFiles.length > 0)
                recoveryDialog.open()
        }
    }
    // This session's renderer goes into its crash diagnostics (the summary a later notice copies).
    RendererInfo {
        window: window
        onInfoChanged: {
            CrashNotice.recordRenderer(api, adapter)
            PerformanceCheck.graphics = summary
        }
    }
    Loader {
        id: collectFilesDialog
        objectName: "collectFilesDialogLoader"
        active: false
        sourceComponent: CollectFilesDialog {}
    }
    // Creator superpowers (AI-11), loaded on first use: Sequence > Rough Cut from Script… (RoughCut.open()) and the
    // clip menu's Auto Zoom… (AutoZoom.open(clip)); Takes.open() shows the Takes panel.
    Loader {
        id: roughCutSheet
        objectName: "roughCutSheetLoader"
        active: false
        sourceComponent: RoughCutSheet {}
    }
    Loader {
        id: autoZoomDialog
        objectName: "autoZoomDialogLoader"
        active: false
        sourceComponent: AutoZoomDialog {}
    }
    // M5l, loaded on first use: Create Multi-Camera Source Sequence… (the Library's menu: Multicam.openCreate), the clip
    // menu's Enhance Speech… and Separate Stems… (ClipAudio.openEnhance / openStems), and the report of Synchronize.
    Loader {
        id: multicamCreateDialog
        objectName: "multicamCreateDialogLoader"
        active: false
        sourceComponent: MulticamCreateDialog {}
    }
    Loader {
        id: enhanceSpeechDialog
        objectName: "enhanceSpeechDialogLoader"
        active: false
        sourceComponent: EnhanceSpeechDialog {}
    }
    Loader {
        id: stemsDialog
        objectName: "stemsDialogLoader"
        active: false
        sourceComponent: StemsDialog {}
    }
    Loader {
        id: syncReportDialog
        objectName: "syncReportDialogLoader"
        active: false
        sourceComponent: SyncReportDialog {}
    }
    // C.11 (M5m), loaded on first use: Edit > Paste Attributes… / Remove Attributes… (Clipboard.openAttributes: the keys,
    // the clip menu).
    Loader {
        id: attributesDialog
        objectName: "attributesDialogLoader"
        active: false
        sourceComponent: AttributesDialog {}
    }
    Connections {
        target: Clipboard

        function onAttributesRequested(mode) {
            attributesDialog.active = true;
            (attributesDialog.item as AttributesDialog)?.openFor(mode)
        }
    }
    // Gaming (M5o, C.15 layout.apply): the layout picker (facecam corner, vertical split, vertical blur fill), loaded on
    // first use beside the Program Monitor.
    Loader {
        id: layoutPicker
        objectName: "layoutPickerLoader"
        active: false
        sourceComponent: LayoutPicker {}
    }
    Connections {
        target: Layouts

        function onOpenRequested() {
            layoutPicker.active = true;
            (layoutPicker.item as LayoutPicker)?.openFor(window.contentItem, monitor)
        }
        function onMessageChanged() {
            if (Layouts.message !== "")
                toast.show(Layouts.message)
        }
    }
    // Gaming (M5o, G.2): Jump / Preview from Highlights keep the playhead in view.
    Connections {
        target: Highlights

        function onRevealRequested() {
            timeline.revealPlayhead()
        }
    }
    // C.13 (M5o): File > Import Lottie… (GraphicsImport.openImport()); the report reuses the import report dialog.
    FileDialog {
        id: lottieDialog
        objectName: "lottieImportDialog"
        // GraphicsImport.openImport's options (Callouts' "Import Lottie sticker…": { saveAsTemplate: true }).
        property var importOptions: ({})
        fileMode: FileDialog.OpenFiles
        nameFilters: GraphicsImport.nameFilters
        currentFolder: ProjectFile.folder
        onAccepted: {
            const chosen = importOptions
            importOptions = ({})
            if (Object.keys(chosen).length === 0) {
                window.importUrls(selectedFiles)
                return
            }
            for (const url of selectedFiles)
                GraphicsImport.importLottie(url, chosen)
        }
        onRejected: importOptions = ({})
    }
    Connections {
        target: GraphicsImport

        function onImported(report) {
            importReportDialog.show(report)
        }
        function onImportRequested(options) {
            lottieDialog.importOptions = options ?? ({})
            lottieDialog.open()
        }
    }
    // M5o: Essential Graphics and the monitor's handles work on one layer.
    Connections {
        target: Graphics

        function onSelectedLayerChanged() {
            if (GraphicCanvas.selectedLayer !== Graphics.selectedLayer)
                GraphicCanvas.selectedLayer = Graphics.selectedLayer
        }
    }
    Connections {
        target: GraphicCanvas

        function onSelectedLayerChanged() {
            if (Graphics.selectedLayer !== GraphicCanvas.selectedLayer)
                Graphics.selectedLayer = GraphicCanvas.selectedLayer
        }
    }
    // M5n (A.9): Modify > Audio Channels… (AudioChannels.openDialog / openForMedia: the clip menu, Effect Controls, the
    // Library), loaded on first use.
    Loader {
        id: audioChannelsDialog
        objectName: "audioChannelsDialogLoader"
        active: false
        sourceComponent: AudioChannelsDialog {}
    }
    // 0.1.2: Audio Gain… (G), loaded on first use.
    Loader {
        id: audioGainDialog
        objectName: "audioGainDialogLoader"
        active: false
        sourceComponent: AudioGainDialog {}
    }
    Connections {
        target: AudioChannels

        function onDialogRequested(mode, keys) {
            audioChannelsDialog.active = true;
            (audioChannelsDialog.item as AudioChannelsDialog)?.openFor(mode, keys)
        }
    }
    // M5n (C.12): Settings > Extensions (Extensions.openManager(): the Settings popover, the palette's "Manage Extensions…").
    Loader {
        id: extensionsManager
        objectName: "extensionsManagerLoader"
        active: false
        sourceComponent: ExtensionsManager {}
    }
    Connections {
        target: Extensions

        function onManagerRequested() {
            extensionsManager.active = true;
            (extensionsManager.item as ExtensionsManager)?.open()
        }
    }
    // M5m (A.6): Settings > Audio plugins (Plugins.openSettings(): the Effects panel's plugin section, the Settings popover).
    Loader {
        id: audioPluginsSettings
        objectName: "audioPluginsSettingsLoader"
        active: false
        sourceComponent: AudioPluginsSettings {}
    }
    Connections {
        target: Plugins

        function onSettingsRequested() {
            audioPluginsSettings.active = true;
            (audioPluginsSettings.item as AudioPluginsSettings)?.open()
        }
    }
    // M5m (A.8): Settings > Audio (microphone, count-in, pre/post-roll, monitoring, latency); VoiceOverSync loads the
    // saved voice-over preferences into the VoiceOver bridge at start-up and saves later changes.
    Loader {
        id: audioSettings
        objectName: "audioSettingsLoader"
        active: false
        sourceComponent: AudioSettings {}
    }
    function openAudioSettings() {
        audioSettings.active = true;
        (audioSettings.item as AudioSettings)?.open()
    }
    VoiceOverSync {}
    Connections {
        target: Multicam

        function onCreateRequested(media) {
            multicamCreateDialog.active = true;
            (multicamCreateDialog.item as MulticamCreateDialog)?.openFor(media)
        }
    }
    Connections {
        target: ClipAudio

        function onEnhanceRequested(clips) {
            enhanceSpeechDialog.active = true;
            (enhanceSpeechDialog.item as EnhanceSpeechDialog)?.openFor(clips)
        }
        function onStemsRequested(clip) {
            stemsDialog.active = true;
            (stemsDialog.item as StemsDialog)?.openFor(clip)
        }
        function onSyncReportChanged() {
            if (ClipAudio.syncReport.ok === undefined)
                return
            syncReportDialog.active = true;
            (syncReportDialog.item as SyncReportDialog)?.open()
        }
    }
    Connections {
        target: Nesting

        function onLibraryRequested() {
            window.showBrowserPanel("library")
        }
    }
    // UX rework (stream T): Reveal in Project (the clip menu, clip.revealInProject), Make Subsequence and Merge Clips
    // bring the Library forward.
    Connections {
        target: Editing

        function onLibraryRequested() {
            window.showBrowserPanel("library")
        }
    }
    // M5m (C.9): a version checkpoint before the assistant's staged plan can be applied.
    Connections {
        target: AgentController

        function onStateChanged() {
            if (AgentController.state === "result")
                History.checkpointBeforeAgent()
        }
    }
    // M5m: Enhance Speech picked in a mixer strip's "+ Effect" menu enhances that track's clips (a clip effect).
    Connections {
        target: AudioMixer

        function onEnhanceClipsRequested(clips) {
            ClipAudio.openEnhance(clips)
        }
    }
    Connections {
        target: RoughCut

        function onOpenRequested() {
            roughCutSheet.active = true;
            (roughCutSheet.item as RoughCutSheet)?.open()
        }
    }
    Connections {
        target: AutoZoom

        function onOpenRequested(clip) {
            autoZoomDialog.active = true;
            (autoZoomDialog.item as AutoZoomDialog)?.openFor(clip)
        }
    }
    Connections {
        target: Takes

        function onOpenRequested() {
            window.showBrowserPanel("takes")
        }
    }
    // Going global (AI-12), loaded on first use: Captions > Translate… (TranslateCaptions.open()), Captions > Dub… and
    // Sequence > Narration from Script… (Dub.open("dub" | "narration")); the assistant's dub.create / narration.create
    // open the Dub dialog prefilled (Dub.proposalRequested) for the user to approve the voice, cost, cap and consent.
    Loader {
        id: translateDialog
        objectName: "translateDialogLoader"
        active: false
        sourceComponent: TranslateDialog {}
    }
    Loader {
        id: dubDialog
        objectName: "dubDialogLoader"
        active: false
        sourceComponent: DubDialog {}
    }
    Connections {
        target: TranslateCaptions

        function onOpenRequested() {
            translateDialog.active = true;
            (translateDialog.item as TranslateDialog)?.open()
        }
    }
    Connections {
        target: Dub

        function onOpenRequested(mode) {
            dubDialog.active = true;
            (dubDialog.item as DubDialog)?.openFor(mode)
        }
        function onProposalRequested() {
            dubDialog.active = true;
            (dubDialog.item as DubDialog)?.openFor(Dub.proposal.kind ?? "dub")
        }
    }
    // Podcast and interview editing (AI-13), loaded on first use: the clip menu's Auto Switch… on a multicam clip
    // (AutoSwitch.open(clip)) and Sequence > Podcast Workflow… (PodcastWorkflow.open()).
    Loader {
        id: autoSwitchDialog
        objectName: "autoSwitchDialogLoader"
        active: false
        sourceComponent: AutoSwitchDialog {}
    }
    Loader {
        id: podcastWorkflowSheet
        objectName: "podcastWorkflowSheetLoader"
        active: false
        sourceComponent: PodcastWorkflowSheet {}
    }
    Connections {
        target: AutoSwitch

        function onOpenRequested(clip) {
            autoSwitchDialog.active = true;
            (autoSwitchDialog.item as AutoSwitchDialog)?.openFor(clip)
        }
    }
    Connections {
        target: PodcastWorkflow

        function onOpenRequested() {
            podcastWorkflowSheet.active = true;
            (podcastWorkflowSheet.item as PodcastWorkflowSheet)?.open()
        }
    }
    // Settings > Media & cache: proxies, preview quality, disk caches (MediaReadiness.openSettings()).
    Loader {
        id: mediaCacheSettings
        objectName: "mediaCacheSettingsLoader"
        active: false
        sourceComponent: MediaCacheSettings {}
    }
    Connections {
        target: MediaReadiness

        function onSettingsRequested() {
            mediaCacheSettings.active = true;
            (mediaCacheSettings.item as MediaCacheSettings)?.open()
        }
    }

    Binding {
        target: Theme
        property: "compact"
        value: shell.height > 0 && shell.height < Theme.compactBelowHeight
    }

    Item {
        id: shell
        objectName: "editorShell"
        anchors.fill: parent
        // Behind the Home screen: nothing in it takes focus, and once Home covers it, it is not drawn.
        enabled: !home.opened
        visible: !(home.opened && home.opacity >= 1)

        TopBar {
            id: topBar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Theme.titleBarHeight
            project: project
            workspace: window.workspace
            onWorkspaceRequested: index => window.workspace = index
            onExportRequested: window.openPreset("export")
            onCommandPaletteRequested: palette.openPalette()
            onSaveRequested: window.saveProject(null)
            docking: docking
            onMenuActionRequested: (action, argument) => window.menuAction(action, argument)
        }

        // The mark and the project's name open the File menu too (the logo is the conventional app menu).
        Item {
            objectName: "fileMenuTarget"
            readonly property point at: topBar.brandArea.mapToItem(topBar, 0, 0)
            x: topBar.x + at.x
            y: topBar.y
            width: topBar.brandArea.width
            height: topBar.height

            Accessible.role: Accessible.ButtonMenu
            Accessible.name: qsTr("File menu")

            TapHandler {
                onTapped: topBar.menuBar.open(0, false)
            }
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
        }

        // UX rework: no left rail - the work area starts at the window's gutter (the Window menu opens the panels).
        Item {
            id: workArea
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: topBar.bottom
            anchors.bottom: parent.bottom
            anchors.leftMargin: Theme.gap
            anchors.rightMargin: Theme.gap
            anchors.bottomMargin: Theme.gap

            // 0.1.3 panel docking: the current workspace's layout (DockManager) in the work area: each group's rectangle and
            // the gutters between them. UX rework: a maximized group (Maximize Frame) is laid out alone, over the whole area.
            readonly property real leastFlexible: Theme.space(30)
            readonly property var dockLayout: docking.layout(docking.maximizedGroup !== "" ? docking.leafOf(docking.maximizedGroup)
                                                                                           : docking.tree,
                                                             width, height, docking.presence(), Theme.gap,
                                                             Theme.dualMonitorMinWidth, leastFlexible)
            /// Where the group `gid` is in the work area ({x, y, width, height}), or null while it takes no room.
            function rectOf(gid: string): var {
                return dockLayout.rects[gid] ?? null
            }
            /// The rectangle around `rects` (nulls skipped); an empty one at the origin for none.
            function union(rects: var): var {
                let out = null
                for (const r of rects) {
                    if (!r)
                        continue
                    if (!out) {
                        out = { x: r.x, y: r.y, width: r.width, height: r.height }
                        continue
                    }
                    const right = Math.max(out.x + out.width, r.x + r.width)
                    const bottom = Math.max(out.y + out.height, r.y + r.height)
                    out.x = Math.min(out.x, r.x)
                    out.y = Math.min(out.y, r.y)
                    out.width = right - out.x
                    out.height = bottom - out.y
                }
                return out ?? { x: 0, y: 0, width: 0, height: 0 }
            }

            // ---- 0.1.3 panel docking: where each panel is ----
            // A panel goes into its group's slot and shows when it is the group's shown tab; a closed panel waits, alive,
            // in panelPool.
            function hostOf(key: string): Item {
                const g = docking.groupOfPanel(key)
                return (g ? docking.hostOf(g.group) : null) ?? panelPool
            }
            function panelShown(key: string): bool {
                const g = docking.groupOfPanel(key)
                return g !== null && docking.hostOf(g.group) !== null && docking.shownPanel(g.group) === key
            }

            // Every panel, declared once. `parent` puts each in the group it is docked in (workArea.hostOf), else here,
            // hidden: a panel keeps its state wherever it goes. Shown ones cross-fade (opacity) as their tabs change.
            Item {
                id: panelPool
                objectName: "panelPool"
                visible: false
                width: workArea.width / 3
                height: workArea.height / 2

                LibraryPanel {
                    id: libraryPanel
                    objectName: "libraryPanel"
                    readonly property bool active: workArea.panelShown("library")
                    parent: workArea.hostOf("library")
                    anchors.fill: parent
                    project: project
                    onImportRequested: importDialog.open()
                    onFilesDropped: urls => window.importUrls(urls)
                    onMetadataPanelRequested: window.showBrowserPanel("metadata")
                    onProductionPanelRequested: window.showBrowserPanel("production")
                    opacity: active ? 1 : 0
                    visible: active || opacity > 0
                    enabled: active
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                }
                LazyPanel {
                    id: effectsLoader
                    name: "effects"
                    parent: workArea.hostOf("effects")
                    anchors.fill: parent
                    shown: workArea.panelShown("effects")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: EffectsPanel {}
                }
                // Transcript and captions (M5e); the panel key stays "captions".
                LazyPanel {
                    id: captionsLoader
                    name: "captions"
                    parent: workArea.hostOf("captions")
                    anchors.fill: parent
                    shown: workArea.panelShown("captions")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: TranscriptPanel {}
                }
                // The transcript's "set up transcription": Settings > AI.
                Connections {
                    target: captionsLoader.item as TranscriptPanel

                    function onSetupRequested() {
                        window.openAiPreferences()
                    }
                }
                // Audio Tools (M5g): loudness, normalize, auto-duck, reduce noise.
                LazyPanel {
                    id: audioToolsLoader
                    name: "audio"
                    parent: workArea.hostOf("audio")
                    anchors.fill: parent
                    shown: workArea.panelShown("audio")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: AudioToolsPanel {}
                }
                // 0.2: the Media Browser (camera cards, spanned clips, Ingest).
                LazyPanel {
                    id: mediaBrowserLoader
                    name: "mediaBrowser"
                    parent: workArea.hostOf("mediaBrowser")
                    anchors.fill: parent
                    shown: workArea.panelShown("mediaBrowser")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: MediaBrowserPanel {}
                }
                LazyPanel {
                    id: markersLoader
                    name: "markers"
                    parent: workArea.hostOf("markers")
                    anchors.fill: parent
                    shown: workArea.panelShown("markers")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: MarkersPanel {}
                }
                LazyPanel {
                    id: generateLoader
                    name: "generate"
                    parent: workArea.hostOf("generate")
                    anchors.fill: parent
                    shown: workArea.panelShown("generate")
                    preload: window.preloadPanels
                    visible: shown
                    enabled: shown
                    sourceComponent: GeneratePanel {}
                }
                Connections {
                    target: GenerateController
                    function onProposalRequested(proposal) { window.revealPanel("generate") }
                    function onShowRequested(clip) {
                        window.revealPanel("generate")
                        generateLoader.withItem(panel => panel.extendClip = clip)
                    }
                }
                // Shorts (AI-9): the best moments as vertical shorts.
                LazyPanel {
                    id: shortsLoader
                    name: "shorts"
                    parent: workArea.hostOf("shorts")
                    anchors.fill: parent
                    shown: workArea.panelShown("shorts")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: ShortsPanel {
                        objectName: "shortsPanel"
                    }
                }
                // Takes (AI-11): lines recorded more than once, compared word by word, the best kept.
                LazyPanel {
                    id: takesLoader
                    name: "takes"
                    parent: workArea.hostOf("takes")
                    anchors.fill: parent
                    shown: workArea.panelShown("takes")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: TakesPanel {}
                }
                // Essential Sound (M5m, A.7): tag clips, one-click presets as ordinary effects, Remix.
                LazyPanel {
                    id: essentialLoader
                    name: "essential"
                    parent: workArea.hostOf("essential")
                    anchors.fill: parent
                    shown: workArea.panelShown("essential")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: EssentialSoundPanel {}
                }
                // Essential Graphics (M5o, C.13): templates, layers, presets, responsive design.
                LazyPanel {
                    id: graphicsLoader
                    name: "graphics"
                    parent: workArea.hostOf("graphics")
                    anchors.fill: parent
                    shown: workArea.panelShown("graphics")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: EssentialGraphicsPanel {}
                }
                // Highlights (M5o, gaming; G.2): the stream's best moments, the reel and the Short.
                LazyPanel {
                    id: highlightsLoader
                    name: "highlights"
                    parent: workArea.hostOf("highlights")
                    anchors.fill: parent
                    shown: workArea.panelShown("highlights")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: HighlightsPanel {}
                }
                // Review (M5m, C.9): review notes and the version history.
                LazyPanel {
                    id: reviewLoader
                    name: "review"
                    parent: workArea.hostOf("review")
                    anchors.fill: parent
                    shown: workArea.panelShown("review")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: ReviewPanel {}
                }
                // 0.2 (P.3): Premiere's Metadata panel (Clip and File sections of the Library's selection).
                LazyPanel {
                    id: metadataLoader
                    name: "metadata"
                    parent: workArea.hostOf("metadata")
                    anchors.fill: parent
                    shown: workArea.panelShown("metadata")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: MetadataPanel {}
                }
                // 0.2 (P.3): Productions and the other open projects.
                LazyPanel {
                    id: productionLoader
                    name: "production"
                    parent: workArea.hostOf("production")
                    anchors.fill: parent
                    shown: workArea.panelShown("production")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: shown || opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: ProductionPanel {}
                }
                Connections {
                    target: productionLoader.item as ProductionPanel

                    function onOpenProjectRequested(file: url) {
                        window.openProjectFile(file)
                    }
                    function onCloseProjectRequested(index: int) {
                        window.closeOpenProject(index)
                    }
                }
                // Scopes (I.5): the Color workspace's left column.
                LazyPanel {
                    id: scopesLoader
                    name: "scopes"
                    parent: workArea.hostOf("scopes")
                    anchors.fill: parent
                    shown: workArea.panelShown("scopes")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: ScopesPanel {}
                }

                // Effect Controls (the Clip Editor).
                InspectorPanel {
                    id: inspectorPanel
                    readonly property bool shownHere: workArea.panelShown("effectControls")
                    parent: workArea.hostOf("effectControls")
                    anchors.fill: parent
                    opacity: shownHere ? 1 : 0
                    visible: opacity > 0
                    enabled: shownHere
                    onExplainRequested: window.showFrameInspector()
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                }
                // M5m: the Captions workspace styles the shown caption track here (C.7). The other workspaces' panels
                // are created when first shown or after the first frame (M5n, LazyPanel).
                LazyPanel {
                    id: captionStyleLoader
                    name: "captionStyle"
                    parent: workArea.hostOf("captionStyle")
                    anchors.fill: parent
                    shown: workArea.panelShown("captionStyle")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: CaptionStylePanel {}
                }
                // Explain this frame (M5k), a tab next to the clip editor.
                LazyPanel {
                    id: frameInspectorLoader
                    name: "frameInspector"
                    parent: workArea.hostOf("frameInspector")
                    anchors.fill: parent
                    shown: workArea.panelShown("frameInspector")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: FrameInspectorPanel {
                        objectName: "frameInspector"
                    }
                }
                Connections {
                    target: frameInspectorLoader.item as FrameInspectorPanel

                    function onCloseRequested() {
                        docking.closePanel("frameInspector")
                    }
                }
                LazyPanel {
                    id: mixerLoader
                    name: "mixer"
                    parent: workArea.hostOf("mixer")
                    anchors.fill: parent
                    shown: workArea.panelShown("mixer")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: AudioMixerPanel {}
                }
                LazyPanel {
                    id: exportLoader
                    name: "export"
                    parent: workArea.hostOf("export")
                    anchors.fill: parent
                    shown: workArea.panelShown("export")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: ExportPanel {}
                }
                LazyPanel {
                    id: colorLoader
                    name: "color"
                    parent: workArea.hostOf("color")
                    anchors.fill: parent
                    shown: workArea.panelShown("color")
                    preload: window.preloadPanels
                    opacity: shown ? 1 : 0
                    visible: opacity > 0
                    enabled: shown
                    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
                    sourceComponent: ColorPanel {}
                }

                // The monitors (M5h): Source and Program side by side in an Edit layout when they fit (Premiere's
                // Editing workspace), else sharing their place with a Source/Program switch (monitorArea).
                SourceMonitorPanel {
                    id: sourceMonitor
                    parent: workArea.hostOf("source")
                    anchors.fill: parent
                    // Two monitors sharing a narrow place: the one chosen with the switch.
                    visible: workArea.panelShown("source") && !(monitorArea.single && monitorArea.current !== "source")
                    single: monitorArea.single
                    onMonitorRequested: which => monitorArea.choose(which)
                    onEditRequested: insert => window.sourceEdit(insert)
                    onEditHovered: (insert, hovered) => {
                        if (hovered)
                            SourceMonitor.previewEdit(insert, Preferences.fitClipChoice)
                        else
                            SourceMonitor.clearPreview()
                    }
                    // UX rework (stream T): files dropped from File Explorer import as File > Import does; the panel
                    // then opens the first one (after the unsaved-changes answer when the sample project had edits).
                    importFiles: (urls, after) => window.importUrlsThen(urls, after)
                }
                MonitorPanel {
                    id: monitor
                    objectName: "monitorPanel"
                    parent: workArea.hostOf("program")
                    anchors.fill: parent
                    visible: workArea.panelShown("program") && !(monitorArea.single && monitorArea.current !== "program")
                    typeTool: timeline.tool === "type"
                    project: project
                    single: monitorArea.single
                    onMonitorRequested: which => monitorArea.choose(which)
                    onPlayheadMoved: timeline.revealPlayhead() // UX rework (stream T): a typed time
                }
                // The timeline, with what can be dropped on it.
                Item {
                    id: timelineHost
                    objectName: "timelineHost"
                    parent: workArea.hostOf("timeline")
                    anchors.fill: parent
                    visible: workArea.panelShown("timeline")

                    TimelinePanel {
                        id: timeline
                        anchors.fill: parent
                        onImportRequested: importDialog.open()
                        onMessageRequested: text => toast.show(text, Theme.warning) // UX rework (stream T)
                    }

                    // Library -> timeline (I.1): a media card dropped on the lanes is edited in at the frame under the
                    // pointer, on the track under it (overwrite, as Premiere; Ctrl/Cmd = insert); the other kind of stream
                    // goes to the matching track, or a free one, never cutting the clips under it (editor::dropMedia).
                    // 0.1.3: with snapping on, the frame snaps to the clip in front (and the playhead, marks, markers), and
                    // an insertion line shows where it lands while the drag is over the lanes (TimelinePanel.trackDrop).
                    // Held over the ruler or below the tracks, a drag adds a track there and lands on it (endDrop(at)
                    // takes that track away again when the drop lands elsewhere). A sequence from the Project panel comes
                    // as application/x-wabcut-media with its own key and is nested where it lands (C.8).
                    DropArea {
                        id: timelineMediaDrop
                        objectName: "timelineDropArea"
                        anchors.fill: timeline
                        keys: ["application/x-wabcut-media", "application/x-wabcut-search-hit", "application/x-wabcut-template"]
                        onEntered: drag => timeline.trackDrop(timelineMediaDrop, drag.x, drag.y)
                        onPositionChanged: drag => timeline.trackDrop(timelineMediaDrop, drag.x, drag.y)
                        onExited: timeline.endDrop()
                        onDropped: drop => {
                            const at = timeline.trackDrop(timelineMediaDrop, drop.x, drop.y)
                            timeline.endDrop(at)
                            if (!at)
                                return
                            // M5o (C.13): a graphics template from Essential Graphics, on the track and frame under the pointer.
                            if (drop.formats.indexOf("application/x-wabcut-template") >= 0) {
                                if (Graphics.instantiate(drop.getDataAsString("application/x-wabcut-template"), at.track, at.time) === "")
                                    drop.accept(Qt.CopyAction)
                                return
                            }
                            // AI-8: a smart search hit inserts just its source range.
                            if (drop.formats.indexOf("application/x-wabcut-search-hit") >= 0) {
                                if (SmartSearch.dropHit(drop.getDataAsString("application/x-wabcut-search-hit"), at.time, at.track,
                                                        !ProjectFile.insertModifierHeld()))
                                    drop.accept(Qt.CopyAction)
                                return
                            }
                            const mediaId = drop.getDataAsString("application/x-wabcut-media")
                            // P.2: Alt- or Shift-drag onto a clip is Premiere's Replace Edit.
                            if (Editing.replaceModifierHeld(false)) {
                                const p = timelineMediaDrop.mapToItem(timeline.view, drop.x, drop.y)
                                if (Editing.replaceDrop(timeline.view.clipAt(p.x, p.y), mediaId, false)) {
                                    drop.accept(Qt.CopyAction)
                                    return
                                }
                            }
                            if (ProjectFile.dropMedia(mediaId, at.time, at.track, !ProjectFile.insertModifierHeld()))
                                drop.accept(Qt.CopyAction)
                        }
                    }

                    // Files from the desktop (Explorer, Finder) -> timeline: imported as a drop on the Project panel
                    // imports them, then edited in one after another from the frame under the pointer, on the track under
                    // it, like a library drop (snapped, with the insertion line; Ctrl/Cmd = insert).
                    DropArea {
                        id: timelineFileDrop
                        objectName: "timelineFileDropArea"
                        anchors.fill: timeline
                        keys: ["text/uri-list"]
                        onEntered: drag => {
                            drag.accepted = drag.hasUrls
                            if (drag.accepted)
                                timeline.trackDrop(timelineFileDrop, drag.x, drag.y)
                        }
                        onPositionChanged: drag => timeline.trackDrop(timelineFileDrop, drag.x, drag.y)
                        onExited: timeline.endDrop()
                        onDropped: drop => {
                            const at = timeline.trackDrop(timelineFileDrop, drop.x, drop.y)
                            timeline.endDrop(at)
                            if (!drop.hasUrls)
                                return
                            drop.accept(Qt.CopyAction)
                            // C.13 (M5o): Lottie animations become graphic clips, with their own report - after the other
                            // files, which may start a new project for them.
                            const files = []
                            const lottie = []
                            for (const url of drop.urls) {
                                if (GraphicsImport.isLottie(url))
                                    lottie.push(url)
                                else
                                    files.push(url)
                            }
                            const time = at ? at.time : ProjectController.playheadText
                            const track = at ? at.track : ""
                            const overwrite = !ProjectFile.insertModifierHeld()
                            const dropNow = () => {
                                if (files.length > 0) {
                                    const result = ProjectFile.dropFiles(files, time, track, overwrite)
                                    if (result.projects && result.projects.length > 0)
                                        window.whenSaved(() => window.openProjectNow(result.projects[0]))
                                }
                                for (const url of lottie)
                                    GraphicsImport.importLottie(url)
                            }
                            // UX rework: files over the edited sample project take its place (a new project, or the
                            // project dropped): whenSaved asks about its edits first, before anything is imported.
                            if (files.length > 0 && ProjectFile.demo)
                                window.whenSaved(dropNow)
                            else
                                dropNow()
                        }
                    }

                    // Source Monitor -> timeline (M5h): its picture (video and audio) or a handle (video only, audio only),
                    // the marked range edited in where it is dropped (overwrite; Ctrl/Cmd = insert), like a library drop
                    // (snapped, with the insertion line: 0.1.3).
                    DropArea {
                        id: timelineSourceDrop
                        objectName: "sourceDropArea"
                        anchors.fill: timeline
                        keys: ["application/x-wabcut-source"]
                        onEntered: drag => timeline.trackDrop(timelineSourceDrop, drag.x, drag.y)
                        onPositionChanged: drag => timeline.trackDrop(timelineSourceDrop, drag.x, drag.y)
                        onExited: timeline.endDrop()
                        onDropped: drop => {
                            const at = timeline.trackDrop(timelineSourceDrop, drop.x, drop.y)
                            timeline.endDrop(at)
                            if (!at)
                                return
                            // P.2: Shift-drag onto a clip is Premiere's Replace Edit (the Source Monitor's clip from its In).
                            if (Editing.replaceModifierHeld(true)) {
                                const p = timelineSourceDrop.mapToItem(timeline.view, drop.x, drop.y)
                                if (Editing.replaceDrop(timeline.view.clipAt(p.x, p.y), "", true)) {
                                    drop.accept(Qt.CopyAction)
                                    return
                                }
                            }
                            if (SourceMonitor.dropAt(at.time, at.track, !ProjectFile.insertModifierHeld(),
                                                     drop.getDataAsString("application/x-wabcut-source")))
                                drop.accept(Qt.CopyAction)
                        }
                    }
                }
            }
            // ---- 0.1.3 panel docking: the groups, where the layout puts them ----
            // One DockSlot per panel group of the current workspace (DockManager.groups): the item its panels go into
            // (they come from panelPool and keep their state); with several panels the slot lays their tabs over the
            // shown panel's tab strip (design language v3).
            Repeater {
                model: docking.groups

                DockSlot {
                    id: groupSlot
                    required property string gid
                    objectName: "dockGroup_" + gid
                    dockKey: gid
                    manager: docking
                    readonly property var place: workArea.rectOf(gid)
                    readonly property var group: docking.leafOf(gid)
                    currentPanel: docking.shownPanel(gid)
                    // Its window's title while it floats.
                    title: Workspaces.panelLabel(currentPanel)
                    x: place ? place.x : 0
                    y: place ? place.y : 0
                    width: place ? place.width : 0
                    height: place ? place.height : 0
                    // UX rework: the monitor a shared place does not show (monitorArea.hiddenGroup) hides with its
                    // group: its glass, outline and hover would otherwise lie over the shown one and take its presses,
                    // drops, ` and hover (DockManager.slotAt and groupUnderPointer skip hidden groups).
                    visible: place !== null && width > 0 && height > 0 && gid !== monitorArea.hiddenGroup
                    // The tabs follow the group's panels, and only when they change (a tab being dragged stays).
                    onGroupChanged: {
                        const next = groupSlot.group ? groupSlot.group.panels : []
                        if (JSON.stringify(next) !== JSON.stringify(groupSlot.tabPanels))
                            groupSlot.tabPanels = next
                    }
                    Component.onCompleted: {
                        groupSlot.tabPanels = groupSlot.group ? groupSlot.group.panels : []
                        docking.register(gid, groupSlot, panelHost)
                    }
                    Component.onDestruction: docking.unregister(gid, groupSlot)

                    Item {
                        id: panelHost
                        objectName: "dockHost_" + groupSlot.gid
                        anchors.fill: parent
                    }

                }
            }

            // Every panel closed: say so, and how to get them back.
            Panel {
                objectName: "noPanelsPlaceholder"
                anchors.fill: parent
                visible: docking.tree === null

                Column {
                    anchors.centerIn: parent
                    spacing: Theme.space(3)

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Every panel of this workspace is closed")
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }
                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: Theme.space(2)

                        PillButton {
                            text: qsTr("Open the Library")
                            variant: "secondary"
                            onClicked: window.showBrowserPanel("library")
                        }
                        PillButton {
                            text: qsTr("Reset the layout")
                            onClicked: docking.reset()
                        }
                    }
                }
            }

            // Monitors (M5h): which ones show, and which one a shared place shows. It covers the docked monitors.
            Item {
                id: monitorArea
                objectName: "monitorArea"

                // Open in this workspace and shown in their group (the Trim view (M5i) takes the Source Monitor's place:
                // DockManager.unavailable).
                readonly property bool sourceShown: workArea.panelShown("source")
                readonly property bool programShown: workArea.panelShown("program")
                // In the main window, not in windows of their own.
                readonly property bool sourceDocked: sourceShown && !(docking.slotOfPanel("source")?.floating ?? false)
                readonly property bool programDocked: programShown && !(docking.slotOfPanel("program")?.floating ?? false)
                // Both docked, each in a place of its own (side by side, or wherever they are docked) ...
                readonly property bool dual: sourceDocked && programDocked && !workArea.dockLayout.merged
                // ... or next to each other in too little room: they share it, one shown, with a switch.
                readonly property bool single: sourceDocked && programDocked && workArea.dockLayout.merged
                // The monitor a single layout shows: "source" or "program".
                property string current: "program"
                // UX rework: the group of the monitor a single layout does not show ("" while each has a place of its
                // own). Its slot hides; the active group and the keyboard focus never stay on it (the one shown takes
                // over), so the Window menu, Shift+`, Ctrl+W and the monitor keys act on the monitor the user sees.
                readonly property string hiddenGroup: single ? (docking.groupOfPanel(current === "source" ? "program" : "source")?.group ?? "")
                                                             : ""
                onHiddenGroupChanged: {
                    if (hiddenGroup === "")
                        return
                    if (docking.activeGroup === hiddenGroup)
                        docking.setActiveGroup(docking.groupOfPanel(current)?.group ?? "")
                    if (docking.groupOfItem(window.activeFocusItem) === hiddenGroup)
                        Qt.callLater(() => monitorArea.current === "source" ? sourceMonitor.focusMonitor() : monitor.focusMonitor())
                }
                // The Source/Program switch in a monitor's header: that monitor shows, and its group is the active one.
                function choose(which: string) {
                    current = which
                    docking.setActiveGroup(docking.groupOfPanel(which)?.group ?? "")
                }
                function placeOf(docked: bool, key: string): var {
                    const slot = docked ? docking.slotOfPanel(key) : null
                    return slot ? workArea.rectOf(slot.dockKey) : null
                }
                readonly property var bounds: workArea.union([placeOf(sourceDocked, "source"), placeOf(programDocked, "program")])
                x: bounds.x
                y: bounds.y
                width: bounds.width
                height: bounds.height

                // Shows (and focuses) a monitor: its tab, opened again when it is closed; its group is the active one.
                function show(which) {
                    window.revealPanel(which)
                    docking.setActiveGroup(docking.groupOfPanel(which)?.group ?? "")
                    current = which
                    Qt.callLater(() => which === "source" ? sourceMonitor.focusMonitor() : monitor.focusMonitor())
                }

                Connections {
                    target: SourceMonitor

                    function onShowSourceRequested() {
                        monitorArea.current = "source"
                    }
                    function onShowProgramRequested() {
                        monitorArea.current = "program"
                    }
                }
            }

            // ---- Draggable gutters (design language v2: the gutters are the splitters) ----
            // One between each two neighbours in a split of the layout (workArea.dockLayout.gutters): dragging it moves
            // their shares of the room (each keeps at least leastFlexible); a double-click gives them even shares.
            Repeater {
                model: workArea.dockLayout.gutters.length

                WorkbenchSplitter {
                    required property int index
                    readonly property var gutter: workArea.dockLayout.gutters[index] ?? null
                    objectName: "dockSplitter"
                    z: 10
                    visible: gutter !== null
                    orientation: gutter && gutter.vertical ? Qt.Vertical : Qt.Horizontal
                    x: !gutter ? 0 : gutter.vertical ? gutter.x : gutter.x - width / 2
                    y: !gutter ? 0 : gutter.vertical ? gutter.y - height / 2 : gutter.y
                    width: !gutter ? 0 : gutter.vertical ? gutter.length : Theme.splitterHit
                    height: !gutter ? 0 : gutter.vertical ? Theme.splitterHit : gutter.length
                    // A deferred drag's guide stops where the sizes will (live drags clamp in shiftWeight).
                    minDelta: live || !gutter ? -Infinity : -Math.max(0, gutter.sizeBefore - workArea.leastFlexible)
                    maxDelta: live || !gutter ? Infinity : Math.max(0, gutter.sizeAfter - workArea.leastFlexible)
                    onActiveChanged: docking.resizing = active
                    onDragged: delta => docking.shiftWeight(gutter.path, gutter.before, gutter.after, delta, gutter.sizeBefore,
                                                           gutter.sizeAfter, workArea.leastFlexible)
                    onFinished: docking.save()
                    onReset: {
                        docking.evenWeights(gutter.path, gutter.before, gutter.after)
                        docking.save()
                    }
                }
            }

            // 0.1.3 panel docking: while a panel's grip is dragged, where it would go lights up (beside a panel, in its
            // place, along an edge, or the window it would get), with what the drop does.
            Item {
                objectName: "dockDropOverlay"
                anchors.fill: parent
                z: 20
                visible: docking.dragged !== null

                Rectangle {
                    id: dropPreview
                    objectName: "dockDropPreview"
                    readonly property bool asWindow: docking.drop.kind === "float"
                    visible: docking.dropRect.width > 0 && docking.dropRect.height > 0
                    x: docking.dropRect.x
                    y: docking.dropRect.y
                    width: docking.dropRect.width
                    height: docking.dropRect.height
                    radius: Theme.radiusPanel
                    // A window of its own looks like one: the panel's surface under a title strip with its name.
                    color: asWindow ? Qt.alpha(Theme.panel, 0.88) : Qt.alpha(Theme.accentFill, 0.18)
                    border.width: Theme.focusRingWidth
                    border.color: Theme.accentStroke

                    Rectangle {
                        visible: dropPreview.asWindow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: parent.border.width
                        height: Theme.controlHeightSmall + Theme.space(2)
                        radius: Theme.radiusPanel
                        color: Qt.alpha(Theme.accentFill, 0.32)

                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: Theme.space(3)
                            anchors.rightMargin: Theme.space(3)
                            text: docking.dragName()
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                        }
                    }
                }
                // What the drop does: in the middle of where the panel would go, else at the top.
                Rectangle {
                    readonly property bool inPreview: dropPreview.visible
                    visible: docking.dropText !== ""
                    x: Math.round(Math.max(Theme.space(2), Math.min(parent.width - width - Theme.space(2),
                                                                    (inPreview ? dropPreview.x + dropPreview.width / 2
                                                                               : parent.width / 2) - width / 2)))
                    y: Math.round(inPreview ? Math.max(dropPreview.y + (dropPreview.asWindow ? Theme.controlHeightSmall + Theme.space(4) : 0),
                                                       dropPreview.y + (dropPreview.height - height) / 2)
                                            : Theme.space(4))
                    width: Math.min(parent.width - Theme.space(4), dropLabel.implicitWidth + Theme.space(6))
                    height: dropLabel.implicitHeight + Theme.space(3)
                    radius: Theme.radiusControl
                    color: Theme.raised
                    border.width: Theme.hairline
                    border.color: Theme.borderStrong

                    Text {
                        id: dropLabel
                        objectName: "dockDropText"
                        anchors.centerIn: parent
                        width: Math.min(implicitWidth, parent.width - Theme.space(6))
                        text: docking.dropText
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.DemiBold
                    }
                }
            }
        }

        DockManager {
            id: docking
            objectName: "docking"
            mainWindow: window
            area: workArea
            rects: workArea.dockLayout.rects
            sharedGroups: workArea.dockLayout.shared
            unavailable: Trim.viewOpen ? ["source"] : []
        }

        // Toasts (design language v2): bottom-right, with a slice edge.
        Toast {
            id: toast
            objectName: "toast"
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: Theme.gap + Theme.space(3)
            anchors.bottomMargin: Theme.gap + Theme.space(3)
            z: 10
        }
        // The Library's results and problems (M5k).
        Connections {
            target: Library

            function onMessageChanged() {
                if (Library.message !== "")
                    toast.show(Library.message)
            }
            // M5l: Reveal nested sequence in Library, a new multi-camera source: the Library comes forward.
            function onItemShown(key) {
                window.showBrowserPanel("library")
            }
        }
        Connections {
            target: Multicam

            function onMessageChanged() {
                if (Multicam.message !== "")
                    toast.show(Multicam.message)
            }
        }
        Connections {
            target: Plugins

            function onMessageChanged() {
                if (Plugins.message !== "")
                    toast.show(Plugins.message)
            }
        }
        Connections {
            target: VoiceOver

            function onErrorChanged() {
                if (VoiceOver.lastError !== "")
                    toast.show(VoiceOver.lastError, Theme.warning)
            }
            function onTakeRecorded(clip) {
                if (clip !== "")
                    ProjectController.selectClip(clip)
            }
        }
        Connections {
            target: Nesting

            function onMessageChanged() {
                if (Nesting.message !== "")
                    toast.show(Nesting.message)
            }
        }
        Connections {
            target: ClipAudio

            function onMessageChanged() {
                if (ClipAudio.message !== "" && ClipAudio.busy === "")
                    toast.show(ClipAudio.message)
            }
        }
        Connections {
            target: ProgramTools

            function onFrameExported(path) {
                toast.show(qsTr("Frame saved next to the project: %1").arg(path.split("/").pop()))
            }
        }
        Connections {
            target: SourceMonitor

            function onMessageChanged() {
                if (SourceMonitor.message !== "")
                    toast.show(SourceMonitor.message, Theme.warning)
            }
        }
        Connections {
            target: Markers

            function onMessageChanged() {
                if (Markers.message !== "")
                    toast.show(Markers.message)
            }
        }
        Connections {
            target: Clipboard

            function onMessageChanged() {
                if (Clipboard.message !== "")
                    toast.show(Clipboard.message)
            }
        }
        Connections {
            target: GraphicsImport

            function onMessageChanged() {
                if (GraphicsImport.message !== "")
                    toast.show(GraphicsImport.message, Theme.warning)
            }
        }
        Connections {
            target: Moments

            function onMessageChanged() {
                if (Moments.message !== "")
                    toast.show(Moments.message, Theme.warning)
            }
        }
        Connections {
            target: AudioChannels

            function onMessageChanged() {
                if (AudioChannels.message !== "")
                    toast.show(AudioChannels.message, Theme.warning)
            }
        }
        // UX rework (stream L): background work is visible wherever you are. A Quick Export says it started (Cancel);
        // an export that ends says so with "Show in <file manager>", or why it failed (the Export panel's queue has the
        // details); Render In to Out says when the previews are ready or why not (Try Again). Watch folders and
        // transcodes report elsewhere; a cancelled job needs no word; a finished export from the Export workspace
        // while its queue is in view is already shown there. While Home covers the editor these go to Home's message
        // strip (window.notify), action included.
        Connections {
            target: ExportController

            function onJobQueued(jobId) {
                const job = ExportController.job(jobId)
                if (job.origin !== "quick")
                    return
                window.notify(qsTr("Exporting %1 in the background").arg(job.title), Theme.accentStroke,
                              { actionText: qsTr("Cancel"), action: () => ExportController.cancel(jobId) })
            }
            function onJobFinished(jobId, succeeded, message) {
                const job = ExportController.job(jobId)
                if (job.origin === "transcode" || job.origin === "watch" || job.state === "cancelled")
                    return
                if (succeeded) {
                    if (job.origin === "export" && workArea.panelShown("export") && !home.opened)
                        return
                    const title = job.title || qsTr("the sequence")
                    const output = job.outputPath ?? "" // kept: the job's row may be gone when the action runs
                    window.notify(qsTr("Exported %1").arg(title), Theme.success,
                                  { actionText: qsTr("Show in %1").arg(ExportController.fileManagerName),
                                    action: () => {
                                        if (ExportController.reveal(jobId))
                                            return
                                        // Its row went meanwhile (Clear Finished): the file it wrote, or why not.
                                        if (ExportController.job(jobId).jobId === undefined
                                                && !ExportController.reveal(output))
                                            window.notify(qsTr("%1 is no longer there: it was moved or deleted.").arg(title),
                                                          Theme.warning, undefined)
                                    } })
                } else {
                    // The queue is in the editor: from Home, Show the queue goes there. The Export workspace shows it
                    // (or the Export panel where this workspace has one already): the user's layout never gets an
                    // Export panel docked into it.
                    window.notify(qsTr("Export failed: %1").arg(job.detail || message), Theme.danger,
                                  { actionText: qsTr("Show the queue"),
                                    action: () => {
                                        home.close()
                                        if (docking.isOpen("export"))
                                            window.focusPanel("export")
                                        else
                                            window.openPreset("export")
                                    },
                                    timeoutMs: 10000 })
                }
            }
        }
        Connections {
            target: MediaReadiness

            function onRenderFinished(outcome, message) {
                if (outcome === "done")
                    window.notify(qsTr("Previews rendered: In to Out plays in real time"), Theme.success, undefined)
                else if (outcome === "failed")
                    window.notify(qsTr("Render In to Out failed: %1").arg(message), Theme.warning,
                                  { actionText: qsTr("Try Again"), action: () => MediaReadiness.renderInToOut(),
                                    timeoutMs: 10000 })
            }
        }
        // C.12 (M5n): forms extensions open (ui.prompt, non-modal), their notifications and crashes.
        ExtensionPromptHost {
            anchors.fill: parent
        }
        // p02 audio: voice-over takes to recover when a project opens (Recover / Discard).
        TakeRecoveryHost {
            anchors.fill: parent
        }
        Connections {
            target: Extensions

            function onNotified(source, level, text) {
                toast.show(qsTr("%1: %2").arg(source).arg(text),
                           level === "error" ? Theme.danger : level === "warning" ? Theme.warning : undefined)
            }
            function onFaulted(id, name, reason, disabled) {
                toast.show(disabled ? qsTr("%1 was turned off after 3 crashes. Turn it on again in Extensions.").arg(name)
                                    : qsTr("%1 crashed: %2").arg(name).arg(reason), Theme.danger)
            }
        }

        // The slice transition between workspaces (design language v2, "Signature moments").
        SliceTransition {
            id: sliceTransition
            anchors.fill: workArea
        }
    }

    // ---- Home screen (M5f): new project, recent projects, one-click import ----
    // Home asks; the unsaved-changes prompt (whenSaved) and the project file run here.
    HomeScreen {
        id: home
        anchors.fill: parent
        // Closing Home puts focus in the editor (M5j: focus is never lost).
        onOpenedChanged: {
            if (!opened) {
                Qt.callLater(window.keepFocus)
                // First run (M5j): the welcome follows the launch Home screen, never covers it.
                if (window.welcomeAfterHome) {
                    window.welcomeAfterHome = false
                    Qt.callLater(onboarding.firstRun)
                }
                performanceOffer.restart()
            }
        }
        // What Home's import left out (the other files beside an opened project, a .prproj beside timelines) shows in
        // the editor once the import or the open succeeded. The note goes with its request (taken when it comes in): an
        // open that is cancelled or fails drops it, and a later open never shows it.
        function showClosingNote(note: string) {
            if (note !== "")
                toast.show(note, Theme.warning)
        }
        onSettingsRequested: window.openSettings("", "")
        // UX rework (Home's New project page): the backend creates, saves and opens. A refusal before the project
        // exists (the name, the folder - one that cannot be made too - the settings) keeps Home open with the error; a
        // failure after it exists (it replaced the project shown; only its first save can fail then) closes Home and
        // says so in the editor. Whether it exists is what decides: the project shown changed while it ran.
        onCreateRequested: (name, folder, settings) => window.whenSaved(() => home.runBusy(qsTr("Creating project"), () => {
            let replaced = false
            const watch = () => { replaced = true }
            ProjectFile.documentChanged.connect(watch)
            const error = Home.createProject(name, folder, settings)
            ProjectFile.documentChanged.disconnect(watch)
            if (error === "") {
                home.close()
            } else if (!replaced) {
                home.showCreateError(error)
            } else {
                home.close()
                toast.show(error, Theme.warning, { timeoutMs: 9000 })
            }
        }))
        // "Start with media" (the all-in-one Import with media; the old New gaming video): the project matched to the
        // first video (or the chosen format), the files imported and edited in, a recording's streams on named tracks.
        // A step after the save that went wrong is a warning in the editor; Lottie and Photoshop files then go through
        // importUrls (their own dialogs).
        onCreateFromMediaRequested: (name, folder, settings, files, options) => window.whenSaved(() => home.runBusy(qsTr("Creating project"), () => {
            const result = Home.createFromMedia(name, folder, settings, files, options)
            if (!result.ok) {
                home.showCreateError(result.error)
                return
            }
            home.close()
            if ((result.warning ?? "") !== "")
                toast.show(result.warning, Theme.warning, { timeoutMs: 9000 })
            const deferred = result.deferred ?? []
            if (deferred.length > 0)
                Qt.callLater(() => window.importUrls(deferred))
        }))
        // Opening is synchronous: "Opening project" (the chase loader) shows for a frame or two first.
        onOpenRequested: file => {
            const note = home.takeClosingNote()
            window.whenSaved(() => {
                if (file.toString() === "") {
                    openDialog.open()
                    return
                }
                home.runBusy(qsTr("Opening project"), () => {
                    if (ProjectFile.open(file)) {
                        home.close()
                        home.showClosingNote(note)
                    } else {
                        home.showMessage(ProjectFile.lastError, "danger")
                    }
                })
            })
        }
        // Open Next to This One: the project opens beside the ones open (no unsaved prompt: nothing closes) - but over
        // the sample project it takes the sample's place, so the sample's edits are asked about first (whenSaved).
        onOpenAlongsideRequested: file => {
            const open = () => home.runBusy(qsTr("Opening project"), () => {
                if (ProjectFile.openAlongside(file))
                    home.close()
                else
                    home.showMessage(ProjectFile.lastError, "danger")
            })
            if (ProjectFile.demo)
                window.whenSaved(open)
            else
                open()
        }
        // Timelines become a new project; its first imported sequence opens and the import report shows
        // (ProjectFile.importReport). Lottie and Photoshop files that came along go through importUrls afterwards.
        onImportRequested: (urls, experimental, deferred) => {
            const note = home.takeClosingNote()
            window.whenSaved(() => home.runBusy(qsTr("Importing"), () => {
                const result = ProjectFile.importFiles(urls, { newProject: true, openImported: true,
                                                               experimentalPremiereProject: experimental })
                if ((result.timelines ?? 0) > 0 || (result.added ?? 0) > 0 || (result.captionTracks ?? 0) > 0) {
                    home.close()
                    home.showClosingNote(note)
                    if (deferred && deferred.length > 0)
                        Qt.callLater(() => window.importUrls(deferred))
                } else {
                    home.showMessage(result.failed && result.failed.length > 0
                                     ? qsTr("%1: %2").arg(result.failed[0].name).arg(result.failed[0].error)
                                     : ProjectFile.lastError, "danger")
                }
            }))
        }
        // Learn > Explore the sample project: the demo as at launch.
        onSampleRequested: window.whenSaved(() => {
            if (ProjectFile.openDemo())
                home.close()
            else
                home.showMessage(ProjectFile.lastError, "danger")
        })
        // Learn > the tour, the Keyboard Trainer, What's new: Home closes first (they run over the editor).
        onHelpRequested: what => {
            window.welcomeAfterHome = false
            home.close()
            Qt.callLater(() => window.showHelp(what))
        }
    }

    // ---- Onboarding (M5j): the welcome, the tour, the Keyboard Trainer and What's new ----
    Onboarding {
        id: onboarding
        anchors.fill: parent
        libraryTarget: docking.slotOfPanel("library")
        timelineTarget: timeline
        exportTarget: topBar.exportTab
        workspace: window.workspacePreset
        onWorkspaceRequested: ws => window.workspace = ws
        onLibraryRequested: window.showBrowserPanel("library")
        onDialogClosed: {
            Qt.callLater(window.keepFocus)
            performanceOffer.restart()
        }
    }
    // The first-run performance check (I.7): offered once, when nothing else is open (Home, the welcome, the tour, the
    // trainer, the crash notice or Recovery); Settings > Media & cache runs it again.
    FirstRunCheckDialog {
        id: firstRunCheckDialog
    }
    Timer {
        id: performanceOffer
        interval: 600
        onTriggered: {
            if (window.performanceCheckDue && PerformanceCheck.offerAtStartup && !home.opened && !onboarding.welcome.visible
                    && !onboarding.tour.active && !onboarding.trainer.visible && !crashNoticeDialog.visible
                    && !recoveryDialog.visible) {
                window.performanceCheckDue = false
                firstRunCheckDialog.offer()
            }
        }
    }
    // Help (the Help menu, the palette, Settings > Keyboard): "welcome", "tour", "trainer", "whatsNew" or "shortcuts".
    function showHelp(what) {
        if (what === "shortcuts")
            shortcutsDialog.openDialog()
        else
            onboarding.run(what)
    }
    // ---- The command palette's shell rows (UX rework, stream S): "Open panel: <name>" for the Window menu's panels
    // (no Generate while AI is off), every workspace, and the recent projects; kept in step with what they come from.
    // Running one comes back as CommandPalette.shellActionRequested ----
    readonly property var paletteShellRows: {
        const rows = []
        const commands = topBar.menuBar.panelCommands
        for (const p of Workspaces.catalog) {
            if (p.key === "generate" && !Preferences.aiEnabled)
                continue
            const command = commands[p.key] ?? ""
            rows.push({ id: "panel:" + p.key, title: qsTr("Open panel: %1").arg(p.label), group: qsTr("Window"),
                        keywords: [p.key, "panel", "window", "show"],
                        shortcut: command !== "" && KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText(command) : "" })
        }
        for (const w of Workspaces.all)
            rows.push({ id: "workspace:" + w.id, title: qsTr("Workspace: %1").arg(w.name), group: qsTr("Window"),
                        keywords: ["workspace", "layout"] })
        for (const r of Home.recent.slice(0, 10))
            rows.push({ id: "recent:" + r.url, title: qsTr("Open Recent: %1").arg(r.name), group: qsTr("File"),
                        detail: String(r.folder ?? ""), keywords: ["recent", "project", "open"],
                        enabled: r.missing !== true, reason: qsTr("The file is missing") })
        return rows
    }
    onPaletteShellRowsChanged: CommandPalette.setShellActions(window.paletteShellRows)
    Connections {
        target: CommandPalette

        function onHelpRequested(what) {
            window.showHelp(what)
        }
        function onShellActionRequested(id) {
            if (id.startsWith("panel:"))
                window.openPanelFromWindowMenu(id.slice("panel:".length))
            else if (id.startsWith("workspace:"))
                window.workspace = Number(id.slice("workspace:".length))
            else if (id.startsWith("recent:"))
                window.openLaunchedProject(id.slice("recent:".length)) // the project open now is not read again
        }
    }

    WindowFrame {
        visible: window.customFrame && !window.maximized
    }

    // Design language v3: the echo mark's launch intro, over everything, once per launch when the window first shows
    // (Main built hidden, as the tests do, never plays it). Preferences.launchIntro turns it off; reduce motion skips it.
    LaunchIntro {
        id: launchIntro
        objectName: "launchIntro"
        anchors.fill: parent
        z: 1000
        property bool launched: false
        function launchOnce() {
            if (launchIntro.launched || !window.visible)
                return
            launchIntro.launched = true
            if (Preferences.launchIntro)
                launchIntro.play()
        }
        Component.onCompleted: Qt.callLater(launchIntro.launchOnce)
        Connections {
            target: window
            function onVisibleChanged() {
                launchIntro.launchOnce()
            }
        }
    }
}

