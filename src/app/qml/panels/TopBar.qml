pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Title bar of the main window (design language v3): the echo mark (it opens the File menu) with the project's name and
// its sequence's frame size and rate, the menu bar (UX rework: File, Edit, Clip, Sequence, Markers, Graphics, View,
// Window, Help - AppMenuBar; one "Menu" button when the bar is short of room, so the workspace tabs keep theirs), the
// sequence switcher, the workspace switcher (a segmented pill group, centred), the transient status pills (0.4: one
// Background tasks pill for exports, Render In to Out, proxies, AI jobs and missed errors - its popover lists them with
// the recent messages; the microphone), command palette, round undo/redo, Save/Export pills and the window controls.
// 0.1.3: the GPU and autosave pills and the Appearance button moved into the Settings window (Settings > Interface can
// show the two pills here again); the Save button carries a dot while there are unsaved changes. With the custom
// (frameless) frame, empty areas drag the window (startSystemMove, so OS snapping/tiling works) and a double-click
// toggles maximize; with the native frame (Preferences.nativeFrame) it is a plain toolbar and the OS draws the window
// controls.
Item {
    id: root

    required property MockProject project
    // The active workspace (Main's ids); the tabs ask for another through workspaceRequested.
    property int workspace: 0
    signal workspaceRequested(int workspace)
    readonly property bool customFrame: !Preferences.nativeFrame
    readonly property bool maximized: Window.window !== null
                                      && (Window.window.visibility === Window.Maximized
                                          || Window.window.visibility === Window.FullScreen)

    signal saveRequested()
    signal exportRequested()
    signal commandPaletteRequested()
    /// A menu bar action only the window can do (AppMenuBar.actionRequested: Main.menuAction).
    signal menuActionRequested(string action, var argument)
    /// The editor's docking, for the Window menu (open panels, the maximized frame).
    property DockManager docking: null

    /// The menu bar (Main opens its menus: the logo, the palette).
    readonly property AppMenuBar menuBar: appMenuBar
    /// The room the menu bar's titles have once everything else has its own (implicit sizes: what the menu bar shows
    /// does not change it). Short of it, the titles fold into one Menu button.
    readonly property real menuRoom: {
        const others = [brandBlock, sequencePill, activityPill, workspaceTabs, trailing]
        // The row's margins, the sequence pill's own margin, and the gaps of the menu bar and the two spacers (each
        // other item adds its own below).
        let used = Theme.space(2) + Theme.gap + Theme.space(1) + 2 * barRow.spacing
        for (const item of others) {
            if (!item.visible)
                continue
            // The activity pill counts at its short width (it shows its full text only in the room left over).
            used += (item === sequencePill ? Math.min(item.implicitWidth, Theme.space(56))
                     : item === activityPill ? activityPill.compactWidth : item.implicitWidth)
                    + barRow.spacing
        }
        return root.width - used
    }
    readonly property bool menusCollapsed: root.width > 0 && appMenuBar.expandedWidth > root.menuRoom

    /// Where Export is (the onboarding tour points at it; M5j): the tab of a workspace made from the Export preset, else
    /// the Export button.
    readonly property Item exportTab: workspaceTabs.exportTab ?? exportButton

    /// The mark and the project's name: Main lays the File menu's target over it.
    readonly property Item brandArea: brandBlock

    /// Keyboard focus to the workspace tabs (M5j).
    function focusWorkspaceTabs() {
        workspaceTabs.focusCurrent()
    }
    /// The workspace `id`'s tab is renamed in place (Window > Workspaces > Rename Workspace…).
    function renameWorkspace(id: int) {
        workspaceTabs.startRename(id)
    }
    /// 0.4: Export Queue… in the Background tasks popover (Main.showExportQueue).
    signal exportQueueRequested()
    /// 0.4: opens the Background tasks popover (window.backgroundTasks; also while the pill is hidden: it opens where
    /// the pill would be, after the sequence pill). A second click on the pill closes it.
    function openActivity() {
        if (activityPopover.opened)
            activityPopover.close()
        else
            activityPopover.open()
    }

    /// 0.4: what runs in the background, in the order the activity pill names it: { icon, text, percent (0..1, -1
    /// none), warn (paused or held: the warning hue), count (how many things it stands for) }.
    readonly property var activityItems: {
        const items = []
        const j = ExportController.jobs
        const pct = v => Math.round(Math.max(0, Math.min(1, v)) * 100)
        if (j.currentState !== "") {
            const p = j.currentProgress
            const text = j.currentState === "running"
                         ? (j.currentEta !== "" ? qsTr("Exporting %1 % · %2").arg(pct(p)).arg(j.currentEta)
                                                : qsTr("Exporting %1 %").arg(pct(p)))
                         : j.currentState === "paused" ? qsTr("Export paused") : qsTr("Export queued")
            items.push({ icon: "download", text: text, percent: j.currentState === "queued" ? -1 : p,
                         warn: j.currentState === "paused", count: Math.max(1, j.activeCount) })
        }
        if (MediaReadiness.rendering)
            items.push({ icon: "film", percent: MediaReadiness.renderProgress, warn: false, count: 1,
                         text: MediaReadiness.renderProgress >= 0 ? qsTr("Rendering %1 %").arg(pct(MediaReadiness.renderProgress))
                                                                  : qsTr("Rendering…") })
        if (MediaReadiness.working)
            items.push({ icon: "layers", percent: -1, warn: false, count: 1,
                         text: MediaReadiness.statusText !== "" ? MediaReadiness.statusText : qsTr("Proxies") })
        if (Transcript.jobStatus === "running")
            items.push({ icon: "captions", percent: Transcript.jobProgress, warn: false, count: 1,
                         text: Transcript.jobProgress >= 0 ? qsTr("Transcribing %1 %").arg(pct(Transcript.jobProgress))
                                                           : qsTr("Transcribing…") })
        if (Highlights.busy)
            items.push({ icon: "sparkles", percent: Highlights.progress, warn: false, count: 1,
                         text: Highlights.progress >= 0 ? qsTr("Highlights %1 %").arg(pct(Highlights.progress))
                                                        : qsTr("Highlights…") })
        if (Shorts.busy)
            items.push({ icon: "clapperboard", percent: Shorts.progress, warn: false, count: 1,
                         text: Shorts.progress >= 0 ? qsTr("Shorts %1 %").arg(pct(Shorts.progress)) : qsTr("Shorts…") })
        if (Collect.running)
            items.push({ icon: "folders", percent: Collect.progress, warn: false, count: 1,
                         text: Collect.progress >= 0 ? qsTr("Collecting files %1 %").arg(pct(Collect.progress))
                                                     : qsTr("Collecting files…") })
        const ext = Extensions.longCommand
        if (ext.running === true)
            items.push({ icon: "puzzle", percent: ext.progress ?? -1, warn: false, count: 1,
                         text: qsTr("%1: %2").arg(String(ext.extension ?? "")).arg(String(ext.text ?? "")) })
        if (j.currentState === "" && j.heldCount > 0)
            items.push({ icon: "clock", percent: -1, warn: true, count: 1,
                         text: j.heldCount === 1 ? qsTr("1 export waiting") : qsTr("%1 exports waiting").arg(j.heldCount) })
        return items
    }

    function toggleMaximized() {
        const w = Window.window
        if (!w)
            return
        if (root.maximized)
            w.showNormal()
        else
            w.showMaximized()
    }

    implicitHeight: Theme.titleBarHeight
    /// The bar's round buttons and pills (40 px, 32 in the compact density).
    readonly property int barControl: Theme.compact ? Theme.controlHeight : Theme.controlHeightLarge


    // Actual render backend + adapter of this window (C++ bridge, src/app/src/bridge/RendererInfo).
    RendererInfo {
        id: renderer
        objectName: "rendererInfo"
        window: root.Window.window
    }

    // Window drag surface underneath the controls: buttons accept their presses, everything else moves.
    Item {
        anchors.fill: parent
        enabled: root.customFrame

        TapHandler {
            gesturePolicy: TapHandler.DragThreshold
            onDoubleTapped: root.toggleMaximized()
        }
        DragHandler {
            target: null
            onActiveChanged: {
                if (active && root.Window.window)
                    root.Window.window.startSystemMove()
            }
        }
    }

    RowLayout {
        id: barRow
        anchors.fill: parent
        anchors.leftMargin: Theme.space(2)
        anchors.rightMargin: Theme.gap
        spacing: Theme.space(2)

        // The mark (the File menu: Main lays its target over this block), the project's name and its format.
        Row {
            id: brandBlock
            Layout.alignment: Qt.AlignVCenter
            Layout.maximumWidth: Theme.space(64)
            spacing: Theme.space(3)
            clip: true

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.controlHeightLarge
                height: width
                radius: width / 2
                color: Theme.raised

                BrandLogo {
                    anchors.centerIn: parent
                    markSize: Math.round(parent.width * 0.62)
                    showWordmark: false
                    showVersion: false
                }
            }

            Column {
                id: projectText
                // The longest the name and format may get (a Column sizes from its children, so they set their widths).
                readonly property real maxWidth: Theme.space(64) - Theme.controlHeightLarge - Theme.space(3)
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1

                Text {
                    objectName: "projectTitle"
                    width: Math.min(implicitWidth, projectText.maxWidth)
                    text: ProjectFile.name !== "" ? ProjectFile.name : qsTr("Untitled project")
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontDisplay
                    font.pixelSize: Theme.compact ? Theme.textLg : Theme.textTitle
                    font.weight: Theme.weightTitle
                    font.letterSpacing: -0.01 * (Theme.compact ? Theme.textLg : Theme.textTitle)
                }
                Text {
                    objectName: "projectFormat"
                    visible: !Theme.compact && ProjectController.frameWidth > 0
                    width: Math.min(implicitWidth, projectText.maxWidth)
                    //: Title bar: the sequence's frame size and rate, e.g. "1920×1080 · 30 fps"
                    text: qsTr("%1×%2 · %3").arg(ProjectController.frameWidth).arg(ProjectController.frameHeight)
                                             .arg(ProjectController.frameRateText)
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
        }

        // UX rework: the menu bar (Premiere's menus), or one Menu button when the bar is short of room.
        AppMenuBar {
            id: appMenuBar
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: implicitWidth
            collapsed: root.menusCollapsed
            docking: root.docking
            workspace: root.workspace
            onActionRequested: (action, argument) => root.menuActionRequested(action, argument)
        }

        // Sequence switcher.
        PillButton {
            id: sequencePill
            objectName: "sequencePill"
            Layout.alignment: Qt.AlignVCenter
            Layout.maximumWidth: Theme.space(56)
            compact: true
            variant: "secondary"
            text: ProjectController.sequenceName
            iconName: "chevron-down"
            LayoutMirroring.enabled: true    // label first, chevron last
            LayoutMirroring.childrenInherit: true
            tooltip: qsTr("Switch sequence")
            Accessible.name: qsTr("Sequence: %1").arg(ProjectController.sequenceName)
            onClicked: sequenceMenu.popup(sequencePill, 0, sequencePill.height + Theme.space(1))
            Layout.leftMargin: Theme.space(1)

            ContextMenu {
                id: sequenceMenu
                objectName: "sequenceMenu"

                Instantiator {
                    model: ProjectController.sequenceNames
                    delegate: ContextMenuItem {
                        required property string modelData
                        required property int index
                        text: modelData
                        iconName: "clapperboard"
                        checkable: true
                        checked: index === ProjectController.currentSequenceIndex
                        onTriggered: ProjectController.currentSequenceIndex = index
                    }
                    onObjectAdded: (index, object) => sequenceMenu.insertItem(index, object as ContextMenuItem)
                    onObjectRemoved: (index, object) => sequenceMenu.removeItem(object as ContextMenuItem)
                }

                ContextMenuSeparator {}

                ContextMenuItem {
                    text: qsTr("New Sequence…")
                    iconName: "plus"
                    command: "file.newSequence"
                    onTriggered: ProjectController.newSequence()
                }
                ContextMenuItem {
                    objectName: "reframeMenuItem"
                    text: qsTr("Reframe…")
                    iconName: "crop"
                    onTriggered: {
                        reframeDialog.active = true;
                        (reframeDialog.item as ReframeDialog)?.open()
                    }
                }
                // AI tools (0.1.3: hidden while AI is turned off in Settings > AI).
                ContextMenuItem {
                    objectName: "roughCutMenuItem"
                    visible: Preferences.aiEnabled
                    height: visible ? implicitHeight : 0
                    text: qsTr("Rough Cut from Script…")
                    iconName: "book-open"
                    onTriggered: RoughCut.open()
                }
                ContextMenuItem {
                    objectName: "narrationMenuItem"
                    visible: Preferences.aiEnabled
                    height: visible ? implicitHeight : 0
                    text: qsTr("Narration from Script…")
                    iconName: "audio-lines"
                    onTriggered: Dub.open("narration")
                }
                ContextMenuItem {
                    objectName: "podcastWorkflowMenuItem"
                    text: qsTr("Podcast Workflow…")
                    iconName: "sparkles"
                    onTriggered: PodcastWorkflow.open()
                }

                ContextMenuSeparator {}

                // 0.1.1: Premiere's Sequence > Render In to Out (Enter); a second time while rendering stops it.
                ContextMenuItem {
                    objectName: "renderInToOutMenuItem"
                    text: MediaReadiness.rendering ? qsTr("Stop Rendering") : qsTr("Render In to Out")
                    iconName: "film"
                    command: "sequence.renderInToOut"
                    onTriggered: MediaReadiness.rendering ? MediaReadiness.cancelRender() : MediaReadiness.renderInToOut()
                }
                // 0.2: Premiere's Sequence > Selection Follows Playhead (a toggle).
                ContextMenuItem {
                    id: followPlayheadItem
                    objectName: "selectionFollowsPlayheadMenuItem"
                    text: qsTr("Selection Follows Playhead")
                    iconName: "mouse-pointer-2"
                    command: "sequence.selectionFollowsPlayhead"
                    checkable: true
                    checked: ProjectController.selectionFollowsPlayhead
                    onTriggered: {
                        ProjectController.selectionFollowsPlayhead = !ProjectController.selectionFollowsPlayhead
                        followPlayheadItem.checked = Qt.binding(() => ProjectController.selectionFollowsPlayhead)
                    }
                }
            }
        }

        // 0.4: Background tasks (where Render In to Out's pill was): the first thing that runs - "Exporting 38 % · 0:42
        // left", "Rendering 60 %", "Proxies 3/12", "Transcribing 20 %", ... - and " +N" for the others; held exports
        // alone "2 exports waiting"; with nothing running, errors nobody saw ("1 problem"). Only the percentage (or the
        // icon) while the menus are folded. A 2 px bar along its foot when it has a percentage (the warning hue while
        // paused or held). Click, Enter or Space: the popover (exports, the other jobs, the recent messages).
        PillButton {
            id: activityPill
            objectName: "activityPill"
            readonly property var first: root.activityItems.length > 0 ? root.activityItems[0] : null
            readonly property int others: {
                let n = 0
                for (const item of root.activityItems)
                    n += item.count
                return Math.max(0, n - 1)
            }
            readonly property bool problems: activityPill.first === null && MessageLog.unseen > 0
            readonly property real percent: activityPill.first !== null ? activityPill.first.percent : -1
            /// The full text, and the short one ("38 %", or nothing: the icon alone).
            readonly property string fullText: activityPill.first !== null
                                               ? activityPill.first.text + (activityPill.others > 0 ? qsTr(" +%1").arg(activityPill.others) : "")
                                               : MessageLog.unseen === 1 ? qsTr("1 problem")
                                               : qsTr("%1 problems").arg(MessageLog.unseen)
            readonly property string compactText: activityPill.percent >= 0
                                                  ? qsTr("%1 %").arg(Math.round(Math.min(1, activityPill.percent) * 100)) : ""
            // Its widths with either text, from a compact pill's padding and gap (PillButton) as constants: its own
            // padding follows the text shown, which follows the room left (a binding loop through menuRoom).
            readonly property real expandedWidth: Theme.iconSm + Theme.space(2) + Math.ceil(activityMetrics.advanceWidth)
                                                  + 2 * Theme.space(3)
            readonly property real compactWidth: activityPill.compactText === "" ? Theme.controlHeightSmall
                                                 : Theme.iconSm + Theme.space(2) + Math.ceil(compactMetrics.advanceWidth)
                                                   + 2 * Theme.space(3)
            /// The menu bar counts the pill at its short width; the full text shows only in the room the expanded
            /// menus leave (the pill shortens itself before the menus fold).
            readonly property bool showsFull: !root.menusCollapsed
                                              && root.menuRoom - appMenuBar.expandedWidth
                                                 >= activityPill.expandedWidth - activityPill.compactWidth
            Layout.alignment: Qt.AlignVCenter
            compact: true
            variant: "secondary"
            visible: activityPill.first !== null || activityPill.problems
            iconName: activityPill.problems ? "triangle-alert" : activityPill.first !== null ? activityPill.first.icon : "zap"
            text: activityPill.showsFull ? activityPill.fullText : activityPill.compactText
            tooltip: activityPill.showsFull ? qsTr("Background tasks") : qsTr("Background tasks: %1").arg(activityPill.fullText)
            command: "window.backgroundTasks"
            Accessible.name: qsTr("Background tasks: %1").arg(activityPill.fullText)
            onClicked: root.openActivity()
            Keys.onReturnPressed: root.openActivity()
            Keys.onEnterPressed: root.openActivity()

            TextMetrics {
                id: compactMetrics
                font: activityPill.font
                text: activityPill.compactText
            }
            TextMetrics {
                id: activityMetrics
                font: activityPill.font
                text: activityPill.fullText
            }
            BeamProgress {
                objectName: "activityPillProgress"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: Theme.space(3)
                anchors.rightMargin: Theme.space(3)
                anchors.bottomMargin: 1
                height: 2
                visible: activityPill.percent >= 0
                value: Math.max(0, activityPill.percent)
                hue: activityPill.first !== null && activityPill.first.warn ? Theme.warning : Theme.accentFill
                label: activityPill.fullText
            }
        }

        Item { Layout.fillWidth: true }

        // The workspaces: a segmented pill group in the middle of the bar.
        WorkspaceTabs {
            id: workspaceTabs
            Layout.alignment: Qt.AlignVCenter
            current: root.workspace

            onActivated: workspace => root.workspaceRequested(workspace)
        }

        Item { Layout.fillWidth: true }

        // The right side: job pills, the palette, undo / redo, Save, Export and the window controls.
        RowLayout {
            id: trailing
            Layout.alignment: Qt.AlignVCenter
            spacing: Theme.space(2)

            // (0.4: an extension's AI command shows in the Background tasks pill and popover, with its Stop.)
            // Voice-over (M5m, A.8): shown while the microphone is open (Test input, a pass, monitoring).
            StatusPill {
                objectName: "microphonePill"
                Layout.alignment: Qt.AlignVCenter
                visible: VoiceOver.inputOpen
                label: VoiceOver.recording ? qsTr("Recording") : qsTr("Microphone")
                dotColor: Theme.danger
                tooltip: qsTr("The microphone is open: %1").arg(VoiceOver.inputName)
            }

            // 0.1.3: shown when Settings > Interface turns them on (the same facts are in Settings).
            StatusPill {
                objectName: "gpuPill"
                Layout.alignment: Qt.AlignVCenter
                visible: Preferences.showGpuStatus
                label: qsTr("GPU")
                detail: renderer.ready ? renderer.summary : qsTr("detecting…")
                dotColor: renderer.ready ? Theme.success : Theme.textTertiary
                tooltip: renderer.ready
                         ? qsTr("Render: %1 on %2 · Decode: %3").arg(renderer.api).arg(renderer.adapter || qsTr("unknown adapter"))
                                                                 .arg(root.project.decodeBackend)
                         : qsTr("Waiting for the first frame")
            }

            StatusPill {
                objectName: "autosavePill"
                Layout.alignment: Qt.AlignVCenter
                visible: Preferences.showSaveStatus
                // 0.4: an autosave that could not write says so (red) until a save works again.
                label: ProjectFile.demo ? qsTr("Demo") : !ProjectFile.autosaveOk ? qsTr("Autosave failed")
                     : ProjectFile.dirty ? qsTr("Unsaved") : qsTr("Saved")
                detail: ProjectFile.demo ? "" : ProjectFile.autosaveText
                dotColor: !ProjectFile.demo && !ProjectFile.autosaveOk ? Theme.danger
                        : ProjectFile.dirty ? Theme.warning : Theme.success
                tooltip: ProjectFile.filePath
            }

            // Command palette, then undo / redo (EditSession history; Ctrl/⌘+Z and Shift+Ctrl/⌘+Z are handled in Main.qml):
            // round raised buttons.
            IconButton {
                objectName: "commandPaletteButton"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: root.barControl
                Layout.preferredHeight: root.barControl
                variant: "secondary"
                iconName: "search"
                tooltip: qsTr("Command palette")
                shortcut: Qt.platform.os === "osx" ? "⇧⌘P" : "Ctrl+Shift+P"
                onClicked: root.commandPaletteRequested()
            }
            IconButton {
                objectName: "undoButton"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: root.barControl
                Layout.preferredHeight: root.barControl
                variant: "secondary"
                iconName: "undo-2"
                enabled: ProjectController.canUndo
                tooltip: ProjectController.undoText
                command: "edit.undo"
                onClicked: ProjectController.undo()
            }
            IconButton {
                objectName: "redoButton"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: root.barControl
                Layout.preferredHeight: root.barControl
                variant: "secondary"
                iconName: "redo-2"
                enabled: ProjectController.canRedo
                tooltip: ProjectController.redoText
                command: "edit.redo"
                onClicked: ProjectController.redo()
            }

            Item { Layout.preferredWidth: Theme.space(1) }

            PillButton {
                id: saveButton
                objectName: "saveButton"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: root.barControl
                variant: "secondary"
                text: qsTr("Save")
                iconName: "save"
                // The save state the autosave pill used to show (0.1.3).
                // 0.4: "Unsaved changes · Autosave failed 14:05: <reason> · <path>" after a failed autosave.
                tooltip: ProjectFile.demo ? qsTr("Save project (the demo is not saved on its own)")
                       : [ProjectFile.dirty ? qsTr("Unsaved changes") : qsTr("All changes saved"), ProjectFile.autosaveText,
                          ProjectFile.filePath].filter(s => s !== "").join(" · ")
                Accessible.description: !ProjectFile.demo && !ProjectFile.autosaveOk
                                        ? qsTr("Unsaved changes. Autosave failed: save the project somewhere else.")
                                        : ProjectFile.dirty ? qsTr("Unsaved changes") : qsTr("All changes saved")
                command: "file.save"
                onClicked: root.saveRequested()

                // Unsaved changes: a dot on the button's corner (0.4: red after a failed autosave).
                Rectangle {
                    objectName: "unsavedDot"
                    visible: (ProjectFile.dirty || !ProjectFile.autosaveOk) && !ProjectFile.demo
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.rightMargin: -Theme.space(0.5)
                    anchors.topMargin: -Theme.space(0.5)
                    width: Theme.dotSize + Theme.space(0.5)
                    height: width
                    radius: width / 2
                    color: ProjectFile.autosaveOk ? Theme.warning : Theme.danger
                    border.width: Theme.hairline + 1
                    border.color: Theme.canvas
                }
            }

            // 0.2: Premiere's Quick Export (a preset, a file, one click).
            QuickExportButton {
                Layout.alignment: Qt.AlignVCenter
            }

            PillButton {
                id: exportButton
                objectName: "topBarExportButton"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: root.barControl
                variant: "primary"
                text: qsTr("Export")
                iconName: "download"
                tooltip: qsTr("Export media")
                command: "file.exportMedia"
                onClicked: root.exportRequested()
            }

            Rectangle {
                visible: root.customFrame
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: Theme.space(1)
                Layout.rightMargin: Theme.space(1)
                implicitWidth: Theme.hairline
                implicitHeight: Theme.controlHeightSmall - Theme.space(2)
                color: Theme.borderStrong
            }

            WindowControls {
                visible: root.customFrame
                Layout.alignment: Qt.AlignVCenter
                maximized: root.maximized
                onMinimizeRequested: root.Window.window?.showMinimized()
                onMaximizeRequested: root.toggleMaximized()
                onCloseRequested: root.Window.window?.close()
            }
        }
    }

    // 0.4: where the Background tasks popover hangs: the pill, or where it would be (just after the sequence pill) while
    // it is hidden. It is the popover's parent, so a press anywhere else in the title bar (a menu, a workspace tab,
    // Save, Export) closes the popover (Popover: CloseOnPressOutsideParent), and a press on the pill leaves it to the
    // pill's click (openActivity() toggles it).
    Item {
        id: activityAnchor
        objectName: "activityAnchor"
        x: barRow.x + (activityPill.visible ? activityPill.x : sequencePill.x + sequencePill.width + barRow.spacing)
        y: barRow.y + (activityPill.visible ? activityPill.y : sequencePill.y)
        width: activityPill.visible ? activityPill.width : 0
        height: activityPill.visible ? activityPill.height : sequencePill.height
    }
    // 0.4: Background tasks (the activity pill, window.backgroundTasks): placed by openActivity() under the pill, or
    // where it would be.
    ActivityPopover {
        id: activityPopover
        parent: activityAnchor
        // Under the pill (the bar's row is laid out in the bar), else just after the sequence pill; inside the bar.
        x: Math.max(Theme.space(2), Math.min(activityAnchor.x, root.width - width - Theme.space(2))) - activityAnchor.x
        y: root.height - Theme.space(1) - activityAnchor.y
        transformOrigin: Item.TopLeft
        onExportQueueRequested: root.exportQueueRequested()
    }

    // Sequence > Reframe… (AI-9): loaded on first use.
    Loader {
        id: reframeDialog
        objectName: "reframeDialogLoader"
        active: false
        sourceComponent: ReframeDialog {}
    }
}
