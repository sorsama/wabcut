pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

// ● PRODUCTION (0.2, P.3: Premiere's Productions panel) - singleton `Production` (ProductionBridge).
//   * Open projects (ProjectFile.openProjects): every project open in this session, the one in the panels marked;
//     click one to switch to it at once (each keeps its undo history and unsaved changes), close the others.
//   * A production is a folder of projects: its folders and projects, who has each one open (a lock), New Project,
//     New Folder, Add Project (a copy that keeps playing the same media), Production Settings (scratch disks for its
//     previews and media cache). Double-click a project to open it next to the others; opening a project of a
//     production opens its production here.
//   * Another project browsed read only ("Browse read-only", or Reveal in Source Project): its bins, sequences and
//     media. Import the selected ones (or drag them onto the Library): copies come in, in one undo step, with what they
//     play, reusing media this project already has; media files are never copied. Copies remember their source, and
//     "Relinks" offers the copies the files their source project was relinked to.
Panel {
    id: root

    objectName: "productionPanel"
    title: Production.open ? qsTr("Production: %1").arg(Production.name) : qsTr("Production")

    signal openProjectRequested(url file)
    /// Close open project `index` (Main.qml asks about unsaved changes first).
    signal closeProjectRequested(int index)

    /// Keys of the other project's items picked for import.
    property var picked: []

    function focusContent() {
        if (Production.browsing.toString() !== "")
            browseList.forceActiveFocus(Qt.TabFocusReason)
        else
            entryList.forceActiveFocus(Qt.TabFocusReason)
    }
    function togglePick(key, extend) {
        const next = extend ? root.picked.slice() : []
        const at = next.indexOf(key)
        if (at >= 0)
            next.splice(at, 1)
        else
            next.push(key)
        root.picked = next
    }

    /// The relinks found by "Relinks" (Production.relinkOffers()).
    property var offers: []

    Connections {
        target: Production
        function onBrowsingChanged() {
            root.picked = Production.revealedKey !== "" ? [Production.revealedKey] : []
            if (Production.revealedKey !== "") {
                for (let i = 0; i < Production.browseItems.length; ++i) {
                    if (Production.browseItems[i].key === Production.revealedKey) {
                        browseList.positionViewAtIndex(i, ListView.Center)
                        break
                    }
                }
            }
        }
    }

    trailing: [
        IconButton {
            objectName: "productionNewButton"
            compact: true
            iconName: "folder-plus"
            tooltip: qsTr("New production…")
            onClicked: newProductionDialog.open()
        },
        IconButton {
            objectName: "productionOpenButton"
            compact: true
            iconName: "folder-open"
            tooltip: qsTr("Open production…")
            onClicked: openProductionDialog.open()
        },
        IconButton {
            objectName: "productionBrowseButton"
            compact: true
            iconName: "layers"
            tooltip: qsTr("Browse another project read-only…")
            onClicked: browseDialog.open()
        }
    ]

    FolderDialog {
        id: newProductionDialog
        title: qsTr("Folder for the new production")
        currentFolder: ProjectFile.folder
        onAccepted: Production.makeProduction(selectedFolder)
    }
    FolderDialog {
        id: openProductionDialog
        title: qsTr("Open production")
        currentFolder: ProjectFile.folder
        onAccepted: Production.openProduction(selectedFolder)
    }
    FileDialog {
        id: browseDialog
        title: qsTr("Browse a project read-only")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Wabcut projects (*.wabcut)")]
        currentFolder: Production.open ? Production.folderUrl : ProjectFile.folder
        onAccepted: Production.browse(selectedFile)
    }
    FileDialog {
        id: addProjectDialog
        title: qsTr("Add project to production")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Wabcut projects (*.wabcut)")]
        currentFolder: ProjectFile.folder
        onAccepted: Production.addProject(selectedFile, "")
    }

    // ---- Open projects (several at once, one in the panels) ----
    Column {
        id: openArea
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        visible: ProjectFile.openProjects.length > 1
        height: visible ? implicitHeight : 0
        spacing: Theme.space(1)

        SectionHeader {
            text: qsTr("Open projects")
        }
        Repeater {
            model: ProjectFile.openProjects
            delegate: Rectangle {
                id: openRow
                required property var modelData
                required property int index
                objectName: "openProject_" + index
                width: openArea.width
                height: Theme.rowHeight
                radius: Theme.radiusClip
                color: openRow.modelData.active ? Theme.accentWash : openHover.hovered ? Theme.hover : "transparent"
                Accessible.role: Accessible.ListItem
                Accessible.name: openRow.modelData.dirty ? qsTr("%1, unsaved changes").arg(openRow.modelData.name) : openRow.modelData.name
                Accessible.selected: openRow.modelData.active

                Icon {
                    id: openIcon
                    x: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    size: Theme.iconXs
                    name: openRow.modelData.readOnly ? "lock" : "film"
                    color: openRow.modelData.active ? Theme.accentStroke : Theme.textTertiary
                }
                Text {
                    anchors.left: openIcon.right
                    anchors.leftMargin: Theme.space(2)
                    anchors.right: closeOpen.left
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    text: (openRow.modelData.dirty ? "● " : "") + openRow.modelData.name
                          + (openRow.modelData.active ? qsTr(" (editing)") : "")
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: openRow.modelData.active ? Font.DemiBold : Font.Normal
                }
                IconButton {
                    id: closeOpen
                    objectName: "openProjectClose_" + openRow.index
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "x"
                    tooltip: qsTr("Close %1").arg(openRow.modelData.name)
                    onClicked: root.closeProjectRequested(openRow.index)
                }
                HoverHandler {
                    id: openHover
                }
                ToolTip.visible: openHover.hovered && openRow.modelData.path !== ""
                ToolTip.text: openRow.modelData.path
                TapHandler {
                    onTapped: {
                        if (!openRow.modelData.active)
                            ProjectFile.switchTo(openRow.index)
                    }
                }
            }
        }
    }

    EmptyState {
        objectName: "productionEmpty"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: openArea.bottom
        anchors.topMargin: Theme.space(6)
        visible: !Production.open && Production.browsing.toString() === ""
        illustration: "library"
        text: qsTr("A production keeps a film's projects together in one folder: open several, share media and sequences between them, and see who is working in which project.")
        actionText: qsTr("New production")
        actionIcon: "folder-plus"
        onActionTriggered: newProductionDialog.open()
    }

    Column {
        id: productionArea
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: openArea.bottom
        anchors.topMargin: openArea.visible ? Theme.space(3) : 0
        visible: Production.open
        spacing: Theme.space(2)

        Text {
            width: parent.width
            text: Production.folder
            color: Theme.textTertiary
            elide: Text.ElideMiddle
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
        Row {
            spacing: Theme.space(1)
            PillButton {
                objectName: "productionNewProject"
                compact: true
                iconName: "plus"
                text: qsTr("New project")
                onClicked: {
                    namePopover.mode = "project"
                    namePopover.open()
                }
            }
            PillButton {
                objectName: "productionNewFolder"
                compact: true
                iconName: "folder-plus"
                text: qsTr("New folder")
                onClicked: {
                    namePopover.mode = "folder"
                    namePopover.open()
                }
            }
            PillButton {
                objectName: "productionAddProject"
                compact: true
                iconName: "import"
                text: qsTr("Add project…")
                onClicked: addProjectDialog.open()
            }
            IconButton {
                id: settingsButton
                objectName: "productionSettingsButton"
                compact: true
                iconName: "settings"
                tooltip: qsTr("Production Settings…")
                onClicked: settingsPopover.open()
            }
            IconButton {
                objectName: "productionRelinksButton"
                compact: true
                iconName: "link"
                tooltip: qsTr("Relinks from source projects")
                onClicked: {
                    root.offers = Production.relinkOffers()
                    relinkPopover.open()
                }
            }
            IconButton {
                compact: true
                iconName: "rotate-cw"
                tooltip: qsTr("Refresh")
                onClicked: Production.refresh()
            }
        }

        ListView {
            id: entryList
            objectName: "productionEntries"
            width: parent.width
            height: Math.min(contentHeight, Theme.space(70))
            clip: true
            model: Production.entries
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationEnabled: true
            focus: true
            ScrollBar.vertical: ThinScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Projects of the production")

            delegate: Rectangle {
                id: entry
                required property var modelData
                required property int index
                objectName: "productionEntry_" + index
                width: entryList.width
                height: Theme.rowHeight
                radius: Theme.radiusClip
                color: entry.modelData.current ? Theme.accentWash : entryHover.hovered ? Theme.hover : "transparent"
                Accessible.role: Accessible.ListItem
                Accessible.name: entry.modelData.locked ? qsTr("%1, open by %2").arg(entry.modelData.name).arg(entry.modelData.lockOwner)
                                                        : entry.modelData.name

                Icon {
                    id: entryIcon
                    x: Theme.space(2) + entry.modelData.depth * Theme.space(4)
                    anchors.verticalCenter: parent.verticalCenter
                    size: Theme.iconXs
                    name: entry.modelData.folder ? "folder" : "film"
                    color: entry.modelData.folder ? Theme.accentStroke : Theme.textTertiary
                }
                Text {
                    anchors.left: entryIcon.right
                    anchors.leftMargin: Theme.space(2)
                    anchors.right: lockIcon.left
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.modelData.current ? qsTr("%1 (editing)").arg(entry.modelData.name)
                          : entry.modelData.open ? qsTr("%1 (open)").arg(entry.modelData.name)
                          : entry.modelData.browsing ? qsTr("%1 (browsing)").arg(entry.modelData.name)
                          : entry.modelData.name
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: entry.modelData.current ? Font.DemiBold : Font.Normal
                }
                Icon {
                    id: lockIcon
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    visible: entry.modelData.locked
                    size: Theme.iconXs
                    name: "lock"
                    color: Theme.warning
                }
                HoverHandler {
                    id: entryHover
                }
                ToolTip.visible: entryHover.hovered && entry.modelData.locked
                ToolTip.text: qsTr("Open by %1: read-only for you").arg(entry.modelData.lockOwner)
                TapHandler {
                    onDoubleTapped: {
                        if (!entry.modelData.folder && !entry.modelData.current)
                            root.openProjectRequested(entry.modelData.url)
                    }
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    enabled: !entry.modelData.folder
                    onTapped: eventPoint => projectMenu.openFor(entry, entry.modelData, eventPoint.position)
                }
            }
        }
    }

    ProductionProjectMenu {
        id: projectMenu
        onOpenRequested: url => root.openProjectRequested(url)
    }

    // The other project, open next to this one.
    Column {
        id: browseArea
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: productionArea.visible ? productionArea.bottom : parent.top
        anchors.topMargin: productionArea.visible ? Theme.space(3) : 0
        anchors.bottom: parent.bottom
        visible: Production.browsing.toString() !== ""
        spacing: Theme.space(2)

        Item {
            width: parent.width
            height: browseTitle.implicitHeight
            SectionHeader {
                id: browseTitle
                anchors.left: parent.left
                text: qsTr("Browsing: %1").arg(Production.browsingName)
            }
            IconButton {
                objectName: "productionStopBrowsing"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: "x"
                tooltip: qsTr("Stop browsing")
                onClicked: Production.stopBrowsing()
            }
        }
        Row {
            spacing: Theme.space(1)
            PillButton {
                objectName: "productionImportPicked"
                compact: true
                variant: "primary"
                iconName: "import"
                enabled: root.picked.length > 0
                text: qsTr("Import %n item(s)", "", root.picked.length)
                onClicked: Production.importItems(root.picked)
            }
            PillButton {
                objectName: "productionEditOther"
                compact: true
                iconName: "film"
                text: qsTr("Open and edit")
                onClicked: root.openProjectRequested(Production.browsing)
            }
        }
        ListView {
            id: browseList
            objectName: "productionBrowseItems"
            width: parent.width
            height: parent.height - y
            clip: true
            model: Production.browseItems
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Items of %1").arg(Production.browsingName)

            delegate: Rectangle {
                id: browseRow
                required property var modelData
                required property int index
                readonly property bool isPicked: root.picked.indexOf(modelData.key) >= 0
                objectName: "productionBrowseItem_" + modelData.key
                width: browseList.width
                height: Theme.rowHeight
                radius: Theme.radiusClip
                color: browseRow.isPicked ? Theme.accentWash : itemHover.hovered ? Theme.hover : "transparent"
                border.width: browseRow.modelData.key === Production.revealedKey ? Theme.hairline * 2 : 0
                border.color: Theme.accentStroke
                Accessible.role: Accessible.ListItem
                Accessible.name: browseRow.modelData.name
                Accessible.selected: browseRow.isPicked

                Icon {
                    id: itemIcon
                    x: Theme.space(2) + browseRow.modelData.depth * Theme.space(4)
                    anchors.verticalCenter: parent.verticalCenter
                    size: Theme.iconXs
                    name: browseRow.modelData.kind === "bin" ? "folder" : browseRow.modelData.kind === "sequence" ? "clapperboard"
                          : browseRow.modelData.kind === "audio" ? "music" : browseRow.modelData.kind === "title" ? "type"
                          : browseRow.modelData.kind === "still" ? "image" : "film"
                    color: browseRow.modelData.kind === "bin" ? Theme.accentStroke : Theme.textTertiary
                }
                Text {
                    id: itemName
                    anchors.left: itemIcon.right
                    anchors.leftMargin: Theme.space(2)
                    anchors.right: itemMeta.left
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    text: browseRow.modelData.name
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Text {
                    id: itemMeta
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    text: browseRow.modelData.meta
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
                HoverHandler {
                    id: itemHover
                }
                TapHandler {
                    id: itemTap
                    onSingleTapped: root.togglePick(browseRow.modelData.key,
                                                    (itemTap.point.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0)
                    onDoubleTapped: Production.importItems([browseRow.modelData.key])
                }
                // Drag onto the Library: imports them there.
                Item {
                    id: dragProxy
                    anchors.fill: parent
                    Drag.dragType: Drag.Automatic
                    Drag.supportedActions: Qt.CopyAction
                    DragHandler {
                        target: null
                        onActiveChanged: {
                            if (active && !browseRow.isPicked)
                                root.picked = [browseRow.modelData.key]
                            if (active)
                                dragProxy.Drag.mimeData = { "application/x-wabcut-other-project": root.picked.join(",") }
                            dragProxy.Drag.active = active
                        }
                    }
                }
            }
        }
    }

    // Production Settings > Scratch Disks.
    Popover {
        id: settingsPopover
        objectName: "productionSettingsPopover"
        parent: productionArea
        title: qsTr("Production Settings")
        property string choosing: ""
        Column {
            width: Theme.space(96)
            spacing: Theme.space(2)
            Repeater {
                model: [
                    { key: "previews", label: qsTr("Previews"), shown: Production.settings.previewsText ?? "" },
                    { key: "mediaCache", label: qsTr("Media cache (waveforms, filmstrips)"), shown: Production.settings.mediaCacheText ?? "" }
                ]
                delegate: Column {
                    id: scratch
                    required property var modelData
                    width: parent.width
                    spacing: Theme.space(1)
                    Text {
                        text: scratch.modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: scratch.modelData.shown
                        color: Theme.textTertiary
                        elide: Text.ElideMiddle
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                    Row {
                        spacing: Theme.space(1)
                        PillButton {
                            objectName: "productionChoose_" + scratch.modelData.key
                            compact: true
                            iconName: "folder-open"
                            text: qsTr("Choose…")
                            onClicked: {
                                settingsPopover.choosing = scratch.modelData.key
                                scratchDialog.open()
                            }
                        }
                        PillButton {
                            objectName: "productionDefault_" + scratch.modelData.key
                            compact: true
                            text: qsTr("Default")
                            enabled: (Production.settings[scratch.modelData.key] ?? "") !== ""
                            onClicked: {
                                const change = {}
                                change[scratch.modelData.key] = ""
                                Production.setSettings(change)
                            }
                        }
                    }
                }
            }
            Text {
                width: parent.width
                text: qsTr("Every project of the production uses these folders. Choose a fast drive for them; a folder inside the production moves with it.")
                color: Theme.textTertiary
                wrapMode: Text.Wrap
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
    }
    FolderDialog {
        id: scratchDialog
        title: qsTr("Scratch disk folder")
        currentFolder: Production.folderUrl
        onAccepted: {
            const change = {}
            change[settingsPopover.choosing] = selectedFolder
            Production.setSettings(change)
        }
    }

    // Copies whose source project relinked their media.
    Popover {
        id: relinkPopover
        objectName: "productionRelinkPopover"
        parent: productionArea
        title: qsTr("Relinks from source projects")
        Column {
            width: Theme.space(96)
            spacing: Theme.space(2)
            Text {
                width: parent.width
                visible: root.offers.length === 0
                text: qsTr("The copies in this project play what their source projects play.")
                color: Theme.textTertiary
                wrapMode: Text.Wrap
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Repeater {
                model: root.offers
                delegate: Column {
                    id: offerRow
                    required property var modelData
                    width: parent.width
                    Text {
                        width: parent.width
                        text: qsTr("%1 (from %2)").arg(offerRow.modelData.name).arg(offerRow.modelData.project)
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Text {
                        width: parent.width
                        text: qsTr("→ %1").arg(offerRow.modelData.to)
                        color: Theme.textTertiary
                        elide: Text.ElideMiddle
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                }
            }
            PillButton {
                objectName: "productionApplyRelinks"
                variant: "primary"
                visible: root.offers.length > 0
                text: qsTr("Relink %n item(s)", "", root.offers.length)
                onClicked: {
                    Production.applyRelinkOffers([])
                    root.offers = Production.relinkOffers()
                    relinkPopover.close()
                }
            }
        }
    }

    // New project / new folder name.
    Popover {
        id: namePopover
        objectName: "productionNamePopover"
        property string mode: "project"
        parent: productionArea
        title: mode === "project" ? qsTr("New project") : qsTr("New folder")
        onOpened: {
            nameField.text = ""
            nameField.forceActiveFocus()
        }
        Column {
            width: Theme.space(72)
            spacing: Theme.space(2)
            FieldInput {
                id: nameField
                objectName: "productionNameField"
                width: parent.width
                placeholderText: namePopover.mode === "project" ? qsTr("Project name") : qsTr("Folder name")
                Accessible.name: placeholderText
                onAccepted: createButton.clicked()
                Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            }
            PillButton {
                id: createButton
                objectName: "productionNameCreate"
                variant: "primary"
                text: qsTr("Create")
                enabled: nameField.text.trim() !== ""
                onClicked: {
                    if (namePopover.mode === "project") {
                        const url = Production.newProject("", nameField.text)
                        if (url.toString() !== "")
                            root.openProjectRequested(url)
                    } else {
                        Production.newFolder("", nameField.text)
                    }
                    namePopover.close()
                }
            }
        }
    }
}
