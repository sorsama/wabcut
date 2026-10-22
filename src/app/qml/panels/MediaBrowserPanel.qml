pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

// ● MEDIA BROWSER (0.2, Premiere's Media Browser; Shift+8): browse drives and folders and import from them. Camera cards
// (AVCHD, Canon XF, P2, XDCAM, XAVC) show their clips instead of their folders, and a recording the camera split into
// several files (GoPro chapters, AVCHD spans, Canon XF parts) is one clip (Library.browseFolder, editor/CameraCards.h).
// Double-click a folder to open it, a clip to show it in the Source Monitor without importing it (its first mark or
// edit there imports it); Import brings the selected clips into the current bin (one undo step). Ingest (its settings
// beside the switch) first copies the clips to a folder, transcodes them there with an export preset, or makes their
// proxies (Library.importFromBrowser). Hovering a thumbnail scrubs through the clip (a strip of frames, Library's
// hover scrub setting); the metadata view shows what probing the current clip reads. Places: the video and home
// folders and the drives.
Panel {
    id: root

    objectName: "mediaBrowserPanel"
    title: qsTr("Media Browser")

    property string folder: ""
    property var entries: []
    property var selected: ({}) // path -> true
    /// The metadata view of the current clip is shown.
    property bool showMetadata: false
    readonly property var currentEntry: list.currentIndex >= 0 && list.currentIndex < root.entries.length
                                        ? root.entries[list.currentIndex] : null
    readonly property bool currentIsClip: root.currentEntry !== null && root.currentEntry.folder !== true
    /// What Library.browserFileInfo read of the current clip (only while the metadata view is shown).
    readonly property var metadata: root.showMetadata && root.currentIsClip ? Library.browserFileInfo(root.currentEntry.path) : ({})
    readonly property int selectedCount: Object.keys(root.selected).length
    readonly property var ingestActions: [
        { id: "copy", label: qsTr("Copy") },
        { id: "transcode", label: qsTr("Transcode") },
        { id: "proxies", label: qsTr("Create Proxies") }
    ]

    function focusContent() {
        list.forceActiveFocus(Qt.TabFocusReason)
    }
    function openFolder(path) {
        root.folder = path
        root.entries = Library.browseFolder(path)
        root.selected = ({})
        list.currentIndex = root.entries.length > 0 ? 0 : -1
    }
    function toggle(path, additive) {
        const next = additive ? Object.assign({}, root.selected) : ({})
        if (additive && next[path])
            delete next[path]
        else
            next[path] = true
        root.selected = next
    }
    function importSelected() {
        const files = Object.keys(root.selected)
        if (files.length === 0 && root.currentIsClip)
            files.push(root.currentEntry.path)
        if (files.length > 0)
            Library.importFromBrowser(files)
    }
    function activate(entry) {
        if (entry.folder) {
            root.openFolder(entry.path)
            return
        }
        SourceMonitor.loadFile(entry.path)
    }
    function presetName(id) {
        const found = ExportController.presets.find(p => p.id === id)
        return found ? found.name : id
    }
    function metadataLines(info) {
        if (!info || info.name === undefined)
            return []
        const lines = [{ label: qsTr("Format"), value: info.container }, { label: qsTr("Duration"), value: info.duration }]
        if (info.video !== undefined) {
            const v = info.video
            lines.push({ label: qsTr("Video"), value: [v.codec + (v.profile !== "" ? " " + v.profile : ""),
                                                        v.width + " × " + v.height,
                                                        info.still ? "" : qsTr("%1 fps").arg(v.frameRate) + (v.variableFrameRate ? qsTr(" (variable)") : "")]
                                                       .filter(s => s !== "").join(" · ") })
            lines.push({ label: qsTr("Pixels"), value: [v.pixelFormat, qsTr("%1-bit").arg(v.bitDepth), v.chroma,
                                                         v.alpha ? qsTr("alpha") : ""].filter(s => s !== "").join(" · ") })
            if (!info.still)
                lines.push({ label: qsTr("Fields"), value: v.fieldOrder === "upper" ? qsTr("Upper field first")
                                                           : v.fieldOrder === "lower" ? qsTr("Lower field first")
                                                                                      : qsTr("Progressive") })
        }
        const audio = info.audio ?? []
        for (let i = 0; i < audio.length; ++i) {
            const a = audio[i]
            lines.push({ label: audio.length > 1 ? qsTr("Audio %1").arg(i + 1) : qsTr("Audio"),
                         value: [a.codec, qsTr("%n channel(s)", "", a.channels), qsTr("%1 Hz").arg(a.sampleRate), a.title]
                                    .filter(s => s !== "").join(" · ") })
        }
        if (info.startTimecode !== undefined)
            lines.push({ label: qsTr("Start timecode"), value: info.startTimecode })
        lines.push({ label: qsTr("Size"), value: (info.files > 1 ? qsTr("%1 files, ").arg(info.files) : "")
                                                 + qsTr("%1 MB").arg(Math.round(info.bytes / 1048576)) })
        if (info.card !== "")
            lines.push({ label: qsTr("Camera card"), value: info.card })
        if (info.modified !== undefined)
            lines.push({ label: qsTr("Modified"), value: info.modified.replace("T", " ") })
        return lines
    }

    Component.onCompleted: {
        const places = Library.browserPlaces()
        if (places.length > 0)
            root.openFolder(places[0].path)
    }

    leading: [
        IconButton {
            objectName: "mediaBrowserUp"
            compact: true
            iconName: "chevron-up"
            tooltip: qsTr("Up one folder")
            enabled: Library.parentFolder(root.folder) !== ""
            onClicked: root.openFolder(Library.parentFolder(root.folder))
        },
        IconButton {
            objectName: "mediaBrowserPlaces"
            compact: true
            iconName: "folder-open"
            tooltip: qsTr("Drives and places")
            onClicked: placesMenu.popup()
        }
    ]
    trailing: [
        Toggle {
            objectName: "mediaBrowserIngest"
            text: qsTr("Ingest")
            tooltip: !Library.ingest ? qsTr("Copy, transcode or make proxies of the clips as they are imported")
                     : Library.ingestAction === "proxies" ? qsTr("Creating proxies of the clips imported")
                     : Library.ingestAction === "transcode" ? qsTr("Transcoding to %1 with %2").arg(Library.ingestFolder).arg(root.presetName(Library.ingestPreset))
                     : qsTr("Copying to %1").arg(Library.ingestFolder)
            checked: Library.ingest
            onToggled: {
                Library.ingest = checked
                // Copy and Transcode need a folder: the settings ask for it.
                if (checked && Library.ingestAction !== "proxies" && Library.ingestFolder === "")
                    ingestPopover.open()
                checked = Qt.binding(() => Library.ingest)
            }
        },
        IconButton {
            objectName: "mediaBrowserIngestSettings"
            compact: true
            iconName: "sliders-horizontal"
            tooltip: qsTr("Ingest Settings")
            onClicked: ingestPopover.open()
        },
        IconButton {
            objectName: "mediaBrowserMetadataToggle"
            compact: true
            iconName: "list"
            tooltip: qsTr("Metadata of the clip")
            checkable: true
            checked: root.showMetadata
            onToggled: root.showMetadata = checked
        },
        PillButton {
            objectName: "mediaBrowserImport"
            variant: "primary"
            text: root.selectedCount > 1 ? qsTr("Import %1").arg(root.selectedCount) : qsTr("Import")
            enabled: root.selectedCount > 0 || root.currentIsClip
            onClicked: root.importSelected()
        }
    ]

    FolderDialog {
        id: ingestDialog
        title: qsTr("Ingest into")
        onAccepted: Library.ingestFolder = selectedFolder.toString()
    }

    OptionsMenu {
        id: placesMenu
        options: Library.browserPlaces().map(p => ({ id: p.path, name: p.name }))
        onChosen: id => root.openFolder(id)
    }

    OptionsMenu {
        id: presetMenu
        options: ExportController.presets.map(p => ({ id: p.id, name: p.name }))
        current: Library.ingestPreset
        onChosen: id => Library.ingestPreset = id
    }

    // Premiere's Ingest Settings: what an import from here does first.
    Popover {
        id: ingestPopover
        objectName: "mediaBrowserIngestPopover"
        parent: root
        title: qsTr("Ingest Settings")
        x: Math.max(0, root.width - width - Theme.space(2))
        y: Theme.space(2)
        transformOrigin: Item.TopRight

        Column {
            width: Theme.space(84)
            spacing: Theme.space(3)

            PillTabs {
                objectName: "mediaBrowserIngestAction"
                width: parent.width
                model: root.ingestActions
                currentIndex: Math.max(0, root.ingestActions.findIndex(a => a.id === Library.ingestAction))
                Accessible.name: qsTr("What Ingest does")
                onActivated: index => Library.ingestAction = root.ingestActions[index].id
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: Library.ingestAction === "proxies"
                      ? qsTr("The clips are imported where they are and proxies of them are made (the proxy settings' format).")
                      : Library.ingestAction === "transcode"
                        ? qsTr("The clips are imported at once and transcoded into the folder through the export queue; each switches to its new file when it is made.")
                        : qsTr("The clips' files are copied into the folder, then the copies are imported.")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Row {
                visible: Library.ingestAction !== "proxies"
                width: parent.width
                spacing: Theme.space(2)
                Text {
                    objectName: "mediaBrowserIngestFolder"
                    width: parent.width - chooseFolder.width - parent.spacing
                    anchors.verticalCenter: parent.verticalCenter
                    text: Library.ingestFolder !== "" ? Library.ingestFolder : qsTr("No folder chosen")
                    color: Library.ingestFolder !== "" ? Theme.textPrimary : Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideMiddle
                }
                PillButton {
                    id: chooseFolder
                    objectName: "mediaBrowserIngestChooseFolder"
                    compact: true
                    text: qsTr("Folder…")
                    tooltip: qsTr("Where the clips are copied or transcoded to")
                    onClicked: ingestDialog.open()
                }
            }
            Row {
                visible: Library.ingestAction === "transcode"
                width: parent.width
                spacing: Theme.space(2)
                Text {
                    width: parent.width - choosePreset.width - parent.spacing
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Preset")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                PillButton {
                    id: choosePreset
                    objectName: "mediaBrowserIngestPreset"
                    compact: true
                    text: root.presetName(Library.ingestPreset)
                    tooltip: qsTr("The export preset the clips are transcoded with")
                    onClicked: presetMenu.popup()
                }
            }
        }
    }

    Text {
        id: location
        objectName: "mediaBrowserLocation"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        text: root.folder
        color: Theme.textTertiary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
        elide: Text.ElideMiddle
    }

    ListView {
        id: list
        objectName: "mediaBrowserList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: location.bottom
        anchors.topMargin: Theme.space(2)
        anchors.bottom: metadataView.visible ? metadataView.top : parent.bottom
        anchors.bottomMargin: metadataView.visible ? Theme.space(2) : 0
        clip: true
        model: root.entries
        spacing: Theme.space(1)
        keyNavigationEnabled: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Media Browser: %n item(s)", "", count)
        Keys.onReturnPressed: if (currentIndex >= 0) root.activate(root.entries[currentIndex])
        Keys.onSpacePressed: if (currentIndex >= 0 && !root.entries[currentIndex].folder) root.toggle(root.entries[currentIndex].path, true)

        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            readonly property bool isFolder: modelData.folder === true
            readonly property bool chosen: root.selected[modelData.path] === true

            objectName: "mediaBrowserRow_" + index
            width: ListView.view.width
            height: Theme.space(isFolder ? 8 : 14)
            radius: Theme.radiusCard
            color: chosen ? Theme.accentSoft : ListView.isCurrentItem ? Theme.hover : "transparent"
            Accessible.role: Accessible.ListItem
            Accessible.name: modelData.name + (isFolder ? qsTr(", folder") : modelData.spanned ? qsTr(", spanned clip of %1 files").arg(modelData.files) : "")

            // The thumbnail; hovering it scrubs through the clip (the tile of its strip under the pointer).
            Item {
                id: thumbBox
                objectName: "mediaBrowserThumb_" + row.index
                anchors.left: parent.left
                anchors.leftMargin: Theme.space(1)
                anchors.verticalCenter: parent.verticalCenter
                width: row.isFolder ? 0 : Theme.space(20)
                height: Theme.space(12)
                visible: !row.isFolder
                clip: true
                property real fraction: 0
                readonly property bool scrubbing: Library.hoverScrub && thumbHover.hovered && strip.status === Image.Ready
                readonly property int tile: Math.min(Library.browserStripFrames - 1, Math.floor(thumbBox.fraction * Library.browserStripFrames))

                Image {
                    anchors.fill: parent
                    visible: !thumbBox.scrubbing
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                    sourceSize.height: height
                    source: row.isFolder ? "" : (row.modelData.thumbnail ?? "")
                }
                Item {
                    // One tile of the strip, centred like the thumbnail.
                    anchors.centerIn: parent
                    width: Math.min(parent.width, strip.implicitWidth / Library.browserStripFrames)
                    height: Math.min(parent.height, strip.implicitHeight)
                    visible: thumbBox.scrubbing
                    clip: true
                    Image {
                        id: strip
                        objectName: "mediaBrowserStrip"
                        x: -thumbBox.tile * (implicitWidth / Library.browserStripFrames)
                        asynchronous: true
                        cache: true
                        sourceSize.height: thumbBox.height
                        source: !row.isFolder && Library.hoverScrub && thumbHover.hovered ? (row.modelData.strip ?? "") : ""
                    }
                }
                Rectangle {
                    // The scrub line.
                    visible: thumbBox.scrubbing
                    width: 1
                    height: parent.height
                    x: Math.round(thumbBox.fraction * (parent.width - width))
                    color: Theme.accentStroke
                }
                HoverHandler {
                    id: thumbHover
                    cursorShape: Library.hoverScrub ? Qt.SizeHorCursor : Qt.ArrowCursor
                    onPointChanged: if (hovered) thumbBox.fraction = Math.max(0, Math.min(1, point.position.x / thumbBox.width))
                }
            }
            Icon {
                anchors.left: parent.left
                anchors.leftMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                visible: row.isFolder
                name: "folder"
            }
            Column {
                anchors.left: thumbBox.right
                anchors.leftMargin: Theme.space(row.isFolder ? 8 : 2)
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space(1)
                Text {
                    width: parent.width
                    text: row.modelData.name
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: !row.isFolder
                    text: [row.modelData.card ?? "",
                           row.modelData.spanned ? qsTr("Spanned: %1 files").arg(row.modelData.files) : "",
                           (row.modelData.audioChannels ?? 0) > 0 ? qsTr("%1 audio channels").arg(row.modelData.audioChannels) : "",
                           Math.round((row.modelData.bytes ?? 0) / 1048576) + " MB"].filter(s => s !== "").join(" · ")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                }
            }
            TapHandler {
                id: tap
                onTapped: {
                    list.currentIndex = row.index
                    if (!row.isFolder)
                        root.toggle(row.modelData.path, (tap.point.modifiers & Qt.ControlModifier) !== 0)
                }
                onDoubleTapped: root.activate(row.modelData)
            }
        }

        EmptyState {
            anchors.centerIn: parent
            visible: list.count === 0
            illustration: "search"
            text: Library.message !== "" ? Library.message : qsTr("No media here. Open a folder or a camera card from the places menu.")
        }
    }

    // The metadata view: what probing the current clip reads (nothing is imported).
    Rectangle {
        id: metadataView
        objectName: "mediaBrowserMetadata"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Math.min(metadataColumn.implicitHeight + Theme.space(4), parent.height / 2)
        visible: root.showMetadata && root.currentIsClip
        radius: Theme.radiusCard
        color: Theme.raised
        border.color: Theme.border
        clip: true
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("Metadata of %1").arg(root.currentEntry ? root.currentEntry.name : "")

        Column {
            id: metadataColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.space(2)
            spacing: Theme.space(1)
            Repeater {
                model: root.metadataLines(root.metadata)
                delegate: Row {
                    id: line
                    required property var modelData
                    width: metadataColumn.width
                    spacing: Theme.space(2)
                    Text {
                        width: Theme.space(24)
                        text: line.modelData.label
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        elide: Text.ElideRight
                    }
                    Text {
                        width: line.width - Theme.space(26)
                        text: line.modelData.value
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        elide: Text.ElideRight
                    }
                }
            }
            Text {
                visible: root.metadata.name === undefined
                width: parent.width
                wrapMode: Text.WordWrap
                text: Library.message !== "" ? Library.message : qsTr("Nothing to read in this clip.")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }
}
