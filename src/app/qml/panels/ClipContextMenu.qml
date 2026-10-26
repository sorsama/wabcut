pragma ComponentBehavior: Bound
import QtQuick

// Right-click menu of a timeline clip (the clip is already selected): clipboard (Cut, Copy, Paste, Paste Insert, Paste
// Attributes…, Remove Attributes…: C.11), ripple delete, enable, link,
// Speed/Duration (the dialog), rename and the label colour swatches. Every action goes through ProjectController.
// M5l: Nest… (the name dialog), the nested clip's sequence (open, reveal in the Library, unnest), Flatten for a
// multi-camera clip, and the audio tools: Enhance Speech…, Render Enhancement, Separate Stems…, Synchronize.
// M5m (A.8): Takes, the submenu of a recorded voice-over take (only on such clips).
// M5n (A.9): Audio Channels, the submenu of clips with sound (Audio Channels…, Fill, Swap, Split to Mono), after Takes.
// M5o (gaming): Moments, the submenu of one-click speed and zoom presets, after Speed/Duration….
// P.2 (E5): Video Options, Replace With Clip, Join Through Edits and Make Subsequence (the Editing bridge).
// UX rework (stream T): Reveal in Project (the clip's item selected in the Library, which comes forward; keymap
// clip.revealInProject, no default key) and Reveal in File Explorer / Finder (its media file), after Rename...
// 0.4 (stream A): Clear (Delete: the selected clips go and leave gaps) before Ripple Delete, and Audio Gain… (G) at the
// top of the audio tools on clips with sound; its dialog opens once the menu has closed (pendingCommand).
// 0.5 (stream C): Scene Edit Detection… after Speed/Duration… (SceneDetect; greyed out with the reason without video).
ContextMenu {
    id: clipMenu
    objectName: "clipContextMenu"

    property string clipId: ""
    property point menuPoint
    /// A keymap command to run once the menu has closed (a dialog it opens keeps the focus).
    property string pendingCommand: ""
    onClosed: {
        const command = clipMenu.pendingCommand
        clipMenu.pendingCommand = ""
        if (command !== "")
            KeymapDispatcher.trigger(command)
    }

    signal renameRequested(string clipId, point at)
    /// Nest…: the panel asks for the new sequence's name.
    signal nestRequested()

    // What the clip is (read when the menu opens).
    property var nestInfo: ({})
    property var audioInfo: ({})
    /// Editing.revealInfo: { item, name, inProject, inExplorer, offline }.
    property var revealInfo: ({})
    property var selection: []
    /// 0.5: SceneDetect.availability(): { enabled, reason, note } for Scene Edit Detection….
    property var sceneInfo: ({})

    function openAt(target, id, point) {
        clipId = id
        menuPoint = point
        nestInfo = Nesting.clipInfo(id)
        audioInfo = ClipAudio.clipInfo(id)
        revealInfo = Editing.revealInfo(id)
        selection = ClipAudio.selectedClips()
        sceneInfo = SceneDetect.availability()
        showChannels()
        showTakes()
        momentsMenu.refresh()
        videoOptionsMenu.refresh()
        interpolationMenu.refresh()
        popup(target, point.x, point.y)
    }
    /// The Moments submenu (M5o).
    readonly property MomentsSubmenu momentsSubmenu: momentsMenu

    // The submenus after the audio tools: Takes (recorded takes) right after Synchronize, then Audio Channels.
    readonly property AudioChannelsSubmenu channelsMenu: AudioChannelsSubmenu {}
    function submenuIndex(submenu) {
        for (let i = 0; i < clipMenu.count; ++i) {
            const item = clipMenu.itemAt(i) as ContextMenuItem
            if (item && item.subMenu === submenu)
                return i
        }
        return -1
    }
    /// The row after Synchronize.
    function afterSynchronize() {
        for (let i = 0; i < clipMenu.count; ++i) {
            if (clipMenu.itemAt(i)?.objectName === "synchronizeMenuItem")
                return i + 1
        }
        return clipMenu.count
    }
    function showChannels() {
        channelsMenu.clipKeys = clipMenu.selection.indexOf(clipMenu.clipId) >= 0 ? clipMenu.selection : [clipMenu.clipId]
        channelsMenu.error = ""
        const at = submenuIndex(channelsMenu)
        if (clipMenu.audioInfo.audio !== true) {
            if (at >= 0)
                clipMenu.takeMenu(at) // takeMenu keeps the submenu (removeMenu would destroy it)
            return
        }
        if (at < 0) {
            const takes = submenuIndex(takesMenu)
            clipMenu.insertMenu(takes >= 0 ? takes + 1 : afterSynchronize(), channelsMenu)
        }
    }

    // Takes (A.8): a recorded take's clip gets the submenu after the audio tools; other clips do not show it.
    readonly property TakesSubmenu takesMenu: TakesSubmenu {}
    function takesIndex() {
        return submenuIndex(clipMenu.takesMenu)
    }
    function showTakes() {
        takesMenu.clipKey = clipMenu.clipId
        takesMenu.refresh()
        const at = takesIndex()
        if (takesMenu.takes.length === 0) {
            if (at >= 0)
                clipMenu.takeMenu(at) // takeMenu keeps the submenu (removeMenu would destroy it)
            return
        }
        if (at >= 0)
            return
        clipMenu.insertMenu(afterSynchronize(), takesMenu)
    }

    ContextMenuItem {
        text: qsTr("Cut")
        iconName: "scissors"
        command: "edit.cut"
        onTriggered: ProjectController.cutSelection()
    }
    ContextMenuItem {
        text: qsTr("Copy")
        iconName: "copy"
        command: "edit.copy"
        onTriggered: ProjectController.copySelection()
    }
    // 0.4: the same Paste as Ctrl+V (edit.paste): effects copied in Effect Controls, when they are the newest copy, go
    // onto the selected clips; otherwise the copied clips at the playhead.
    ContextMenuItem {
        objectName: "pasteMenuItem"
        text: qsTr("Paste")
        iconName: "clipboard-paste"
        command: "edit.paste"
        enabled: ProjectController.canPaste || ProjectController.effectControls.canPasteEffects
        onTriggered: ProjectController.runCommand("edit.paste")
    }
    // C.11 (M5m): Paste Insert, and Premiere's Paste Attributes… / Remove Attributes… (the attributes dialog).
    ContextMenuItem {
        objectName: "pasteInsertMenuItem"
        text: qsTr("Paste Insert")
        iconName: "insert-edit"
        command: "edit.pasteInsert"
        enabled: ProjectController.canPaste
        onTriggered: ProjectController.pasteInsert()
    }
    ContextMenuItem {
        objectName: "pasteAttributesMenuItem"
        text: qsTr("Paste Attributes…")
        iconName: "sliders-horizontal"
        command: "edit.pasteAttributes"
        enabled: Clipboard.canPasteAttributes
        onTriggered: Clipboard.openAttributes("paste")
    }
    ContextMenuItem {
        objectName: "removeAttributesMenuItem"
        text: qsTr("Remove Attributes…")
        iconName: "rotate-ccw"
        command: "edit.removeAttributes"
        enabled: Clipboard.canRemoveAttributes
        onTriggered: Clipboard.openAttributes("remove")
    }
    ContextMenuItem {
        text: qsTr("Generative Extend…")
        iconName: "sparkles"
        onTriggered: GenerateController.openExtend(clipMenu.clipId)
    }
    ContextMenuItem {
        objectName: "autoZoomMenuItem"
        text: qsTr("Auto Zoom…")
        iconName: "zoom-in"
        onTriggered: AutoZoom.open(clipMenu.clipId)
    }
    ContextMenuItem {
        objectName: "autoSwitchMenuItem"
        text: qsTr("Auto Switch…")
        iconName: "camera"
        enabled: AutoSwitch.isMulticam(clipMenu.clipId)
        onTriggered: AutoSwitch.open(clipMenu.clipId)
    }
    ContextMenuSeparator {}
    // 0.4: Premiere's Clear: the selected clips go and leave gaps (one undo step "Delete").
    ContextMenuItem {
        objectName: "clearMenuItem"
        text: qsTr("Clear")
        iconName: "x"
        command: "edit.clear"
        enabled: ProjectController.selectedClips.length > 0
        onTriggered: ProjectController.deleteSelection()
    }
    ContextMenuItem {
        text: qsTr("Ripple Delete")
        iconName: "trash-2"
        command: "edit.rippleDelete"
        onTriggered: ProjectController.rippleDeleteSelection()
    }
    ContextMenuItem {
        id: enableItem
        text: qsTr("Enable")
        iconName: "power"
        command: "clip.enable"
        checkable: true
        checked: ProjectController.selectionEnabled
        onTriggered: {
            ProjectController.toggleEnabledSelection()
            enableItem.checked = Qt.binding(() => ProjectController.selectionEnabled)
        }
    }
    ContextMenuItem {
        text: ProjectController.selectionLinked ? qsTr("Unlink") : qsTr("Link")
        iconName: ProjectController.selectionLinked ? "unlink" : "link"
        command: "clip.link"
        onTriggered: ProjectController.toggleLinkSelection()
    }
    // 0.2: Premiere's Group / Ungroup (Ctrl/Cmd+G, Ctrl/Cmd+Shift+G).
    ContextMenuItem {
        objectName: "groupMenuItem"
        text: qsTr("Group")
        iconName: "layers"
        command: "clip.group"
        enabled: ProjectController.selectedClips.length > 1
        onTriggered: ProjectController.groupSelection()
    }
    ContextMenuItem {
        objectName: "ungroupMenuItem"
        text: qsTr("Ungroup")
        iconName: "layers"
        command: "clip.ungroup"
        enabled: ProjectController.selectionGrouped
        onTriggered: ProjectController.ungroupSelection()
    }
    ContextMenuItem {
        text: qsTr("Speed/Duration…")
        iconName: "gauge"
        command: "clip.speedDuration"
        onTriggered: KeymapDispatcher.trigger("clip.speedDuration") // Main.qml opens the dialog
    }
    // 0.5 (stream C): Premiere's Scene Edit Detection… on the selected video clips; its dialog opens once the menu has
    // closed. Without a video clip it is greyed out and says why.
    ContextMenuItem {
        objectName: "sceneEditDetectionMenuItem"
        text: clipMenu.sceneInfo.enabled === true || (clipMenu.sceneInfo.note ?? "") === ""
              ? qsTr("Scene Edit Detection…") : qsTr("Scene Edit Detection… (%1)").arg(clipMenu.sceneInfo.note)
        iconName: "clapperboard"
        command: "clip.sceneEditDetection"
        enabled: clipMenu.sceneInfo.enabled === true
        Accessible.description: clipMenu.sceneInfo.reason ?? ""
        onTriggered: clipMenu.pendingCommand = "clip.sceneEditDetection"
    }
    // P.2 (E5): Premiere's Clip > Video Options (frame holds, Scale to Frame Size, Set to Frame Size / Fill Frame),
    // Replace With Clip and Join Through Edits (on a clip at a through edit).
    VideoOptionsSubmenu {
        id: videoOptionsMenu
        clipKey: clipMenu.clipId
    }
    // P.2: Clip > Video Options > Time Interpolation (next to Video Options in this menu).
    TimeInterpolationSubmenu {
        id: interpolationMenu
    }
    ReplaceWithClipSubmenu {}
    ContextMenuItem {
        objectName: "joinThroughEditsMenuItem"
        visible: videoOptionsMenu.info.throughEdit === true
        height: visible ? implicitHeight : 0
        text: qsTr("Join Through Edits")
        iconName: "link"
        command: "sequence.joinThroughEdits"
        onTriggered: Editing.joinThroughEdits(clipMenu.clipId)
    }
    // M5o (gaming): one-click slow-mo, ramps, freeze + replay, fast forward and zoom punch-ins for this clip.
    MomentsSubmenu {
        id: momentsMenu
        clipKey: clipMenu.clipId
    }
    // M5o (gaming): the facecam / vertical layout picker (C.15 layout.apply).
    ContextMenuItem {
        objectName: "layoutMenuItem"
        text: qsTr("Layout…")
        iconName: "layout-grid"
        onTriggered: Layouts.open()
    }
    // ---- Nesting and multi-camera (M5l) ----
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "makeSubsequenceMenuItem"
        text: qsTr("Make Subsequence")
        iconName: "copy"
        command: "sequence.makeSubsequence"
        enabled: clipMenu.selection.length > 0
        onTriggered: Editing.makeSubsequence(false)
    }
    ContextMenuItem {
        objectName: "nestMenuItem"
        text: qsTr("Nest…")
        iconName: "layers"
        enabled: clipMenu.selection.length > 0
        onTriggered: clipMenu.nestRequested()
    }
    ContextMenuItem {
        objectName: "openNestedMenuItem"
        visible: clipMenu.nestInfo.nested === true
        height: visible ? implicitHeight : 0
        text: clipMenu.nestInfo.multicam ? qsTr("Open Multi-Camera Source") : qsTr("Open Nested Sequence")
        iconName: "folder-open"
        onTriggered: Nesting.openNested(clipMenu.clipId)
    }
    ContextMenuItem {
        objectName: "revealNestedMenuItem"
        visible: clipMenu.nestInfo.nested === true
        height: visible ? implicitHeight : 0
        text: qsTr("Reveal Nested Sequence in Library")
        iconName: "library"
        onTriggered: Nesting.revealInLibrary(clipMenu.clipId)
    }
    ContextMenuItem {
        objectName: "unnestMenuItem"
        visible: clipMenu.nestInfo.nested === true && clipMenu.nestInfo.multicam !== true
        height: visible ? implicitHeight : 0
        text: qsTr("Unnest")
        iconName: "fold-vertical"
        onTriggered: Nesting.unnest(clipMenu.clipId)
    }
    ContextMenuItem {
        objectName: "flattenMenuItem"
        visible: clipMenu.nestInfo.multicam === true
        height: visible ? implicitHeight : 0
        text: qsTr("Flatten")
        iconName: "layout-grid"
        onTriggered: Multicam.flatten(clipMenu.clipId)
    }
    // ---- Audio tools (A.4 / A.5) ----
    ContextMenuSeparator {
        visible: clipMenu.audioInfo.audio === true
        height: visible ? implicitHeight : 0
    }
    // 0.4: Premiere's Audio Gain… (G) on the selected clips: Main opens the dialog after the menu has closed.
    ContextMenuItem {
        objectName: "audioGainMenuItem"
        visible: clipMenu.audioInfo.audio === true
        height: visible ? implicitHeight : 0
        text: qsTr("Audio Gain…")
        iconName: "volume-2"
        command: "clip.audioGain"
        onTriggered: clipMenu.pendingCommand = "clip.audioGain"
    }
    ContextMenuItem {
        objectName: "enhanceSpeechMenuItem"
        visible: clipMenu.audioInfo.audio === true
        height: visible ? implicitHeight : 0
        text: qsTr("Enhance Speech…")
        iconName: "audio-lines"
        onTriggered: ClipAudio.openEnhance(clipMenu.selection.length > 0 ? clipMenu.selection : [clipMenu.clipId])
    }
    ContextMenuItem {
        objectName: "renderEnhancementMenuItem"
        visible: clipMenu.audioInfo.badge === "missingRender"
        height: visible ? implicitHeight : 0
        text: qsTr("Render Enhancement")
        iconName: "rotate-ccw"
        onTriggered: ClipAudio.enhanceSpeech(clipMenu.selection.length > 0 ? clipMenu.selection : [clipMenu.clipId],
                                             clipMenu.audioInfo.strength ?? 100, true)
    }
    ContextMenuItem {
        objectName: "separateStemsMenuItem"
        visible: clipMenu.audioInfo.audio === true
        height: visible ? implicitHeight : 0
        text: qsTr("Separate Stems…")
        iconName: "music"
        onTriggered: ClipAudio.openStems(clipMenu.clipId)
    }
    ContextMenuItem {
        objectName: "synchronizeMenuItem"
        visible: clipMenu.audioInfo.audio === true || clipMenu.selection.length > 1
        height: visible ? implicitHeight : 0
        enabled: clipMenu.selection.length > 1
        text: qsTr("Synchronize")
        iconName: "arrow-left-right"
        onTriggered: ClipAudio.synchronize(clipMenu.selection)
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        text: qsTr("Rename…")
        iconName: "pencil"
        onTriggered: clipMenu.renameRequested(clipMenu.clipId, clipMenu.menuPoint)
    }
    // A nested clip keeps "Reveal Nested Sequence in Library" above (the same action).
    ContextMenuItem {
        objectName: "revealInProjectMenuItem"
        visible: clipMenu.nestInfo.nested !== true
        height: visible ? implicitHeight : 0
        text: qsTr("Reveal in Project")
        iconName: "library"
        command: "clip.revealInProject"
        enabled: clipMenu.revealInfo.inProject === true
        onTriggered: Editing.revealInProject(clipMenu.clipId)
    }
    ContextMenuItem {
        objectName: "revealInExplorerMenuItem"
        // Clips with a media file (not titles, generators or nested sequences); a missing file shows why it is off.
        visible: clipMenu.revealInfo.inExplorer === true || clipMenu.revealInfo.offline === true
        height: visible ? implicitHeight : 0
        text: clipMenu.revealInfo.offline === true
              ? (Qt.platform.os === "osx" ? qsTr("Reveal in Finder (file missing)") : qsTr("Reveal in File Explorer (file missing)"))
              : (Qt.platform.os === "osx" ? qsTr("Reveal in Finder") : qsTr("Reveal in File Explorer"))
        iconName: "folder-open"
        enabled: clipMenu.revealInfo.inExplorer === true
        onTriggered: Editing.revealInExplorer(clipMenu.clipId)
    }
    // Label colour swatches
    Item {
        objectName: "clipLabelColors"
        implicitWidth: labelRow.implicitWidth + Theme.space(6)
        implicitHeight: labelRow.implicitHeight + Theme.space(2)

        Row {
            id: labelRow
            x: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space(0.5)

            Repeater {
                model: Theme.clipColors.length

                delegate: Swatch {
                    required property int index
                    swatchColor: Theme.clipColor(index)
                    checkable: false
                    tooltip: qsTr("Label color %1").arg(index + 1)
                    onClicked: {
                        ProjectController.setSelectionColor(index)
                        clipMenu.close()
                    }
                }
            }
        }
    }
}
