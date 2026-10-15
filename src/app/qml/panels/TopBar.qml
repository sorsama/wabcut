pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Title bar of the main window (design language v3): the echo mark (it opens the File menu) with the project's name and
// its sequence's frame size and rate, the sequence switcher, the workspace switcher (a segmented pill group, centred),
// the transient status pills (rendering, an extension's job, the microphone), command palette, round undo/redo,
// Save/Export pills and the window controls. 0.1.3: the GPU and autosave pills and the Appearance button moved
// into the Settings window (Settings > Interface can show the two pills here again); the Save button carries a dot
// while there are unsaved changes. With the custom (frameless) frame, empty areas drag the window
// (startSystemMove, so OS snapping/tiling works) and a double-click toggles maximize; with the native frame
// (Preferences.nativeFrame) it is a plain toolbar and the OS draws the window controls.
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

    /// Where Export is (the onboarding tour points at it; M5j): the tab of a workspace made from the Export preset, else
    /// the Export button.
    readonly property Item exportTab: workspaceTabs.exportTab ?? exportButton

    /// The mark and the project's name: Main lays the File menu's target over it.
    readonly property Item brandArea: brandBlock

    /// Keyboard focus to the workspace tabs (M5j).
    function focusWorkspaceTabs() {
        workspaceTabs.focusCurrent()
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
            }
        }

        // 0.1.1: Render In to Out's progress; click to stop it (or to dismiss why it failed).
        PillButton {
            id: renderPill
            objectName: "renderProgressPill"
            property string dismissed: ""
            Layout.alignment: Qt.AlignVCenter
            compact: true
            variant: "secondary"
            visible: MediaReadiness.rendering
                     || (MediaReadiness.renderMessage !== "" && MediaReadiness.renderMessage !== renderPill.dismissed)
            text: !MediaReadiness.rendering ? MediaReadiness.renderMessage
                  : MediaReadiness.renderProgress >= 0 ? qsTr("Rendering %1 %").arg(Math.round(MediaReadiness.renderProgress * 100))
                  : qsTr("Rendering…")
            iconName: "x"
            tooltip: MediaReadiness.rendering ? qsTr("Stop rendering (the segments rendered so far stay)") : qsTr("Dismiss")
            onClicked: {
                if (MediaReadiness.rendering)
                    MediaReadiness.cancelRender()
                else
                    renderPill.dismissed = MediaReadiness.renderMessage
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

        // An extension's AI-model command running as a command job (C.14), with its progress and Cancel.
        StatusPill {
            id: extensionJobPill
            objectName: "extensionJobPill"
            readonly property var job: Extensions.longCommand
            Layout.alignment: Qt.AlignVCenter
            visible: job.running === true
            label: job.extension ?? ""
            detail: (job.progress ?? -1) >= 0 ? qsTr("%1 %2 %").arg(job.text ?? "").arg(Math.round(job.progress * 100))
                                              : (job.text ?? "")
            dotColor: Theme.accentStroke
            tooltip: (job.queued ?? 0) > 0 ? qsTr("%1 is running an AI command; %n more waiting", "", job.queued).arg(label)
                                           : qsTr("%1 is running an AI command").arg(label)
        }
        IconButton {
            objectName: "extensionJobCancel"
            Layout.alignment: Qt.AlignVCenter
            visible: extensionJobPill.visible
            compact: true
            iconName: "x"
            tooltip: qsTr("Stop %1's command (nothing is changed)").arg(extensionJobPill.label)
            onClicked: Extensions.cancelLongCommand()
        }

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
            label: ProjectFile.demo ? qsTr("Demo") : ProjectFile.dirty ? qsTr("Unsaved") : qsTr("Saved")
            detail: ProjectFile.demo ? "" : ProjectFile.autosaveText
            dotColor: ProjectFile.dirty ? Theme.warning : Theme.success
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
            tooltip: ProjectFile.demo ? qsTr("Save project (the demo is not saved on its own)")
                   : [ProjectFile.dirty ? qsTr("Unsaved changes") : qsTr("All changes saved"), ProjectFile.autosaveText,
                      ProjectFile.filePath].filter(s => s !== "").join(" · ")
            Accessible.description: ProjectFile.dirty ? qsTr("Unsaved changes") : qsTr("All changes saved")
            command: "file.save"
            onClicked: root.saveRequested()

            // Unsaved changes: a dot on the button's corner.
            Rectangle {
                objectName: "unsavedDot"
                visible: ProjectFile.dirty && !ProjectFile.demo
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.rightMargin: -Theme.space(0.5)
                anchors.topMargin: -Theme.space(0.5)
                width: Theme.dotSize + Theme.space(0.5)
                height: width
                radius: width / 2
                color: Theme.warning
                border.width: Theme.hairline + 1
                border.color: Theme.canvas
            }
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

    // Sequence > Reframe… (AI-9): loaded on first use.
    Loader {
        id: reframeDialog
        objectName: "reframeDialogLoader"
        active: false
        sourceComponent: ReframeDialog {}
    }
}
