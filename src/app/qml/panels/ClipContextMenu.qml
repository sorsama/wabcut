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
ContextMenu {
    id: clipMenu
    objectName: "clipContextMenu"

    property string clipId: ""
    property point menuPoint

    signal renameRequested(string clipId, point at)
    /// Nest…: the panel asks for the new sequence's name.
    signal nestRequested()

    // What the clip is (read when the menu opens).
    property var nestInfo: ({})
    property var audioInfo: ({})
    property var selection: []

    function openAt(target, id, point) {
        clipId = id
        menuPoint = point
        nestInfo = Nesting.clipInfo(id)
        audioInfo = ClipAudio.clipInfo(id)
        selection = ClipAudio.selectedClips()
        showChannels()
        showTakes()
        momentsMenu.refresh()
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
    ContextMenuItem {
        objectName: "pasteMenuItem"
        text: qsTr("Paste")
        iconName: "clipboard-paste"
        command: "edit.paste"
        enabled: ProjectController.canPaste
        onTriggered: ProjectController.paste()
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
    ContextMenuItem {
        text: qsTr("Speed/Duration…")
        iconName: "gauge"
        command: "clip.speedDuration"
        onTriggered: KeymapDispatcher.trigger("clip.speedDuration") // Main.qml opens the dialog
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
