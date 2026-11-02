pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

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
//   * 0.2 (P.3): the Freeform view (LibraryFreeformView), Search Bins (the search bar's "Create new search bin",
//     SearchBinPopover), the clip metadata columns, the Metadata panel and XMP from the item menu, and items dragged
//     in from another project (the Production panel) imported here.
//
// 0.4: a folder dropped here, or chosen with the ··· menu's Import folder…, comes in as a bin (subfolders nested) in the
// open bin, in one undo step; File > Import puts files in the open bin too (Main.importFilesNow).
// I.1 behaviour kept: files dropped here are imported, "+" imports, media cards drag onto the timeline
// (application/x-wabcut-media), a double-click shows the clip in the Source Monitor, a sequence opens in its tab.
// I.3: the proxy chip on each card and the proxy queue pill under the grid.
// UX rework (stream L): Premiere's thumbnail-size slider at the right of the row under the grid (zoom-out / zoom-in
// buttons around it; the same steps as Ctrl+wheel, = / - and Shift+] / Shift+[), and the Project panel's selection
// keys (Home, End, Page Up / Down, the arrows when a toolbar button has the focus): Library.selectionMoveRequested,
// answered by moveSelection() in the view that shows.
// P1 round (0.6): bins drag into bins with the Library's own drag (no system drag: the bin, crumb or tab under the
// pointer lights up and a chip follows it; Escape-free, a release elsewhere does nothing), Cut / Paste and Move to Bin
// from the item menu (cut items dim; Escape forgets them), Link Media… and a double-click on an offline item open the
// Relink dialog for it (Library.requestLinkMedia), Properties… (Library.requestProperties), and the footer's summary
// line (Library.summaryText: items, selection, size, length).
// P1 round (0.6, stream 2D): the cards' usage badge and "Unused" mark with the usage menu (Find in Timeline, #40), the
// delete confirmation for items sequences use (#38: their clips go too, one undo step), and twirl-down bins in the list
// view (#46, LibraryListView).
Panel {
    id: root

    KeymapScope.scope: "project"

    required property MockProject project
    /// The item being renamed in the grid ("" = none).
    property string renaming: ""

    signal importRequested()
    /// 0.4: the ··· menu's Import folder… (the window opens its folder picker: Main.openImportFolder).
    signal importFolderRequested()
    signal filesDropped(var urls)
    /// 0.2 (P.3): show the Metadata panel / the Production panel.
    signal metadataPanelRequested()
    signal productionPanelRequested()

    // P1 round (#41): a bin dragged inside the Library (LibraryCard.binDragMoved): where the pointer is, the bin under it
    // (binDropTarget, "" = the root; binDropValid: there is one and the selection may go there).
    property bool binDragging: false
    property point binDragPoint: Qt.point(0, 0)
    property string binDropTarget: ""
    property bool binDropValid: false
    /// The bin under the scene point: the bin bar's crumbs and tabs, else a bin card or row of the view that shows.
    /// Null when there is none, or when it is one of the bins being dragged (a bin cannot go into itself).
    function binAt(sceneX: real, sceneY: real): var {
        let found = binBar.visible ? binBar.binAt(sceneX, sceneY) : null
        if (found === null && content.visible) {
            let key = ""
            if (grid.visible) {
                const p = grid.mapFromItem(null, sceneX, sceneY)
                if (p.x >= 0 && p.y >= 0 && p.x < grid.width && p.y < grid.height)
                    key = Library.items.keyOf(grid.indexAt(p.x + grid.contentX, p.y + grid.contentY))
            } else if (list.visible) {
                key = list.keyAt(sceneX, sceneY)
            } else if (freeform.visible) {
                key = freeform.keyAt(sceneX, sceneY)
            }
            const d = key !== "" ? Library.details(key) : ({})
            if (d.kind === "bin" && d.searchBin !== true)
                found = { key: key }
        }
        if (found === null || (found.key !== "" && Library.isSelected(found.key)))
            return null
        return found
    }
    function binDragMoved(scenePosition: point) {
        root.binDragging = true
        root.binDragPoint = root.mapFromItem(null, scenePosition.x, scenePosition.y)
        const found = root.binAt(scenePosition.x, scenePosition.y)
        root.binDropValid = found !== null
        root.binDropTarget = found !== null ? found.key : ""
    }
    /// The release: into the bin under the pointer (one undo step), else (the Freeform view) by `dx`, `dy` there.
    function binDragFinished(scenePosition: point, dx: real, dy: real) {
        const found = root.binAt(scenePosition.x, scenePosition.y)
        root.binDragging = false
        root.binDropValid = false
        root.binDropTarget = ""
        // After the dragged card's handler has returned: the move re-lists the bin (the card goes away).
        const keys = Library.selection
        if (found !== null)
            Qt.callLater(() => Library.moveItems(keys, found.key))
        else if (freeform.visible && (dx !== 0 || dy !== 0))
            Qt.callLater(() => Library.moveFreeform(keys, dx, dy))
    }
    readonly property string binDropShown: root.binDropValid ? root.binDropTarget : "none"

    title: qsTr("Library")

    // Focus target for Window > Library and its focus key: the list, or the selected (else the first) card.
    function focusContent() {
        if (Library.view === "list") {
            list.forceActiveFocus(Qt.TabFocusReason)
            return
        }
        if (freeform.visible) {
            freeform.forceActiveFocus(Qt.TabFocusReason)
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
            const d = Library.details(key)
            if (d.offline === true)
                Library.requestLinkMedia([key]) // P1 round (#43): Premiere's Link Media… on a double-click
            else if (d.key !== undefined)
                SourceMonitor.load(key)
        }
    }
    function showMenu(key, kind, target, point, fraction) {
        itemMenu.openFor(target, key, kind, point, fraction)
    }
    /// The Project panel's selection keys (Library.selectionMoveRequested): moves the selection - extends it with
    /// `extend` - "up", "down", "left", "right", "home", "end", "pageUp" or "pageDown" in the view that shows (the grid
    /// by its columns and the rows in view, the list by rows, the Freeform view in list order). Nothing while the Text
    /// tab or the Smart search results hide the items: a selection nobody can see is never moved (Delete acts on it).
    function moveSelection(where: string, extend: bool) {
        const count = Library.items.count
        if (count === 0 || !content.visible)
            return
        const current = root.selectionLeadRow()
        const columns = grid.visible ? grid.columns : 1
        const pageRows = grid.visible ? Math.max(1, Math.floor(grid.height / grid.cellHeight))
                       : list.visible ? list.pageRows : 1
        const delta = where === "up" ? -columns
                    : where === "down" ? columns
                    : where === "left" ? -1
                    : where === "right" ? 1
                    : where === "home" ? -current
                    : where === "end" ? count - 1 - current
                    : where === "pageUp" ? -columns * pageRows
                    : where === "pageDown" ? columns * pageRows : 0
        if (grid.visible)
            grid.step(delta, extend)
        else if (list.visible)
            list.showRow(root.stepSelection(delta, extend))
        else if (freeform.visible)
            root.stepSelection(delta, extend)
    }

    // The keyboard's place in the selection (Premiere's): Shift+arrows grow and shrink the range from the item the last
    // arrow key went to (the lead), while the anchor (the item the range started from, LibraryBridge) stays put. A
    // selection made any other way (a click, Select All, Find) starts over from its last item. Shared by the grid, the
    // list and the Freeform view.
    property string selectionLead: ""
    property bool movingSelection: false
    /// The row the selection keys move from: the lead while it is selected and listed, else the last selected row
    /// (-1: nothing is selected).
    function selectionLeadRow(): int {
        if (root.selectionLead !== "" && Library.isSelected(root.selectionLead)) {
            const row = Library.items.rowOf(root.selectionLead)
            if (row >= 0)
                return row
        }
        const keys = Library.selection
        return keys.length > 0 ? Library.items.rowOf(keys[keys.length - 1]) : -1
    }
    /// Selects the item `delta` rows from the lead (`extend`: the range from the anchor to it); returns its row (-1 when
    /// nothing is listed).
    function stepSelection(delta: int, extend: bool): int {
        const count = Library.items.count
        if (count === 0)
            return -1
        const to = Math.max(0, Math.min(count - 1, root.selectionLeadRow() + delta))
        const key = Library.items.keyOf(to)
        root.movingSelection = true
        Library.select(key, extend ? Library.Range : Library.Replace)
        root.movingSelection = false
        root.selectionLead = key
        return to
    }
    // While the Text tab or the Smart search results hide the items, the Library's own keys (the selection keys, the
    // card size) belong to the focused control (a Text tab slider takes its arrows) instead of the hidden grid.
    Keys.onShortcutOverride: event => event.accepted = !content.visible
                                                        && (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) === 0
                                                        && [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End,
                                                            Qt.Key_PageUp, Qt.Key_PageDown, Qt.Key_Equal, Qt.Key_Minus,
                                                            Qt.Key_BracketLeft, Qt.Key_BracketRight, Qt.Key_BraceLeft,
                                                            Qt.Key_BraceRight].includes(event.key)
    // P1 round (#41): Escape forgets what Cut marked.
    Keys.onEscapePressed: event => {
        event.accepted = Library.cutKeys.length > 0
        if (event.accepted)
            Library.clearCut()
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
        function onSelectionMoveRequested(where, extend) {
            root.moveSelection(where, extend)
        }
        function onSelectionChanged() {
            if (!root.movingSelection)
                root.selectionLead = "" // a click or another change: the keys start over from it
        }
        // P1 round (#38): Delete (and Delete with Options) of items sequences use asks first.
        function onDeleteConfirmRequested(keys, impact, withOptions) {
            deletePopover.openFor(keys, impact, withOptions)
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
            objectName: "libraryFreeformButton"
            compact: true
            checkable: true
            checked: Library.view === "freeform"
            iconName: "shapes"
            tooltip: qsTr("Freeform view")
            onClicked: Library.view = "freeform"
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
            anchors.right: searchBinButton.left
            anchors.rightMargin: Theme.space(1)
            anchors.verticalCenter: parent.verticalCenter
            onQueryChanged: Library.findText = searchBar.smart ? "" : searchBar.query
            onSmartChanged: Library.findText = searchBar.smart ? "" : searchBar.query
        }
        // Premiere's "Create new search bin": a search bin that finds what the search finds now.
        IconButton {
            id: searchBinButton
            objectName: "librarySearchBinButton"
            anchors.right: filterButton.left
            anchors.rightMargin: Theme.space(1)
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            iconName: "folder-search"
            enabled: Library.filtering && !searchBar.smart
            tooltip: qsTr("Create new search bin")
            onClicked: Library.createSearchBin()
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
        dropTarget: root.binDropShown
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
        anchors.bottom: parent.bottom
        anchors.topMargin: Theme.space(1)
        anchors.bottomMargin: root.footerHeight > 0 ? root.footerHeight + Theme.space(2) : 0
        visible: tabs.currentIndex !== 2 && !smartResults.visible

        GridView {
            id: grid
            objectName: "libraryGrid"
            // Premiere's Project panel zoom: Library.thumbnailScale sizes the cards (the slider under the grid, Ctrl+wheel
            // here, = / - and Shift+] / Shift+[, or the tools menu).
            readonly property int columns: Math.max(1, Math.floor(width / (Theme.mediaCardMinWidth * Library.thumbnailScale)))
            anchors.fill: parent
            anchors.leftMargin: -Theme.space(1)
            anchors.rightMargin: -Theme.space(1)
            // 0.2 (P.3): the Freeform view needs a plain bin; while Find or a search bin lists items, the grid shows.
            visible: Library.view === "grid" || (Library.view === "freeform" && !Library.freeformAvailable)
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

            WheelHandler {
                objectName: "libraryThumbnailZoom"
                acceptedModifiers: Qt.ControlModifier
                onWheel: event => Library.thumbnailScale += (event.angleDelta.y > 0 ? Library.thumbnailScaleStep
                                                                                   : -Library.thumbnailScaleStep)
            }

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
                const to = root.stepSelection(delta, extend) // from the lead: Shift+arrows grow and shrink the range
                if (to >= 0)
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
                required property bool searchBin
                required property var marks // UX rework (stream T): the In/Out band
                required property bool cut // P1 round (#41)
                required property int uses // P1 round (#40): the usage badge
                required property bool unused
                required property int masterEffects // 0.6 (#71): the fx badge
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
                    searchBin: cell.searchBin
                    marks: cell.marks
                    cut: cell.cut
                    uses: cell.uses
                    unused: cell.unused
                    masterEffects: cell.masterEffects
                    dropHighlight: root.binDropShown === cell.key
                    onActivated: root.activate(cell.key, cell.kind)
                    onMenuRequested: point => root.showMenu(cell.key, cell.kind, card, point, card.scrubFraction)
                    onUsageRequested: position => usageMenu.openFor(cell.key, card, position.x, position.y)
                    onBinDragMoved: scenePosition => root.binDragMoved(scenePosition)
                    onBinDragFinished: scenePosition => root.binDragFinished(scenePosition, 0, 0)
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
            dropTarget: root.binDropShown
            onStepRequested: (delta, extend) => list.showRow(root.stepSelection(delta, extend))
            onActivated: (key, kind) => root.activate(key, kind)
            onMenuRequested: (key, target, position) => root.showMenu(key, Library.details(key).kind ?? "", target, position, -1)
            onBinDragMoved: scenePosition => root.binDragMoved(scenePosition)
            onBinDragFinished: scenePosition => root.binDragFinished(scenePosition, 0, 0)
        }

        LibraryFreeformView {
            id: freeform
            anchors.fill: parent
            visible: Library.view === "freeform" && Library.freeformAvailable
            dropTarget: root.binDropShown
            onActivated: (key, kind) => root.activate(key, kind)
            onMenuRequested: (key, kind, target, position, fraction) => root.showMenu(key, kind, target, position, fraction)
            onSaveLayoutRequested: layoutPopover.open()
            onUsageRequested: (key, target, position) => usageMenu.openFor(key, target, position.x, position.y)
            onBinDragMoved: scenePosition => root.binDragMoved(scenePosition)
            onBinDragFinished: (scenePosition, dx, dy) => root.binDragFinished(scenePosition, dx, dy)
        }
    }

    // AI-8: Smart search results in the grid's place, narrowed by the Find filters.
    SearchResultsPanel {
        id: smartResults
        objectName: "smartSearchPanel"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabs.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: Theme.space(3)
        anchors.bottomMargin: proxyPill.visible ? proxyPill.height + Theme.space(2) : 0
        visible: searchBar.smartActive && tabs.currentIndex !== 2
        results: Library.filterCount > 0 ? SmartSearch.results.filter(hit => Library.passesFilters(hit.mediaId))
                                         : SmartSearch.results
    }

    // P1 round (#45): "48 items · 3 selected · 12.4 GB · 1:22:10" at the left of the row under the views, in every view;
    // it elides before the proxy pill.
    Text {
        id: summary
        objectName: "librarySummary"
        anchors.left: parent.left
        anchors.right: proxyPill.visible ? proxyPill.left : zoomRow.visible ? zoomRow.left : parent.right
        anchors.rightMargin: Theme.space(2)
        anchors.bottom: parent.bottom
        height: Theme.controlHeightSmall
        visible: tabs.currentIndex !== 2 && !smartResults.visible
        verticalAlignment: Text.AlignVCenter
        text: Library.summaryText
        elide: Text.ElideRight
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    // "Proxies 3/12": proxies the project has / wants, under the grid; a click opens Media & cache settings.
    StatusPill {
        id: proxyPill
        objectName: "proxyStatusPill"
        anchors.right: zoomRow.visible ? zoomRow.left : parent.right
        anchors.rightMargin: zoomRow.visible ? Theme.space(2) : 0
        anchors.bottom: parent.bottom
        // Never under the card-size buttons (a very narrow Library cuts the pill's text at its edge).
        width: Math.min(implicitWidth, parent.width - (zoomRow.visible ? zoomRow.width + Theme.space(2) : 0))
        clip: true
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

    // The card size (Premiere's zoom slider): at the right of the row under the grid, with zoom-out / zoom-in buttons
    // that step it like the keys. Grid view only (the list has rows; the Freeform view sizes its cards in the project).
    Row {
        id: zoomRow
        objectName: "libraryZoomRow"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: content.visible && grid.visible
        spacing: Theme.space(1)

        IconButton {
            objectName: "libraryZoomOut"
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            iconName: "zoom-out"
            tooltip: qsTr("Smaller thumbnails")
            command: "project.thumbnailSizePrevious"
            enabled: Library.thumbnailScale > Library.thumbnailScaleMin
            onClicked: Library.thumbnailScale -= Library.thumbnailScaleStep
        }
        ValueSlider {
            id: zoomSlider
            objectName: "libraryThumbnailSlider"
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.space(24)
            // A narrow Library with the proxy pill keeps the buttons only.
            visible: !proxyPill.visible || zoomRow.parent.width >= proxyPill.implicitWidth + width
                                                                  + 2 * Theme.controlHeightSmall + Theme.space(5)
            from: Library.thumbnailScaleMin
            to: Library.thumbnailScaleMax
            stepSize: Library.thumbnailScaleStep
            snapMode: Slider.SnapAlways
            value: Library.thumbnailScale
            onMoved: Library.thumbnailScale = value
            Accessible.name: qsTr("Thumbnail size")
            // The arrows step the slider while it has the focus (not the Library's selection keys).
            Keys.onShortcutOverride: event => event.accepted = (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier | Qt.ShiftModifier)) === 0
                                                               && [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down].includes(event.key)

            Tooltip {
                visible: zoomSlider.hovered || zoomSlider.pressed
                //: The Library's card size, e.g. "Thumbnail size: 125%"
                text: qsTr("Thumbnail size: %1%").arg(Math.round(zoomSlider.value * 100))
            }
        }
        IconButton {
            objectName: "libraryZoomIn"
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            iconName: "zoom-in"
            tooltip: qsTr("Larger thumbnails")
            command: "project.thumbnailSizeNext"
            enabled: Library.thumbnailScale < Library.thumbnailScaleMax
            onClicked: Library.thumbnailScale += Library.thumbnailScaleStep
        }
    }
    // The row under the views (the summary, the proxy pill, the card size): the views end above it.
    readonly property real footerHeight: Math.max(proxyPill.visible ? proxyPill.height : 0,
                                                  zoomRow.visible ? zoomRow.height : 0,
                                                  summary.visible ? summary.height : 0)

    // P1 round (#41): the chip that follows a bin dragged inside the Library.
    Rectangle {
        objectName: "libraryBinDragChip"
        visible: root.binDragging
        x: root.binDragPoint.x + Theme.space(3)
        y: root.binDragPoint.y + Theme.space(3)
        z: 1000
        width: chipRow.implicitWidth + Theme.space(4)
        height: chipRow.implicitHeight + Theme.space(2)
        radius: height / 2
        color: root.binDropValid ? Theme.accentFill : Theme.raised
        border.width: Theme.hairline
        border.color: Theme.borderStrong
        Row {
            id: chipRow
            anchors.centerIn: parent
            spacing: Theme.space(1)
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "folder"
                size: Theme.iconXs
                color: root.binDropValid ? Theme.accentOnFill : Theme.textSecondary
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                //: The chip following bins dragged in the Library, e.g. "Move 2 items"
                text: qsTr("Move %n item(s)", "", Library.selectionCount)
                color: root.binDropValid ? Theme.accentOnFill : Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
            }
        }
    }

    LibraryItemMenu {
        id: itemMenu
        onRenameRequested: key => root.rename(key)
        onMetadataRequested: key => metadataPopover.openFor(key, root)
        onMetadataPanelRequested: root.metadataPanelRequested()
        onEditSearchBinRequested: key => searchBinPopover.openEdit(key, searchRow)
        onSubclipRequested: key => subclipPopover.openMakeOf(key, searchRow)
        onEditSubclipRequested: key => subclipPopover.openEdit(key, searchRow)
        onOpenRequested: (key, kind) => root.activate(key, kind)
        onLinkMediaRequested: keys => Library.requestLinkMedia(keys)
        onPropertiesRequested: key => Library.requestProperties(key)
        onRevealSourceRequested: key => {
            const shown = Production.revealSource(key)
            if (shown.ok && !shown.switched)
                root.productionPanelRequested() // browsed read-only there, the item pointed at
        }
    }

    // The tools menu: Import Image Sequence, hover scrub, unused media, duplicates, Make Subclip.
    LibraryToolsMenu {
        id: toolsMenu
        onRemoveUnusedRequested: removeUnusedPopover.open()
        onMakeSubclipRequested: root.makeSubclip()
        onImportImageSequenceRequested: imageSequenceDialog.openDialog()
        onNewSearchBinRequested: searchBinPopover.openNew(searchRow)
        onImportFromProjectRequested: importProjectDialog.open()
        onProductionRequested: root.productionPanelRequested()
        onImportFolderRequested: root.importFolderRequested()
    }

    SearchBinPopover {
        id: searchBinPopover
    }

    // P1 round (#40): the usage badge's list of places (Find in Timeline).
    UsageMenu {
        id: usageMenu
    }

    // P1 round (#38): deleting items whose media (or nested sequence) sequences use: "Hero.mp4 is used by 14 clips in 3
    // sequences. Delete it and remove those clips?" - Delete takes the items and their clips (gaps stay) in one undo
    // step. Delete with Options (Ctrl/Cmd+Delete) shows it every time, with the clips' removal as a switch: off, only the
    // items no sequence uses go and the used ones stay (fix round 1, ARGON-12: never a dead-end switch).
    Popover {
        id: deletePopover
        objectName: "libraryDeletePopover"
        parent: searchRow
        property var keys: []
        property var impact: ({})
        property bool withOptions: false
        readonly property int clips: deletePopover.impact.clips ?? 0
        readonly property bool removeClips: deletePopover.clips > 0 && (!deletePopover.withOptions || removeClipsToggle.checked)
        /// Delete with Options with the switch off: the used items stay; `unusedKeys` (deleteImpact) go.
        readonly property bool keepUsed: deletePopover.withOptions && deletePopover.clips > 0 && !removeClipsToggle.checked
        readonly property var unusedKeys: deletePopover.impact.unusedKeys ?? []
        /// "“Main edit”" for one sequence, else "3 sequences"; "1 clip", "14 clips" (whole words: read at a glance).
        readonly property string sequencesText: (deletePopover.impact.sequences ?? 0) === 1
                                                ? qsTr("“%1”").arg((deletePopover.impact.sequenceNames ?? [""])[0])
                                                : qsTr("%1 sequences").arg(deletePopover.impact.sequences ?? 0)
        readonly property string clipsText: deletePopover.clips === 1 ? qsTr("1 clip") : qsTr("%1 clips").arg(deletePopover.clips)
        function openFor(keys: var, impact: var, withOptions: bool) {
            deletePopover.keys = keys
            deletePopover.impact = impact
            deletePopover.withOptions = withOptions
            removeClipsToggle.checked = true
            deletePopover.open()
            deleteConfirm.forceActiveFocus()
        }
        title: deletePopover.withOptions ? qsTr("Delete with Options") : qsTr("Delete items in use?")
        Column {
            width: Theme.space(84)
            spacing: Theme.space(2)
            Text {
                objectName: "libraryDeleteText"
                width: parent.width
                wrapMode: Text.WordWrap
                text: deletePopover.clips === 0
                      ? ((deletePopover.impact.items ?? 0) === 1
                         ? qsTr("Delete this item from the project? No sequence uses it. The files stay on disk, and Undo brings it back.")
                         : qsTr("Delete these %1 items from the project? No sequence uses them. The files stay on disk, and Undo brings them back.").arg(deletePopover.impact.items ?? 0))
                      : (deletePopover.impact.usedItems ?? 0) <= 1
                        //: "“Hero.mp4” is used by 14 clips in 3 sequences. ..."
                        ? qsTr("“%1” is used by %2 in %3. Delete it and remove those clips? The gaps stay, the files stay on disk, and Undo brings everything back.")
                              .arg(deletePopover.impact.firstName ?? "").arg(deletePopover.clipsText).arg(deletePopover.sequencesText)
                        //: "3 of these items are used by 14 clips in 2 sequences. ..."
                        : qsTr("%1 of these items are used by %2 in %3. Delete them and remove those clips? The gaps stay, the files stay on disk, and Undo brings everything back.")
                              .arg(deletePopover.impact.usedItems ?? 0).arg(deletePopover.clipsText).arg(deletePopover.sequencesText)
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            // 0.6 (card #74): an audition that keeps other takes loses only the takes that play what goes.
            Text {
                objectName: "libraryDeleteTakesText"
                visible: (deletePopover.impact.takes ?? 0) > 0
                width: parent.width
                wrapMode: Text.WordWrap
                text: (deletePopover.impact.takes ?? 0) === 1
                      ? qsTr("Of those, an audition loses only 1 take and keeps its other takes.")
                      : qsTr("Of those, auditions lose only %1 takes and keep their other takes.").arg(deletePopover.impact.takes ?? 0)
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Toggle {
                id: removeClipsToggle
                objectName: "libraryDeleteRemoveClips"
                visible: deletePopover.withOptions && deletePopover.clips > 0
                text: qsTr("Remove their clips from sequences")
                checked: true
            }
            Text {
                objectName: "libraryDeleteKeepUsedText"
                visible: deletePopover.keepUsed
                width: parent.width
                wrapMode: Text.WordWrap
                text: deletePopover.unusedKeys.length === 0
                      ? qsTr("Every item here is used by a sequence: nothing would be deleted.")
                      : qsTr("Only the items no sequence uses are deleted (%1 of %2); the used ones stay in the project.")
                            .arg(deletePopover.unusedKeys.length).arg(deletePopover.keys.length)
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            Row {
                spacing: Theme.space(2)
                PillButton {
                    id: deleteConfirm
                    objectName: "libraryDeleteConfirm"
                    variant: "primary"
                    text: deletePopover.removeClips ? qsTr("Delete and Remove Clips")
                          : deletePopover.keepUsed ? qsTr("Delete Unused Items") : qsTr("Delete")
                    enabled: deletePopover.clips === 0 || deletePopover.removeClips || deletePopover.unusedKeys.length > 0
                    disabledReason: qsTr("Every item here is used by a sequence: turn on Remove their clips to delete them")
                    onClicked: {
                        const keys = deletePopover.keepUsed ? deletePopover.unusedKeys : deletePopover.keys
                        const removeClips = deletePopover.removeClips
                        deletePopover.close()
                        Library.deleteItems(keys, removeClips)
                    }
                }
                PillButton {
                    objectName: "libraryDeleteCancel"
                    variant: "ghost"
                    text: qsTr("Cancel")
                    onClicked: deletePopover.close()
                }
            }
        }
    }

    FileDialog {
        id: importProjectDialog
        title: qsTr("Import from project")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Wabcut projects (*.wabcut)")]
        currentFolder: ProjectFile.folder
        onAccepted: Production.importProject(selectedFile)
    }

    // Save as New Layout (the Freeform view's menu).
    Popover {
        id: layoutPopover
        objectName: "libraryLayoutPopover"
        parent: searchRow
        title: qsTr("Save as New Layout")
        onOpened: {
            layoutName.text = qsTr("Layout %1").arg(Library.freeformLayouts.length + 1)
            layoutName.forceActiveFocus()
            layoutName.selectAll()
        }
        Column {
            width: Theme.space(72)
            spacing: Theme.space(2)
            FieldInput {
                id: layoutName
                objectName: "libraryLayoutName"
                width: parent.width
                Accessible.name: qsTr("Layout name")
                Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
                onAccepted: layoutSave.clicked()
            }
            PillButton {
                id: layoutSave
                objectName: "libraryLayoutSave"
                variant: "primary"
                text: qsTr("Save")
                enabled: layoutName.text.trim() !== ""
                onClicked: {
                    Library.saveFreeformLayout(layoutName.text)
                    layoutPopover.close()
                }
            }
        }
    }

    ImageSequenceDialog {
        id: imageSequenceDialog
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
        text: qsTr("Drop media files or folders here, or import them with %1.").arg(KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText("file.import") : "")
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
    // 0.2 (P.3): items of another project (the Production panel) come in as copies.
    DropArea {
        id: otherProjectDrop
        objectName: "libraryOtherProjectDrop"
        anchors.fill: parent
        keys: ["application/x-wabcut-other-project"]
        onDropped: drop => {
            Production.importItems(drop.getDataAsString("application/x-wabcut-other-project").split(","))
            drop.accept(Qt.CopyAction)
        }
    }
    Rectangle {
        anchors.fill: parent
        visible: fileDrop.containsDrag || otherProjectDrop.containsDrag
        radius: Theme.radiusPanel
        color: "transparent"
        border.width: Theme.selectionWidth
        border.color: Theme.accentStroke
    }
}
