pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Library (M5k: Premiere's Project panel). Bridge: `Library` (LibraryBridge), rows: Library.items (LibraryModel).
//
//   * Bins: the bin bar (bins opened as tabs, the breadcrumb), New Bin (Ctrl/⌘+B) and New Bin from Selection, drag
//     items onto a bin card, a list row, a crumb or a tab to move them. Double-click a bin to open it here; its menu
//     opens it in a tab.
//   * Grid (cards: thumbnail or poster frame, label strip, hover scrub over the filmstrip, subclip range, proxy and AI
//     tags) or list (LibraryListView: metadata columns, sorting, inline editing) - Premiere's icon and list views
//     (project.iconView / project.listView / project.toggleView).
//   * Find (Ctrl/⌘+F) is the search bar: the text and the filters (funnel) list matches from every bin; with Smart on,
//     smart results (AI-8) replace the grid and are narrowed by the same filters.
//   * Make Subclip (Ctrl/⌘+U) from the Source Monitor's marks; Edit Subclip; the poster frame from the hover-scrub
//     position; Reveal in File Explorer; unused media (show, remove) and duplicates.
//   * Media / Audio / Text tabs: everything, bins and audio, Essential Graphics (titles, M5d).
//
// I.1 behaviour kept: files dropped here are imported, "+" imports, media cards drag onto the timeline
// (application/x-wabcut-media), a double-click shows the clip in the Source Monitor, a sequence opens in its tab.
// I.3: the proxy chip on each card and the proxy queue pill under the grid.
Panel {
    id: root

    KeymapScope.scope: "project"

    required property MockProject project
    /// The item being renamed in the grid ("" = none).
    property string renaming: ""

    signal importRequested()
    signal filesDropped(var urls)

    title: qsTr("Library")

    // Focus target for the rail's "Library" shortcut: the list, or the selected (else the first) card.
    function focusContent() {
        if (Library.view === "list") {
            list.forceActiveFocus(Qt.TabFocusReason)
            return
        }
        const keys = Library.selection
        grid.focusRow(keys.length > 0 ? Math.max(0, Library.items.rowOf(keys[0])) : 0)
    }
    function focusFind() {
        searchBar.focusField()
    }
    /// Make Subclip (Ctrl/⌘+U): ask for its name.
    function makeSubclip() {
        if (Library.canMakeSubclip)
            subclipPopover.openMake(searchRow)
    }
    function rename(key) {
        if (Library.view === "list")
            list.startEdit(key, "name")
        else
            root.renaming = key
    }
    function activate(key, kind) {
        if (kind === "bin") {
            Library.openBin(key)
        } else if (kind === "sequence") {
            SequenceTabs.open(key)
        } else {
            MediaReadiness.setLibrarySelection([key])
            if (!(Library.details(key).offline ?? true))
                SourceMonitor.load(key)
        }
    }
    function showMenu(key, kind, target, point, fraction) {
        itemMenu.openFor(target, key, kind, point, fraction)
    }

    Connections {
        target: Library
        function onRenameRequested(key) {
            root.rename(key)
        }
        function onFindRequested() {
            root.focusFind()
        }
        function onMakeSubclipRequested() {
            root.makeSubclip()
        }
    }

    trailing: [
        IconButton {
            objectName: "libraryGridButton"
            compact: true
            checkable: true
            checked: Library.view === "grid"
            iconName: "layout-grid"
            tooltip: qsTr("Icon view")
            command: "project.iconView"
            onClicked: Library.view = "grid"
        },
        IconButton {
            objectName: "libraryListButton"
            compact: true
            checkable: true
            checked: Library.view === "list"
            iconName: "list"
            tooltip: qsTr("List view")
            command: "project.listView"
            onClicked: Library.view = "list"
        },
        IconButton {
            objectName: "libraryNewBinButton"
            compact: true
            iconName: "folder-plus"
            tooltip: qsTr("New bin")
            command: "project.newBin"
            onClicked: Library.newBin()
        },
        IconButton {
            id: moreButton
            objectName: "libraryMoreButton"
            compact: true
            iconName: "ellipsis"
            tooltip: qsTr("Library tools")
            onClicked: toolsMenu.popup(moreButton, 0, moreButton.height)
        },
        IconButton {
            compact: true
            iconName: "plus"
            tooltip: qsTr("Import media")
            command: "file.import"
            onClicked: root.importRequested()
        }
    ]

    // Find: the search bar (Smart toggle, AI-8) and the filters.
    Item {
        id: searchRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: searchBar.implicitHeight

        SmartSearchBar {
            id: searchBar
            objectName: "librarySearchBar"
            anchors.left: parent.left
            anchors.right: filterButton.left
            anchors.rightMargin: Theme.space(1)
            anchors.verticalCenter: parent.verticalCenter
            onQueryChanged: Library.findText = searchBar.smart ? "" : searchBar.query
            onSmartChanged: Library.findText = searchBar.smart ? "" : searchBar.query
        }
        IconButton {
            id: filterButton
            objectName: "libraryFilterButton"
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            checkable: true
            checked: Library.filterCount > 0
            iconName: "funnel"
            tooltip: Library.filterCount > 0 ? qsTr("Find filters: %n active", "", Library.filterCount) : qsTr("Find filters")
            onClicked: {
                checked = Qt.binding(() => Library.filterCount > 0)
                filters.open()
            }
            FindFiltersPopover {
                id: filters
                x: filterButton.width - width
                y: filterButton.height + Theme.space(1)
            }
        }
    }

    PillTabs {
        id: tabs
        objectName: "libraryTabs"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: searchRow.bottom
        anchors.topMargin: Theme.space(2)
        compact: true
        model: [
            { label: qsTr("Media"), icon: "film" },
            { label: qsTr("Audio"), icon: "music" },
            { label: qsTr("Text"), icon: "type" }
        ]
        onCurrentIndexChanged: Library.scope = currentIndex === 1 ? "audio" : "all"
        Accessible.name: qsTr("Library category")
    }

    BinBar {
        id: binBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabs.bottom
        anchors.topMargin: Theme.space(1)
        visible: tabs.currentIndex !== 2 && !smartResults.visible
        height: visible ? implicitHeight : 0
    }

    // Text: Essential Graphics (New Title and the title style, M5d).
    TitleStylePanel {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabs.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: Theme.space(3)
        visible: tabs.currentIndex === 2
    }

    Item {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: binBar.bottom
        anchors.bottom: proxyPill.visible ? proxyPill.top : parent.bottom
        anchors.topMargin: Theme.space(1)
        anchors.bottomMargin: proxyPill.visible ? Theme.space(2) : 0
        visible: tabs.currentIndex !== 2 && !smartResults.visible

        GridView {
            id: grid
            objectName: "libraryGrid"
            readonly property int columns: Math.max(1, Math.floor(width / Theme.mediaCardMinWidth))
            anchors.fill: parent
            anchors.leftMargin: -Theme.space(1)
            anchors.rightMargin: -Theme.space(1)
            visible: Library.view === "grid"
            clip: true
            model: Library.items
            cellWidth: width / columns
            cellHeight: Math.round((cellWidth - Theme.space(2)) * 9 / 16) + Theme.space(13)
            boundsBehavior: Flickable.StopAtBounds
            // The cards are the tab stops (with focus rings); their arrow keys reach the grid's handler.
            keyNavigationEnabled: false
            ScrollBar.vertical: ThinScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Library: %n item(s)", "", count)

            /// Keyboard focus on the card of `row` (the grid itself when there is none).
            function focusRow(row) {
                if (count === 0) {
                    grid.forceActiveFocus(Qt.TabFocusReason)
                    return
                }
                positionViewAtIndex(row, GridView.Contain)
                const cell = itemAtIndex(row)
                const card = cell && cell.children.length > 0 ? cell.children[0] : null // the LibraryCard
                if (card)
                    card.forceActiveFocus(Qt.TabFocusReason)
                else
                    grid.forceActiveFocus(Qt.TabFocusReason)
            }
            function step(delta, extend) {
                const keys = Library.selection
                const from = keys.length > 0 ? Library.items.rowOf(keys[keys.length - 1]) : -1
                const to = Math.max(0, Math.min(count - 1, from + delta))
                if (count === 0)
                    return
                Library.select(Library.items.keyOf(to), extend ? Library.Range : Library.Replace)
                focusRow(to) // the focus ring follows the selection
            }
            Keys.onPressed: event => {
                const extend = (event.modifiers & Qt.ShiftModifier) !== 0
                if (event.key === Qt.Key_Right) grid.step(1, extend)
                else if (event.key === Qt.Key_Left) grid.step(-1, extend)
                else if (event.key === Qt.Key_Down) grid.step(grid.columns, extend)
                else if (event.key === Qt.Key_Up) grid.step(-grid.columns, extend)
                else if (event.key === Qt.Key_F2 && Library.selection.length === 1) root.rename(Library.selection[0])
                else return
                event.accepted = true
            }
            Keys.onShortcutOverride: event => event.accepted = (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) === 0
                                                                && [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_F2].includes(event.key)

            delegate: Item {
                id: cell
                required property int index
                required property string key
                required property string kind
                required property string name
                required property int label
                required property string thumbnail
                required property string durationText
                required property string meta
                required property bool offline
                required property string proxyState
                required property real proxyProgress
                required property bool generated
                required property var subclip
                required property bool selected
                required property bool filmstrip
                required property int itemCount
                required property string binPath
                required property string derivedFrom
                width: grid.cellWidth
                height: grid.cellHeight

                LibraryCard { // the first child: grid.focusRow() focuses it
                    id: card
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    index: cell.index
                    key: cell.key
                    kind: cell.kind
                    name: cell.name
                    label: cell.label
                    thumbnail: cell.thumbnail
                    durationText: cell.durationText
                    meta: cell.meta
                    offline: cell.offline
                    proxyState: cell.proxyState
                    proxyProgress: cell.proxyProgress
                    generated: cell.generated
                    subclip: cell.subclip
                    selected: cell.selected
                    filmstrip: cell.filmstrip
                    itemCount: cell.itemCount
                    binPath: cell.binPath
                    derivedFrom: cell.derivedFrom
                    onActivated: root.activate(cell.key, cell.kind)
                    onMenuRequested: point => root.showMenu(cell.key, cell.kind, card, point, card.scrubFraction)
                }
                // Inline rename under the thumbnail.
                Loader {
                    active: root.renaming === cell.key
                    x: Theme.space(1)
                    width: parent.width - Theme.space(2)
                    y: Math.round((grid.cellWidth - Theme.space(2)) * 9 / 16) + Theme.space(2)
                    sourceComponent: FieldInput {
                        objectName: "libraryRenameField"
                        text: cell.name
                        Accessible.name: qsTr("Name")
                        Component.onCompleted: {
                            forceActiveFocus()
                            selectAll()
                        }
                        onAccepted: {
                            Library.rename(cell.key, text)
                            root.renaming = ""
                            grid.forceActiveFocus()
                        }
                        onActiveFocusChanged: if (!activeFocus && root.renaming === cell.key) root.renaming = ""
                        Keys.onEscapePressed: event => {
                            root.renaming = ""
                            grid.forceActiveFocus()
                            event.accepted = true
                        }
                        Keys.onShortcutOverride: event => event.accepted = true
                    }
                }
            }
            // A click on empty space clears the selection.
            TapHandler {
                enabled: grid.count > 0
                onTapped: eventPoint => {
                    if (grid.indexAt(eventPoint.position.x + grid.contentX, eventPoint.position.y + grid.contentY) < 0)
                        Library.clearSelection()
                }
            }
        }

        LibraryListView {
            id: list
            anchors.fill: parent
            visible: Library.view === "list"
            onActivated: (key, kind) => root.activate(key, kind)
            onMenuRequested: (key, target, position) => root.showMenu(key, Library.details(key).kind ?? "", target, position, -1)
        }
    }

    // AI-8: Smart search results in the grid's place, narrowed by the Find filters.
    SearchResultsPanel {
        id: smartResults
        objectName: "smartSearchPanel"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabs.bottom
        anchors.bottom: proxyPill.visible ? proxyPill.top : parent.bottom
        anchors.topMargin: Theme.space(3)
        visible: searchBar.smartActive && tabs.currentIndex !== 2
        results: Library.filterCount > 0 ? SmartSearch.results.filter(hit => Library.passesFilters(hit.mediaId))
                                         : SmartSearch.results
    }

    // "Proxies 3/12": proxies the project has / wants, under the grid; a click opens Media & cache settings.
    StatusPill {
        id: proxyPill
        objectName: "proxyStatusPill"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        visible: MediaReadiness.statusText !== "" && tabs.currentIndex !== 2
        label: MediaReadiness.statusText
        detail: MediaReadiness.statusDetail
        dotColor: MediaReadiness.working ? (MediaReadiness.pausedForForeground ? Theme.warning : Theme.accentStroke)
                                         : (MediaReadiness.statusDetail !== "" ? Theme.warning : Theme.success)
        tooltip: qsTr("Proxies: click for proxy and cache settings")
        Accessible.role: Accessible.Button

        TapHandler {
            onTapped: MediaReadiness.openSettings()
        }
        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
        BeamProgress {
            objectName: "proxyProgress"
            visible: MediaReadiness.working
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: parent.radius / 2
            anchors.rightMargin: parent.radius / 2
            height: Theme.hairline * 2
            indeterminate: true
            active: !MediaReadiness.pausedForForeground
            label: MediaReadiness.statusText
            loader: "build"
        }
    }

    LibraryItemMenu {
        id: itemMenu
        onRenameRequested: key => root.rename(key)
        onMetadataRequested: key => metadataPopover.openFor(key, root)
        onSubclipRequested: key => subclipPopover.openMakeOf(key, searchRow)
        onEditSubclipRequested: key => subclipPopover.openEdit(key, searchRow)
        onOpenRequested: (key, kind) => root.activate(key, kind)
    }

    // The tools menu: hover scrub, unused media, duplicates, Make Subclip.
    LibraryToolsMenu {
        id: toolsMenu
        onRemoveUnusedRequested: removeUnusedPopover.open()
        onMakeSubclipRequested: root.makeSubclip()
    }

    Popover {
        id: removeUnusedPopover
        objectName: "libraryRemoveUnusedPopover"
        parent: searchRow
        title: qsTr("Remove unused media")
        Column {
            width: Theme.space(72)
            spacing: Theme.space(2)
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Remove %n item(s) that no sequence uses from the project? The files stay on disk, and Undo brings them back.", "", Library.unusedCount)
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Row {
                spacing: Theme.space(2)
                PillButton {
                    objectName: "libraryRemoveUnusedConfirm"
                    variant: "primary"
                    text: qsTr("Remove")
                    onClicked: {
                        Library.removeUnused()
                        removeUnusedPopover.close()
                    }
                }
                PillButton {
                    variant: "ghost"
                    text: qsTr("Cancel")
                    onClicked: removeUnusedPopover.close()
                }
            }
        }
    }

    // Comment and description of a media item (the list edits them inline).
    Popover {
        id: metadataPopover
        objectName: "libraryMetadataPopover"
        property string key: ""
        function openFor(itemKey, target) {
            key = itemKey
            const d = Library.details(itemKey)
            commentField.text = d.comment ?? ""
            descriptionField.text = d.description ?? ""
            parent = target
            open()
            commentField.forceActiveFocus()
        }
        title: qsTr("Comment and description")
        Column {
            width: Theme.space(80)
            spacing: Theme.space(2)
            FieldInput {
                id: commentField
                objectName: "libraryCommentField"
                width: parent.width
                Accessible.name: qsTr("Comment")
                placeholderText: qsTr("Comment")
                Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            }
            FieldInput {
                id: descriptionField
                objectName: "libraryDescriptionField"
                width: parent.width
                Accessible.name: qsTr("Description")
                placeholderText: qsTr("Description")
                Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            }
            PillButton {
                objectName: "libraryMetadataSave"
                variant: "primary"
                text: qsTr("Save")
                onClicked: {
                    const d = Library.details(metadataPopover.key)
                    if (commentField.text !== d.comment)
                        Library.setComment(metadataPopover.key, commentField.text)
                    if (descriptionField.text !== d.description)
                        Library.setDescription(metadataPopover.key, descriptionField.text)
                    metadataPopover.close()
                }
            }
        }
    }

    SubclipPopover {
        id: subclipPopover
    }

    // Empty project: how to add media (design language v2 empty state).
    EmptyState {
        objectName: "libraryEmptyState"
        anchors.centerIn: content
        width: content.width - Theme.space(8)
        visible: content.visible && Library.items.count === 0 && !Library.filtering && Library.currentBin === ""
                 && tabs.currentIndex === 0
        illustration: "library"
        text: qsTr("Drop media files here, or import them with %1.").arg(KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText("file.import") : "")
        actionText: qsTr("Import media")
        actionIcon: "import"
        onActionTriggered: root.importRequested()
    }
    // An empty bin or tab, or nothing matching Find.
    EmptyState {
        id: noResults
        objectName: "libraryNoResults"
        anchors.centerIn: content
        width: content.width - Theme.space(8)
        visible: content.visible && Library.items.count === 0 && (Library.filtering || Library.currentBin !== "" || tabs.currentIndex !== 0)
        illustration: Library.filtering ? "search" : "library"
        //: %1 = the search text
        text: Library.filtering ? (searchBar.query.trim() !== "" ? qsTr("No item matches “%1”.").arg(searchBar.query.trim())
                                                                 : qsTr("No item matches these filters."))
              : Library.currentBin !== "" ? qsTr("This bin is empty. Drag items onto it to move them here.")
              : qsTr("No audio in this project yet. Import music, voice-over or sound effects.")
        actionText: Library.filtering ? qsTr("Clear search") : qsTr("Import media")
        actionIcon: Library.filtering ? "x" : "import"
        onActionTriggered: {
            if (Library.filtering) {
                searchBar.query = ""
                Library.clearFilters()
            } else {
                root.importRequested()
            }
        }
    }

    // Files dragged in from the desktop: media and timelines are imported.
    DropArea {
        id: fileDrop
        objectName: "libraryDropArea"
        anchors.fill: parent
        keys: ["text/uri-list"]
        onEntered: drag => drag.accepted = drag.hasUrls
        onDropped: drop => {
            if (!drop.hasUrls)
                return
            root.filesDropped(drop.urls)
            drop.accept(Qt.CopyAction)
        }
    }
    Rectangle {
        anchors.fill: parent
        visible: fileDrop.containsDrag
        radius: Theme.radiusPanel
        color: "transparent"
        border.width: Theme.selectionWidth
        border.color: Theme.accentStroke
    }
}
