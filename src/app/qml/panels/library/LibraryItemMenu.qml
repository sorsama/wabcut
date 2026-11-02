pragma ComponentBehavior: Bound
import QtQuick

// The Library's right-click menu (M5k; it absorbed I.3's proxy menu): open, rename, label, comment, subclips, poster
// frame, reveal, proxies, delete. Acts on the selection where Premiere does (label, new bin, delete).
// P1 round (0.6): Link Media… on offline items (#43), Move to Bin ▸ with the bin tree, Cut and Paste (#41), the Proxy
// rows on every selected item and on a whole bin (#44), and Properties… (#39).
// P1 round (0.6, stream 2D): Find in Timeline ▸ lists where the item is used and opens the sequence there (#40), and
// Delete / Delete with Options… ask first when sequences use what goes (#38: Library.requestDelete).
ContextMenu {
    id: menu
    objectName: "libraryItemMenu"

    property string key: ""
    property string kind: ""
    /// Where hover scrub last was on the card (poster frame "here"), or -1.
    property real scrubFraction: -1
    readonly property bool isMedia: kind !== "" && kind !== "bin" && kind !== "sequence"
    readonly property var proxy: isMedia ? (MediaReadiness.mediaStates[key] ?? null) : null
    readonly property string proxyState: proxy ? proxy.state : ""
    readonly property bool making: proxyState === "generating" || proxyState === "paused" || proxyState === "queued"
    /// What the menu's selection rows act on: the selection when the item is in it, else the item.
    readonly property var targets: Library.isSelected(menu.key) ? Library.selection : [menu.key]
    /// Read when the menu opens: the bins Move to Bin offers (Library.binTree), the Proxy rows' counts
    /// (Library.proxyPlan) and the offline items of `targets` (Link Media…).
    property var bins: []
    property var proxyPlan: ({ targets: 0, create: 0, ready: 0, making: 0, deletable: 0 })
    property var offlineTargets: []
    /// P1 round (#40): where the item is used (Library.usages; bins: none), for Find in Timeline ▸.
    property var usages: []
    readonly property int usageLimit: 40
    /// The Proxy rows act on several items (a bin, or a selection of more than one).
    readonly property bool proxyMany: menu.kind === "bin" || (menu.proxyPlan.targets ?? 0) > 1

    signal renameRequested(string key)
    signal metadataRequested(string key)
    // 0.2 (P.3)
    signal metadataPanelRequested()
    signal editSearchBinRequested(string key)
    signal subclipRequested(string key)
    signal editSubclipRequested(string key)
    signal openRequested(string key, string kind)
    /// Reveal in Source Project (0.2, P.3): a copy imported from another project.
    signal revealSourceRequested(string key)
    /// P1 round (#43): Link Media… on the offline items `keys` (the Relink dialog, filtered to them).
    signal linkMediaRequested(var keys)
    /// P1 round (#39): Properties… of `key`.
    signal propertiesRequested(string key)

    function openFor(target, itemKey, itemKind, point, fraction) {
        key = itemKey
        kind = itemKind
        scrubFraction = fraction === undefined ? -1 : fraction
        const chosen = Library.isSelected(itemKey) ? Library.selection : [itemKey]
        bins = Library.binTree(chosen)
        proxyPlan = Library.proxyPlan(itemKey)
        offlineTargets = chosen.filter(k => Library.details(k).offline === true)
        usages = itemKind === "bin" ? [] : Library.usages(itemKey)
        popup(target, point.x, point.y)
    }
    /// A row that opens a dialog runs once the menu has closed (the dialog then keeps the focus).
    property var pendingAction: null
    onClosed: {
        const action = menu.pendingAction
        menu.pendingAction = null
        if (action !== null)
            action()
    }

    // P1 round (#43): Premiere's Link Media… first on an offline item.
    ContextMenuItem {
        objectName: "libraryMenuLinkMedia"
        visible: menu.offlineTargets.length > 0
        height: visible ? implicitHeight : 0
        text: menu.offlineTargets.length > 1 ? qsTr("Link Media… (%1)").arg(menu.offlineTargets.length) : qsTr("Link Media…")
        iconName: "link"
        onTriggered: {
            const keys = menu.offlineTargets
            menu.pendingAction = () => menu.linkMediaRequested(keys)
        }
    }
    ContextMenuSeparator {
        visible: menu.offlineTargets.length > 0
        height: visible ? implicitHeight : 0
    }
    ContextMenuItem {
        objectName: "libraryMenuOpen"
        text: menu.kind === "bin" ? qsTr("Open bin") : menu.kind === "sequence" ? qsTr("Open in timeline")
                                                                                : qsTr("Open in Source Monitor")
        iconName: menu.kind === "bin" ? "folder-open" : "film"
        onTriggered: menu.openRequested(menu.key, menu.kind)
    }
    ContextMenuItem {
        objectName: "libraryMenuOpenTab"
        visible: menu.kind === "bin"
        height: visible ? implicitHeight : 0
        text: qsTr("Open bin in new tab")
        iconName: "external-link"
        onTriggered: Library.openBinInTab(menu.key)
    }
    ContextMenuItem {
        objectName: "libraryMenuEditSearchBin"
        visible: menu.kind === "bin" && (Library.details(menu.key).searchBin ?? false)
        height: visible ? implicitHeight : 0
        text: qsTr("Edit search bin…")
        iconName: "folder-search"
        onTriggered: menu.editSearchBinRequested(menu.key)
    }
    // P1 round (#40): Find in Timeline ▸ every place the item is used (the card's usage badge lists the same).
    ContextMenu {
        id: findMenu
        objectName: "libraryMenuFindInTimeline"
        title: qsTr("Find in Timeline")
        enabled: menu.kind !== "bin"
        ContextMenuItem {
            objectName: "libraryMenuFindInTimelineNone"
            visible: menu.usages.length === 0
            height: visible ? implicitHeight : 0
            enabled: false
            text: qsTr("Not used in any sequence")
        }
        Instantiator {
            model: menu.usages.slice(0, menu.usageLimit)
            delegate: ContextMenuItem {
                id: useRow
                required property var modelData
                required property int index
                objectName: "libraryMenuFindInTimeline_" + useRow.index
                //: A place a Library item is used: sequence, timecode, tracks, e.g. "Sequence 01 · 00:00:12:03 · V1, A1"
                text: qsTr("%1 · %2 · %3").arg(useRow.modelData.sequenceName).arg(useRow.modelData.timecode)
                                           .arg(useRow.modelData.tracks)
                iconName: "film"
                Accessible.description: useRow.modelData.clipName
                onTriggered: {
                    const sequence = useRow.modelData.sequenceKey
                    const clip = useRow.modelData.clipId
                    menu.pendingAction = () => Library.findInTimeline(sequence, clip)
                }
            }
            onObjectAdded: (index, object) => findMenu.insertItem(index + 1, object)
            onObjectRemoved: (index, object) => findMenu.removeItem(object)
        }
        ContextMenuItem {
            visible: menu.usages.length > menu.usageLimit
            height: visible ? implicitHeight : 0
            enabled: false
            //: More places than the menu lists, e.g. "12 more"
            text: qsTr("%n more", "", Math.max(0, menu.usages.length - menu.usageLimit))
        }
    }
    ContextMenuSeparator {}
    // P1 round (#41): Cut and Paste move items between bins (Premiere's Project panel Edit > Cut / Paste).
    ContextMenuItem {
        objectName: "libraryMenuCut"
        text: qsTr("Cut")
        iconName: "scissors"
        command: "edit.cut"
        enabled: menu.targets.length > 0
        onTriggered: Library.cutItems(menu.targets)
    }
    ContextMenuItem {
        objectName: "libraryMenuPaste"
        text: Library.cutKeys.length > 0 ? qsTr("Paste (%1)").arg(Library.cutKeys.length) : qsTr("Paste")
        iconName: "clipboard-paste"
        command: "edit.paste"
        enabled: Library.cutKeys.length > 0
        onTriggered: Library.pasteItems()
    }
    ContextMenuItem {
        objectName: "libraryMenuRename"
        text: qsTr("Rename")
        iconName: "pencil"
        enabled: menu.kind !== "sequence"
        onTriggered: menu.renameRequested(menu.key)
    }
    ContextMenu {
        id: labelMenu
        objectName: "libraryMenuLabel"
        title: qsTr("Label")
        enabled: Library.mediaSelected || menu.kind === "sequence" // 0.2: sequences have labels too
        Instantiator {
            model: Library.labelNames
            delegate: ContextMenuItem {
                required property string modelData
                required property int index
                objectName: "libraryMenuLabel_" + index
                text: modelData
                iconName: "square"
                iconColor: Theme.clipColor(index)
                onTriggered: Library.setLabel(Library.isSelected(menu.key) ? Library.selection : [menu.key], index)
            }
            onObjectAdded: (index, object) => labelMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => labelMenu.removeItem(object)
        }
    }
    ContextMenuItem {
        objectName: "libraryMenuMetadata"
        visible: menu.isMedia
        height: visible ? implicitHeight : 0
        text: qsTr("Comment and description…")
        iconName: "pencil"
        onTriggered: menu.metadataRequested(menu.key)
    }
    ContextMenuItem {
        objectName: "libraryMenuMetadataPanel"
        visible: menu.isMedia
        height: visible ? implicitHeight : 0
        text: qsTr("Metadata…")
        iconName: "tag"
        onTriggered: menu.metadataPanelRequested()
    }
    ContextMenuItem {
        objectName: "libraryMenuReadXmp"
        visible: menu.isMedia && menu.kind !== "title"
        height: visible ? implicitHeight : 0
        text: qsTr("Read metadata from file (XMP)")
        iconName: "import"
        onTriggered: Library.readXmp(Library.isSelected(menu.key) ? Library.selection : [menu.key])
    }
    ContextMenuItem {
        objectName: "libraryMenuWriteXmp"
        visible: menu.isMedia && menu.kind !== "title"
        height: visible ? implicitHeight : 0
        text: qsTr("Write XMP sidecar")
        iconName: "save"
        onTriggered: Library.writeXmp(Library.isSelected(menu.key) ? Library.selection : [menu.key])
    }
    // P1 round (#41): Move to Bin ▸ the project's bin tree ("Project" = the root), then New Bin… from the selection.
    ContextMenu {
        id: moveMenu
        objectName: "libraryMenuMoveToBin"
        title: qsTr("Move to Bin")
        enabled: menu.targets.length > 0
        Instantiator {
            model: menu.bins
            delegate: ContextMenuItem {
                id: binRow
                required property var modelData
                required property int index
                objectName: "libraryMenuMoveTo_" + (binRow.modelData.key === "" ? "root" : binRow.modelData.key)
                // The tree: each level a step further in.
                leftPadding: Theme.space(3.5) + binRow.modelData.depth * Theme.space(3)
                text: binRow.modelData.name
                iconName: binRow.modelData.key === "" ? "library" : "folder"
                enabled: !binRow.modelData.disabled
                // Fix round 1 (ARGON-22): the bin they are all in already is ticked and greyed out, with why.
                checkable: binRow.modelData.current === true
                checked: binRow.modelData.current === true
                disabledReason: binRow.modelData.reason ?? ""
                onTriggered: Library.moveItems(menu.targets, binRow.modelData.key)
            }
            onObjectAdded: (index, object) => moveMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => moveMenu.removeItem(object)
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            objectName: "libraryMenuMoveToNewBin"
            text: qsTr("New Bin…")
            iconName: "folder-plus"
            enabled: Library.selectionCount > 0
            onTriggered: Library.newBinFromSelection()
        }
    }
    ContextMenuItem {
        objectName: "libraryMenuNewBin"
        text: qsTr("New bin from selection")
        iconName: "folder-plus"
        enabled: Library.selectionCount > 0
        onTriggered: Library.newBinFromSelection()
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "libraryMenuSubclip"
        visible: menu.isMedia && menu.kind !== "still" && menu.kind !== "title"
        height: visible ? implicitHeight : 0
        text: qsTr("Make subclip…")
        iconName: "scissors"
        onTriggered: menu.subclipRequested(menu.key)
    }
    ContextMenuItem {
        objectName: "libraryMenuEditSubclip"
        visible: menu.kind === "subclip"
        height: visible ? implicitHeight : 0
        text: qsTr("Edit subclip…")
        iconName: "scissors"
        onTriggered: menu.editSubclipRequested(menu.key)
    }
    // P.2 (E5): Premiere's Duplicate, New Sequence From Clip, Sequence Settings…, Merge Clips…, Modify > Interpret
    // Footage… / Timecode…, Replace Footage…, Make Offline and Edit Original (the Editing bridge).
    ContextMenuItem {
        objectName: "libraryMenuDuplicate"
        visible: menu.kind !== "bin"
        height: visible ? implicitHeight : 0
        text: qsTr("Duplicate")
        iconName: "copy"
        command: "edit.duplicate"
        onTriggered: Editing.duplicate(Library.isSelected(menu.key) ? Library.selection : [menu.key])
    }
    ContextMenuItem {
        objectName: "libraryMenuNewSequenceFromClip"
        visible: menu.isMedia
        height: visible ? implicitHeight : 0
        text: qsTr("New Sequence From Clip")
        iconName: "clapperboard"
        command: "file.newSequenceFromClip"
        onTriggered: Editing.newSequenceFromClip(menu.key)
    }
    ContextMenuItem {
        objectName: "libraryMenuSequenceSettings"
        visible: menu.kind === "sequence"
        height: visible ? implicitHeight : 0
        text: qsTr("Sequence Settings…")
        iconName: "sliders-horizontal"
        command: "sequence.settings"
        onTriggered: Editing.openSequenceSettings(menu.key)
    }
    // P1 round (#22, stream 2C): export the selected sequences straight from the Library (Premiere: File > Export >
    // Media with sequences selected in the Project panel). One sequence's Export… opens it in the Export workspace;
    // more sequences, or Send to Export Queue…, open the Export Sequences dialog (Main, ExportController's signal).
    ContextMenuItem {
        id: exportSequencesItem
        readonly property var sequenceKeys: menu.targets.filter(k => String(k).startsWith("s"))
        objectName: "libraryMenuExportSequences"
        visible: exportSequencesItem.sequenceKeys.length > 0
        height: visible ? implicitHeight : 0
        text: exportSequencesItem.sequenceKeys.length > 1 ? qsTr("Export %n Sequences…", "", exportSequencesItem.sequenceKeys.length) : qsTr("Export…")
        iconName: "upload"
        onTriggered: {
            const keys = exportSequencesItem.sequenceKeys.slice()
            menu.pendingAction = () => ExportController.sequencesExportRequested(keys, false)
        }
    }
    ContextMenuItem {
        id: queueSequencesItem
        readonly property var sequenceKeys: menu.targets.filter(k => String(k).startsWith("s"))
        objectName: "libraryMenuQueueSequences"
        visible: queueSequencesItem.sequenceKeys.length > 0
        height: visible ? implicitHeight : 0
        text: qsTr("Send to Export Queue…")
        iconName: "list"
        onTriggered: {
            const keys = queueSequencesItem.sequenceKeys.slice()
            menu.pendingAction = () => ExportController.sequencesExportRequested(keys, true)
        }
    }
    ContextMenuItem {
        objectName: "libraryMenuMergeClips"
        visible: menu.isMedia && Library.selectionCount > 1
        height: visible ? implicitHeight : 0
        text: qsTr("Merge Clips…")
        iconName: "layers"
        command: "clip.mergeClips"
        onTriggered: Editing.openMergeClips(Library.selection)
    }
    ContextMenu {
        id: modifyMenu
        objectName: "libraryMenuModify"
        title: qsTr("Modify")
        enabled: menu.isMedia && menu.kind !== "title"
        ContextMenuItem {
            objectName: "libraryMenuInterpretFootage"
            text: qsTr("Interpret Footage…")
            iconName: "film"
            command: "clip.interpretFootage"
            onTriggered: Editing.openInterpretFootage(Library.isSelected(menu.key) ? Library.selection : [menu.key])
        }
        ContextMenuItem {
            // 0.2: camera RAW (ProRes RAW, CinemaDNG) - exposure and white balance of the development.
            objectName: "libraryMenuRawSourceSettings"
            text: qsTr("Source Settings…")
            iconName: "sliders-horizontal"
            command: "clip.rawSourceSettings"
            enabled: Editing.isRawMedia(menu.key)
            onTriggered: Editing.openRawSourceSettings(Library.isSelected(menu.key) ? Library.selection : [menu.key])
        }
        ContextMenuItem {
            objectName: "libraryMenuTimecode"
            text: qsTr("Timecode…")
            iconName: "clock"
            command: "clip.modifyTimecode"
            onTriggered: Editing.openTimecode(Library.isSelected(menu.key) ? Library.selection : [menu.key])
        }
    }
    ContextMenuItem {
        objectName: "libraryMenuReplaceFootage"
        visible: menu.isMedia && menu.kind !== "title"
        height: visible ? implicitHeight : 0
        text: qsTr("Replace Footage…")
        iconName: "import"
        command: "clip.replaceFootage"
        onTriggered: Editing.openReplaceFootage(menu.key)
    }
    ContextMenuItem {
        objectName: "libraryMenuMakeOffline"
        visible: menu.isMedia && menu.kind !== "title"
        height: visible ? implicitHeight : 0
        text: qsTr("Make Offline")
        iconName: "eye-off"
        command: "clip.makeOffline"
        onTriggered: Editing.makeOffline(Library.isSelected(menu.key) ? Library.selection : [menu.key])
    }
    ContextMenuItem {
        objectName: "libraryMenuEditOriginal"
        visible: menu.isMedia && menu.kind !== "title"
        height: visible ? implicitHeight : 0
        text: qsTr("Edit Original")
        iconName: "external-link"
        command: "edit.editOriginal"
        onTriggered: Editing.editOriginal(menu.key)
    }
    ContextMenuSeparator {}
    // M5l: Premiere's Create Multi-Camera Source Sequence… on the selected media (the angles, in Library order).
    ContextMenuItem {
        objectName: "libraryMenuMulticam"
        visible: menu.isMedia && menu.kind !== "title" && menu.kind !== "still"
        height: visible ? implicitHeight : 0
        text: qsTr("Create Multi-Camera Source Sequence…")
        iconName: "layout-grid"
        onTriggered: Multicam.openCreate(Library.selection.length > 0 ? Library.selection : [menu.key])
    }
    // A.9 (M5n): Modify > Audio Channels… for media items (audio.setMediaChannels).
    ContextMenuItem {
        objectName: "libraryMenuAudioChannels"
        visible: menu.isMedia && menu.kind !== "still" && menu.kind !== "title"
        height: visible ? implicitHeight : 0
        text: qsTr("Audio Channels…")
        iconName: "sliders-horizontal"
        onTriggered: AudioChannels.openForMedia([menu.key])
    }
    ContextMenuItem {
        objectName: "libraryMenuPoster"
        visible: menu.isMedia && menu.kind !== "audio" && menu.kind !== "title"
        height: visible ? implicitHeight : 0
        enabled: menu.scrubFraction >= 0
        text: qsTr("Set poster frame here")
        iconName: "image"
        onTriggered: Library.setPosterFrame(menu.key, menu.scrubFraction)
    }
    ContextMenuItem {
        visible: menu.isMedia && (Library.details(menu.key).posterText ?? "") !== ""
        height: visible ? implicitHeight : 0
        text: qsTr("Clear poster frame")
        iconName: "x"
        onTriggered: Library.setPosterFrame(menu.key, -1)
    }
    ContextMenuItem {
        objectName: "libraryMenuReveal"
        visible: menu.isMedia && menu.kind !== "title"
        height: visible ? implicitHeight : 0
        text: Qt.platform.os === "osx" ? qsTr("Reveal in Finder") : qsTr("Reveal in File Explorer")
        iconName: "folder-open"
        onTriggered: Library.reveal(menu.key)
    }
    ContextMenuItem {
        id: revealSourceItem
        objectName: "libraryMenuRevealSource"
        readonly property var source: ProjectController.revision >= 0 && menu.key !== "" && menu.kind !== "bin"
                                      ? Production.sourceOf(menu.key) : ({})
        visible: (revealSourceItem.source.projectName ?? "") !== ""
        height: visible ? implicitHeight : 0
        text: qsTr("Reveal in Source Project (%1)").arg(revealSourceItem.source.projectName ?? "")
        iconName: "layers"
        onTriggered: menu.revealSourceRequested(menu.key)
    }
    // P1 round (#44): the Proxy rows act on every selected item (and a bin's media), with the counts in the labels.
    ContextMenu {
        objectName: "libraryMenuProxy"
        title: qsTr("Proxy")
        enabled: menu.kind === "bin" || (menu.proxyPlan.targets ?? 0) > 0
        ContextMenuItem {
            objectName: "createProxyItem"
            text: menu.kind === "bin" ? qsTr("Create proxies for all media in this bin (%1)").arg(menu.proxyPlan.create ?? 0)
                  : menu.proxyMany ? qsTr("Create proxies (%1)").arg(menu.proxyPlan.create ?? 0)
                  : menu.proxyState === "ready" ? qsTr("Remake proxy") : qsTr("Create proxy")
            iconName: "film"
            enabled: menu.proxyMany ? (menu.proxyPlan.create ?? 0) > 0 : !menu.making
            onTriggered: {
                if (menu.proxyMany) {
                    Library.createProxies(menu.key)
                    return
                }
                if (menu.proxyState === "ready")
                    MediaReadiness.deleteProxies([menu.key])
                MediaReadiness.generateProxies([menu.key])
            }
        }
        ContextMenuItem {
            objectName: "remakeProxiesItem"
            visible: menu.proxyMany
            height: visible ? implicitHeight : 0
            text: qsTr("Remake proxies (%1)").arg(menu.proxyPlan.ready ?? 0)
            iconName: "rotate-cw"
            enabled: (menu.proxyPlan.ready ?? 0) > 0
            onTriggered: Library.remakeProxies(menu.key)
        }
        ContextMenuItem {
            visible: !menu.proxyMany
            height: visible ? implicitHeight : 0
            text: qsTr("Reveal proxy")
            iconName: "eye"
            enabled: menu.proxyState === "ready" || menu.proxyState === "stale"
            onTriggered: MediaReadiness.revealProxy(menu.key)
        }
        ContextMenuItem {
            objectName: "deleteProxyItem"
            text: menu.proxyMany ? qsTr("Delete proxies (%1)").arg(menu.proxyPlan.deletable ?? 0)
                  : menu.making ? qsTr("Cancel proxy") : qsTr("Delete proxy")
            iconName: "trash-2"
            enabled: menu.proxyMany ? (menu.proxyPlan.deletable ?? 0) > 0
                                    : menu.proxyState !== "" && menu.proxyState !== "failed"
            onTriggered: {
                if (menu.proxyMany)
                    Library.deleteProxies(menu.key)
                else
                    MediaReadiness.deleteProxies([menu.key])
            }
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            text: qsTr("Proxy and cache settings…")
            iconName: "settings"
            onTriggered: MediaReadiness.openSettings()
        }
    }
    // P1 round (#39): Premiere's Properties (File > Get Properties for > Selection, Ctrl/Cmd+Shift+H).
    ContextMenuItem {
        objectName: "libraryMenuProperties"
        text: qsTr("Properties…")
        iconName: "list"
        command: "file.getProperties"
        onTriggered: {
            const itemKey = menu.key
            menu.pendingAction = () => menu.propertiesRequested(itemKey)
        }
    }
    ContextMenuSeparator {}
    // P1 round (#38): items sequences use ask first (the panel's confirmation offers to remove their clips too).
    ContextMenuItem {
        objectName: "libraryMenuDelete"
        text: qsTr("Delete")
        iconName: "trash-2"
        command: "project.delete"
        enabled: menu.targets.length > 0
        onTriggered: {
            const keys = menu.targets
            menu.pendingAction = () => Library.requestDelete(keys, false)
        }
    }
    ContextMenuItem {
        objectName: "libraryMenuDeleteWithOptions"
        text: qsTr("Delete with Options…")
        iconName: "trash-2"
        command: "project.deleteWithOptions"
        enabled: menu.targets.length > 0
        onTriggered: {
            const keys = menu.targets
            menu.pendingAction = () => Library.requestDelete(keys, true)
        }
    }
}
