pragma ComponentBehavior: Bound
import QtQuick

// The Library's right-click menu (M5k; it absorbed I.3's proxy menu): open, rename, label, comment, subclips, poster
// frame, reveal, proxies, delete. Acts on the selection where Premiere does (label, new bin, delete).
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

    signal renameRequested(string key)
    signal metadataRequested(string key)
    signal subclipRequested(string key)
    signal editSubclipRequested(string key)
    signal openRequested(string key, string kind)

    function openFor(target, itemKey, itemKind, point, fraction) {
        key = itemKey
        kind = itemKind
        scrubFraction = fraction === undefined ? -1 : fraction
        popup(target, point.x, point.y)
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
    ContextMenuSeparator {}
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
        enabled: Library.mediaSelected
        Instantiator {
            model: Library.labelNames
            delegate: ContextMenuItem {
                required property string modelData
                required property int index
                objectName: "libraryMenuLabel_" + index
                text: modelData
                iconName: "square"
                iconColor: Theme.clipColor(index)
                onTriggered: Library.setLabel(Library.selection, index)
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
    ContextMenu {
        objectName: "libraryMenuProxy"
        title: qsTr("Proxy")
        enabled: menu.isMedia && menu.kind !== "audio" && menu.kind !== "title"
        ContextMenuItem {
            objectName: "createProxyItem"
            text: menu.proxyState === "ready" ? qsTr("Remake proxy") : qsTr("Create proxy")
            iconName: "film"
            enabled: !menu.making
            onTriggered: {
                if (menu.proxyState === "ready")
                    MediaReadiness.deleteProxies([menu.key])
                MediaReadiness.generateProxies([menu.key])
            }
        }
        ContextMenuItem {
            text: qsTr("Reveal proxy")
            iconName: "eye"
            enabled: menu.proxyState === "ready" || menu.proxyState === "stale"
            onTriggered: MediaReadiness.revealProxy(menu.key)
        }
        ContextMenuItem {
            objectName: "deleteProxyItem"
            text: menu.making ? qsTr("Cancel proxy") : qsTr("Delete proxy")
            iconName: "trash-2"
            enabled: menu.proxyState !== "" && menu.proxyState !== "failed"
            onTriggered: MediaReadiness.deleteProxies([menu.key])
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            text: qsTr("Proxy and cache settings…")
            iconName: "settings"
            onTriggered: MediaReadiness.openSettings()
        }
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "libraryMenuDelete"
        text: qsTr("Delete")
        iconName: "trash-2"
        command: "project.delete"
        enabled: Library.selectionCount > 0
        onTriggered: Library.deleteSelection()
    }
}
