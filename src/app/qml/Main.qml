import QtQuick
import QtQuick.Dialogs
import QtQuick.Window

// Wabcut main window (design language v2): the title bar (TopBar) with the workspace tabs, the left rail (panel
// switcher) and a docked workbench of panels with 6px gutters you can drag (docs/design-language.md, "Geometry").
//
// Frame: frameless with custom chrome by default; Preferences.nativeFrame switches to the platform's decorations.
// Density: below Theme.compactBelowHeight the shell turns compact (tighter panel chrome and track heights) and gives
// the reclaimed height to the timeline.
// Workspaces (0.1.3, panels/workspace/Workspaces.qml): every workspace is the user's, made from a preset (Edit, Color,
// Audio, Captions, Export) or as a copy of another, and each keeps its own panel layout: a tree of panel groups
// (DockManager) the user rearranges by dragging a group's grip or a panel's tab (beside another group, into its tabs,
// along an edge, or into a window of its own) and changes by closing panels (their tabs' ×, the close button in their
// header) and opening them again (the rail, File > Window, the panel focus keys). The panels are declared once
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
    /// The rail's panel: the browser panel shown in the group of the browser panels, else "".
    readonly property string railPanel: {
        const g = docking.groupOfColumn("left")
        const key = g ? docking.shownPanel(g.group) : ""
        return Workspaces.isBrowserPanel(key) ? key : ""
    }
    /// The browser panel shown (the rail's).
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
        revealPanel(key)
        Qt.callLater(() => {
            if (key === "library") {
                libraryPanel.focusContent()
                return
            }
            // The other tabs are LazyPanels (M5n): focused once created.
            const loaders = { effects: effectsLoader, captions: captionsLoader, audio: audioToolsLoader,
                              markers: markersLoader, shorts: shortsLoader, generate: generateLoader, takes: takesLoader,
                              essential: essentialLoader, review: reviewLoader, graphics: graphicsLoader,
                              highlights: highlightsLoader, mediaBrowser: mediaBrowserLoader,
                              metadata: metadataLoader, production: productionLoader }
            loaders[key]?.withItem(panel => panel.focusContent())
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
        // welcome follows it (M5j).
        if (Home.showAtLaunch) {
            home.open()
            window.welcomeAfterHome = true
            window.performanceCheckDue = PerformanceCheck.offerAtStartup
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
        if (launched.toString() !== "") {
            window.whenSaved(() => {
                if (ProjectFile.open(launched))
                    home.close()
                else
                    home.showImportError(ProjectFile.lastError)
            })
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
        case "effects.deleteCustomItem":
            EffectPresets.deleteCurrentItem()
            return
        case "window.timeline":
            window.revealPanel("timeline")
            Qt.callLater(() => timeline.focusLanes())
            return
        case "window.sourceMonitor":
            monitorArea.show("source")
            return
        case "window.programMonitor":
            monitorArea.show("program")
            return
        // p02 audio: Shift+6 / Shift+9, the Audio Track Mixer and the Audio Clip Mixer (the mixer panel's two views).
        case "window.audioTrackMixer":
        case "window.audioClipMixer":
            window.revealPanel("mixer")
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
            audioGainDialog.active = true
            audioGainDialog.item.openFor(ClipAudio.selectedClips())
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
        case "file.import":
            importDialog.open()
            return
        case "file.exit":
            window.close()
            return
        case "edit.keyboardShortcuts":
            shortcutsDialog.openDialog()
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

    // Runs `action` now, or after the user saved or discarded the unsaved changes (Cancel drops it).
    function whenSaved(action) {
        if (!ProjectFile.dirty) {
            action()
            return
        }
        pendingAction = action
        unsavedDialog.open()
    }
    function continuePending() {
        const action = pendingAction
        pendingAction = null
        if (action)
            action()
    }
    // Saves (asking for a file while untitled), then runs `then`.
    function saveProject(then) {
        if (ProjectFile.untitled || ProjectFile.demo) {
            saveAsDialog.then = then
            saveAsDialog.open()
            return
        }
        if (ProjectFile.save() && then)
            then()
    }
    function newProject() {
        whenSaved(() => ProjectFile.newProject())
    }
    function openProject() {
        whenSaved(() => openDialog.open())
    }
    // 0.2 (P.3): a project chosen in the Production panel (or File > Open Project Next to This One) opens next to the
    // ones open and comes into the panels; the others stay loaded with their undo history and unsaved changes.
    function openProjectFile(url) {
        ProjectFile.openAlongside(url) // a failure shows as ProjectFile.lastError, like File > Open
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
    function showHome() {
        home.open()
    }
    // File > Import and drops on the Library. A .wabcut project among the files is opened instead.
    function importUrls(urls) {
        // C.13 (M5o): Lottie animations become graphic clips, with their own report. 0.2: Photoshop files open Import
        // Layered File (in turn when several come together).
        const rest = []
        const layered = []
        for (const url of urls) {
            if (GraphicsImport.isLottie(url))
                GraphicsImport.importLottie(url)
            else if (Library.isLayeredFile(url))
                layered.push(url)
            else
                rest.push(url)
        }
        if (layered.length > 0)
            importLayeredFileDialog.openFor(layered)
        if (rest.length === 0)
            return ({})
        const result = ProjectFile.importFiles(rest)
        if (result.projects && result.projects.length > 0)
            whenSaved(() => ProjectFile.open(result.projects[0]))
        return result
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

    // File menu (Alt+F, or a click on the logo).
    ContextMenu {
        id: fileMenu
        objectName: "fileMenu"

        ContextMenuItem {
            objectName: "fileMenuHome"
            text: qsTr("Home")
            iconName: "house"
            onTriggered: window.showHome()
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            text: qsTr("New Project")
            command: "file.newProject"
            onTriggered: window.newProject()
        }
        ContextMenuItem {
            text: qsTr("Open Project…")
            command: "file.openProject"
            onTriggered: window.openProject()
        }
        // 0.2 (P.3): several projects open at once (the Production panel lists them and switches between them).
        ContextMenuItem {
            objectName: "fileMenuOpenAlongside"
            text: qsTr("Open Project Next to This One…")
            onTriggered: openAlongsideDialog.open()
        }
        ContextMenuItem {
            objectName: "fileMenuCloseProject"
            text: qsTr("Close Project")
            enabled: ProjectFile.openProjects.length > 1
            onTriggered: window.closeOpenProject(ProjectFile.activeProject)
        }
        ContextMenuItem {
            text: qsTr("Save")
            iconName: "save"
            command: "file.save"
            onTriggered: window.saveProject(null)
        }
        ContextMenuItem {
            text: qsTr("Save As…")
            command: "file.saveAs"
            onTriggered: {
                saveAsDialog.then = null
                saveAsDialog.open()
            }
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            text: qsTr("Import…")
            iconName: "plus"
            command: "file.import"
            onTriggered: importDialog.open()
        }
        ContextMenuItem {
            objectName: "importLottieMenuItem"
            text: qsTr("Import Lottie…")
            iconName: "plus"
            onTriggered: lottieDialog.open()
        }
        ContextMenuItem {
            text: qsTr("Relink Offline Media…")
            enabled: ProjectFile.offlineCount > 0
            onTriggered: relinkDialog.open()
        }
        // I.4: every saved/autosaved/backup version (the recovery dashboard), and the portable project capsule.
        ContextMenuItem {
            objectName: "recoverMenuItem"
            text: qsTr("Recover…")
            onTriggered: recoveryDialog.openDashboard()
        }
        // 0.1.1: Premiere's File > Export > Final Cut Pro XML / EDL / OTIO (interchange.export, on a worker).
        ContextMenuItem {
            objectName: "exportTimelineMenuItem"
            text: qsTr("Export Timeline…")
            iconName: "file-down"
            enabled: ProjectController.sequenceNames.length > 0
            onTriggered: exportTimelineDialog.openForCurrent()
        }
        ContextMenuItem {
            objectName: "collectFilesMenuItem"
            text: qsTr("Collect Files…")
            enabled: !ProjectFile.demo
            onTriggered: {
                collectFilesDialog.active = true
                collectFilesDialog.item.open()
            }
        }
        ContextMenuSeparator {}
        // 0.1.3: the Settings window (the rail hosts it).
        ContextMenuItem {
            objectName: "fileMenuSettings"
            text: qsTr("Settings…")
            iconName: "settings"
            shortcut: Qt.platform.os === "osx" ? "⌘," : "Ctrl+,"
            onTriggered: rail.openSettings("")
        }
        ContextMenuItem {
            objectName: "fileMenuShortcuts"
            text: qsTr("Keyboard Shortcuts…")
            iconName: "keyboard"
            command: "edit.keyboardShortcuts"
            onTriggered: shortcutsDialog.openDialog()
        }
        // P.2 (E5): Premiere's File > New > Sequence From Clip, Sequence > Sequence Settings and Window > History.
        ContextMenuItem {
            objectName: "newSequenceFromClipMenuItem"
            text: qsTr("New Sequence From Clip")
            iconName: "clapperboard"
            command: "file.newSequenceFromClip"
            enabled: Library.mediaSelected
            onTriggered: Editing.runCommand("file.newSequenceFromClip")
        }
        ContextMenuItem {
            objectName: "sequenceSettingsMenuItem"
            text: qsTr("Sequence Settings…")
            iconName: "sliders-horizontal"
            command: "sequence.settings"
            enabled: ProjectController.sequenceNames.length > 0
            onTriggered: Editing.runCommand("sequence.settings")
        }
        ContextMenuItem {
            objectName: "historyMenuItem"
            text: qsTr("History")
            iconName: "undo-2"
            command: "window.history"
            onTriggered: Editing.runCommand("window.history")
        }
        ContextMenuItem {
            objectName: "editingPreferencesMenuItem"
            text: qsTr("Media and Timeline Preferences…")
            iconName: "settings"
            command: "edit.preferencesMedia"
            onTriggered: Editing.runCommand("edit.preferencesMedia")
        }
        // 0.1.3 panel docking: Window: every panel, checked when it is open in this workspace; choosing one opens it (or
        // shows it where it is).
        ContextMenu {
            id: windowMenu
            objectName: "windowMenu"
            title: qsTr("Window")

            Instantiator {
                model: Workspaces.catalog
                delegate: ContextMenuItem {
                    required property var modelData
                    objectName: "windowMenu_" + modelData.key
                    text: modelData.label
                    iconName: modelData.icon
                    checkable: true
                    checked: docking.isOpen(modelData.key)
                    onTriggered: {
                        window.revealPanel(modelData.key)
                        checked = Qt.binding(() => docking.isOpen(modelData.key))
                    }
                }
                onObjectAdded: (index, object) => windowMenu.insertItem(index, object as ContextMenuItem)
                onObjectRemoved: (index, object) => windowMenu.removeItem(object as ContextMenuItem)
            }
        }
        // The current workspace's panels back where its preset puts them.
        ContextMenuItem {
            objectName: "resetPanelLayoutMenuItem"
            text: qsTr("Reset Panel Layout")
            iconName: "layout-grid"
            onTriggered: docking.reset()
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            text: qsTr("Exit")
            command: "file.exit"
            onTriggered: window.close()
        }
    }
    Shortcut {
        sequence: "Alt+F"
        onActivated: fileMenu.popup(topBar, Theme.gap, topBar.height)
    }
    // 0.1.3: Settings (Cmd+, on macOS).
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: rail.openSettings("")
    }

    FileDialog {
        id: openDialog
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Wabcut projects (*.wabcut)"), qsTr("All files (*)")]
        currentFolder: ProjectFile.folder
        onAccepted: {
            if (ProjectFile.open(selectedFile))
                home.close()
        }
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
            if (ProjectFile.saveAs(selectedFile) && next)
                next()
        }
        onRejected: then = null
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
            attributesDialog.active = true
            attributesDialog.item.openFor(mode)
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
            layoutPicker.active = true
            layoutPicker.item.openFor(window.contentItem, monitor)
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
            audioChannelsDialog.active = true
            audioChannelsDialog.item.openFor(mode, keys)
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
            extensionsManager.active = true
            extensionsManager.item.open()
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
            audioPluginsSettings.active = true
            audioPluginsSettings.item.open()
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
        audioSettings.active = true
        audioSettings.item.open()
    }
    VoiceOverSync {}
    Connections {
        target: Multicam

        function onCreateRequested(media) {
            multicamCreateDialog.active = true
            multicamCreateDialog.item.openFor(media)
        }
    }
    Connections {
        target: ClipAudio

        function onEnhanceRequested(clips) {
            enhanceSpeechDialog.active = true
            enhanceSpeechDialog.item.openFor(clips)
        }
        function onStemsRequested(clip) {
            stemsDialog.active = true
            stemsDialog.item.openFor(clip)
        }
        function onSyncReportChanged() {
            if (ClipAudio.syncReport.ok === undefined)
                return
            syncReportDialog.active = true
            syncReportDialog.item.open()
        }
    }
    Connections {
        target: Nesting

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
            roughCutSheet.active = true
            roughCutSheet.item.open()
        }
    }
    Connections {
        target: AutoZoom

        function onOpenRequested(clip) {
            autoZoomDialog.active = true
            autoZoomDialog.item.openFor(clip)
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
            translateDialog.active = true
            translateDialog.item.open()
        }
    }
    Connections {
        target: Dub

        function onOpenRequested(mode) {
            dubDialog.active = true
            dubDialog.item.openFor(mode)
        }
        function onProposalRequested() {
            dubDialog.active = true
            dubDialog.item.openFor(Dub.proposal.kind ?? "dub")
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
            autoSwitchDialog.active = true
            autoSwitchDialog.item.openFor(clip)
        }
    }
    Connections {
        target: PodcastWorkflow

        function onOpenRequested() {
            podcastWorkflowSheet.active = true
            podcastWorkflowSheet.item.open()
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
            mediaCacheSettings.active = true
            mediaCacheSettings.item.open()
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
        }

        // The mark and the project's name open the File menu (the shell has no menu bar; the logo is the conventional
        // app menu).
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
                onTapped: fileMenu.popup(topBar, Theme.gap, topBar.height)
            }
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
        }

        LeftRail {
            id: rail
            anchors.left: parent.left
            anchors.top: topBar.bottom
            anchors.bottom: parent.bottom
            anchors.leftMargin: Theme.gap
            anchors.bottomMargin: Theme.gap
            width: Theme.railWidth
            panel: window.railPanel   // nothing highlighted while the browser is hidden (Export) or shows the scopes (Color)
            onPanelRequested: key => window.showBrowserPanel(key)
            onHelpRequested: what => window.showHelp(what)
            onAudioSettingsRequested: window.openAudioSettings()
        }

        Item {
            id: workArea
            anchors.left: rail.right
            anchors.right: parent.right
            anchors.top: topBar.bottom
            anchors.bottom: parent.bottom
            anchors.leftMargin: Theme.gap
            anchors.rightMargin: Theme.gap
            anchors.bottomMargin: Theme.gap

            // 0.1.3 panel docking: the current workspace's layout (DockManager) in the work area: each group's rectangle and
            // the gutters between them.
            readonly property real leastFlexible: Theme.space(30)
            readonly property var dockLayout: docking.layout(docking.tree, width, height, docking.presence(), Theme.gap,
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
                // Transcript and captions (M5e); the rail key stays "captions".
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
                    sourceComponent: TranscriptPanel {
                        onSetupRequested: rail.openAiPreferences()
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
                    sourceComponent: ProductionPanel {
                        onOpenProjectRequested: url => window.openProjectFile(url)
                        onCloseProjectRequested: index => window.closeOpenProject(index)
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
                        onCloseRequested: docking.closePanel("frameInspector")
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
                    onMonitorRequested: which => monitorArea.current = which
                    onEditRequested: insert => window.sourceEdit(insert)
                    onEditHovered: (insert, hovered) => {
                        if (hovered)
                            SourceMonitor.previewEdit(insert, Preferences.fitClipChoice)
                        else
                            SourceMonitor.clearPreview()
                    }
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
                    onMonitorRequested: which => monitorArea.current = which
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
                            // C.13 (M5o): Lottie animations become graphic clips, with their own report.
                            const files = []
                            for (const url of drop.urls) {
                                if (GraphicsImport.isLottie(url))
                                    GraphicsImport.importLottie(url)
                                else
                                    files.push(url)
                            }
                            if (files.length === 0)
                                return
                            const result = ProjectFile.dropFiles(files, at ? at.time : ProjectController.playheadText,
                                                                 at ? at.track : "", !ProjectFile.insertModifierHeld())
                            if (result.projects && result.projects.length > 0)
                                window.whenSaved(() => ProjectFile.open(result.projects[0]))
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
                    visible: place !== null && width > 0 && height > 0
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
                function placeOf(docked: bool, key: string): var {
                    const slot = docked ? docking.slotOfPanel(key) : null
                    return slot ? workArea.rectOf(slot.dockKey) : null
                }
                readonly property var bounds: workArea.union([placeOf(sourceDocked, "source"), placeOf(programDocked, "program")])
                x: bounds.x
                y: bounds.y
                width: bounds.width
                height: bounds.height

                // Shows (and focuses) a monitor: its tab, opened again when it is closed.
                function show(which) {
                    window.revealPanel(which)
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
        onSettingsRequested: rail.openSettings("")
        onCreateRequested: (name, folder, sequence) => window.whenSaved(() => {
            const error = Home.createProject(name, folder, sequence)
            if (error === "")
                home.close()
            else
                home.showCreateError(error)
        })
        // Gaming (M5o): New gaming video - a project at the recording's size and rate, its audio as named tracks.
        onGamingRequested: (name, folder, tracks) => window.whenSaved(() => {
            const error = GamingStart.create(name, folder, tracks)
            if (error === "")
                home.close()
            else
                home.showGamingError(error)
        })
        onOpenRequested: file => window.whenSaved(() => {
            if (file.toString() === "") {
                openDialog.open()
                return
            }
            // Opening is synchronous: show "Opening project" (the chase loader) for a frame or two first, then open.
            home.busyText = qsTr("Opening project")
            homeOpenLater.file = file
            homeOpenLater.restart()
        })
        Timer {
            id: homeOpenLater
            property url file
            interval: 48
            onTriggered: {
                const opened = ProjectFile.open(homeOpenLater.file)
                home.busyText = ""
                if (opened)
                    home.close()
                else
                    home.showImportError(ProjectFile.lastError)
            }
        }

        // A timeline (or media) becomes a new project; its first imported sequence opens and the import report
        // shows (ProjectFile.importReport).
        onImportRequested: (urls, experimental) => window.whenSaved(() => {
            const result = ProjectFile.importFiles(urls, { newProject: true, openImported: true,
                                                           experimentalPremiereProject: experimental })
            if ((result.timelines ?? 0) > 0 || (result.added ?? 0) > 0)
                home.close()
            else
                home.showImportError(result.failed && result.failed.length > 0
                                     ? qsTr("%1: %2").arg(result.failed[0].name).arg(result.failed[0].error)
                                     : ProjectFile.lastError)
        })
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
    // Help (the left rail, the palette).
    function showHelp(what) {
        if (what === "shortcuts")
            shortcutsDialog.openDialog()
        else
            onboarding.run(what)
    }
    Connections {
        target: CommandPalette

        function onHelpRequested(what) {
            window.showHelp(what)
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

