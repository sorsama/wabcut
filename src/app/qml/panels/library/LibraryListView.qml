pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// The Library's list view (M5k, Premiere's Project panel in list view): one row per bin, sequence or media item, one
// column per metadata field (Library.columns). A TableView over Library.items, so only the cells in view exist
// (10,000 items scroll like 10); every cell is one Text and, in the Name and Label columns, one mark.
//
//   * header: click to sort (again: descending), drag a header to move its column, drag its right edge to resize;
//     the columns button picks which show (the name always does);
//   * rows: click selects (Ctrl/⌘ adds, Shift extends), double-click opens (a bin opens in place, media in the Source
//     Monitor, a sequence in its timeline tab); a click on the name of a selected item, F2 or Enter renames inline;
//     double-click a Comment or Description cell to edit it. Every edit is one undo step (Library);
//   * 0.7 (card 021), while a cell is open: Tab / Shift+Tab move to the next / previous editable cell (along the row,
//     then on to the next row), Enter / Shift+Enter to the same column one row down / up (Premiere's Next / Previous
//     Column Field and Row Field: the keymap's project.nextColumnField ... via Library.fieldStepRequested); the value
//     is kept as the cell is left. In the Name column Enter just renames. Esc cancels.
//   * drag rows onto a bin row, a crumb or a bin tab to move them; media rows drag onto the timeline too.
//   * 0.7 (card 023), Premiere's List view > Thumbnails (Library.listThumbnails, in the ... menu): the Name cell shows
//     the item's thumbnail (16:9, at the row's height) and the rows grow with the size slider (Library.thumbnailScale:
//     the slider under the list, Ctrl+wheel over it, the zoom keys); off, the rows are Theme.rowHeight text lines.
//   * P1 round (#46), Premiere's twirl-down bins: a bin's chevron (or Right / Left) shows its contents under it,
//     indented, sorted within each level; Alt+click (Alt+Right) opens every level inside it; Left on a row inside a bin
//     goes to that bin. Find and search bins list flat, as before. Which bins are open is kept per project.
FocusScope {
    id: root
    objectName: "libraryList"
    activeFocusOnTab: true

    signal activated(string key, string kind)
    signal menuRequested(string key, Item target, point position)
    /// P1 round (#41): a bin row drags inside the Library only (LibraryCard.binDragMoved): the panel follows it.
    signal binDragMoved(point scenePosition)
    signal binDragFinished(point scenePosition)

    /// P1 round (#41): the bin row a Library drag of bins would drop into ("none": none).
    property string dropTarget: "none"
    /// The key of the row under the scene point `sceneX`, `sceneY` ("" outside the rows).
    function keyAt(sceneX: real, sceneY: real): string {
        const inView = table.mapFromItem(null, sceneX, sceneY)
        if (inView.x < 0 || inView.y < 0 || inView.x >= table.width || inView.y >= table.height)
            return ""
        const p = table.contentItem.mapFromItem(null, sceneX, sceneY)
        const row = Math.floor(p.y / root.rowHeight)
        return row >= 0 && row < table.rows ? Library.items.keyOf(row) : ""
    }

    readonly property var columns: Library.columns
    /// P1 round (#46): the rows are the twirl-down tree (the model's: list view, no Find, not a search bin).
    readonly property bool tree: Library.view === "list" && !Library.filtering && !Library.searchBinOpen
    /// Expands or collapses bin `key` (`recursive`: every bin inside it too), keeping the scroll where it was.
    function toggleBin(key: string, expanded: bool, recursive: bool) {
        const y = table.contentY
        Library.setExpanded(key, expanded, recursive)
        table.forceLayout()
        table.contentY = Math.max(0, Math.min(y, table.contentHeight - table.height))
    }
    /// Right / Left on the selection's last row: a closed bin opens (Alt: every level), an open one goes to its first
    /// row; Left closes an open bin, else goes to the bin the row is in.
    function twirlKey(right: bool, all: bool) {
        const keys = Library.selection
        if (keys.length === 0)
            return
        const key = keys[keys.length - 1]
        const row = Library.items.rowOf(key)
        if (row < 0)
            return
        const bin = key.startsWith("b")
        const open = bin && Library.isExpanded(key)
        if (right) {
            if (bin && (!open || all))
                root.toggleBin(key, true, all)
            else if (open)
                root.stepRequested(1, false)
            return
        }
        if (open) {
            root.toggleBin(key, false, all)
            return
        }
        const parent = Library.parentOf(key)
        const parentRow = parent !== "" ? Library.items.rowOf(parent) : -1
        if (parentRow >= 0) {
            Library.select(parent, Library.Replace)
            root.showRow(parentRow)
        }
    }
    /// Rows in view (Page Up / Page Down move by this many: LibraryPanel.moveSelection).
    readonly property int pageRows: Math.max(1, Math.floor(table.height / root.rowHeight))
    /// 0.7 (card 023): a row's height - a text line, or (thumbnails on) twice that at the slider's 100 %, following it.
    readonly property int rowHeight: Library.listThumbnails ? Math.round(Theme.rowHeight * 2 * Library.thumbnailScale)
                                                            : Theme.rowHeight
    onRowHeightChanged: {
        table.forceLayout()
        Qt.callLater(table.forceLayout) // and once the bindings behind the change have settled
    }
    property string editingKey: ""
    property string editingColumn: ""

    function startEdit(key, column) {
        root.editingKey = key
        root.editingColumn = column
    }
    // `refocus` false while the edit moves on to another cell (the next editor takes the focus itself).
    function finishEdit(value, commit, refocus = true) {
        const key = root.editingKey
        const column = root.editingColumn
        root.editingKey = ""
        root.editingColumn = ""
        if (!commit)
            return
        if (column === "name")
            Library.rename(key, value)
        else if (column === "comment")
            Library.setComment(key, value)
        else if (column === "description")
            Library.setDescription(key, value)
        else // 0.2 (P.3): scene, shot, take, logNote, client, tapeName
            Library.setMetadata([key], column, value)
        if (refocus)
            table.forceActiveFocus()
    }

    // ---- Next / Previous Column Field and Row Field (0.7, card 021) ----
    /// The field of the open cell's editor (set by the editor while it exists).
    property var cellEditor: null
    /// Whether the cell of column `columnId` in a row of `kind` can be edited: Name on bins and media (a sequence is
    /// renamed from its own menu); the metadata columns on media only; Good is a check box.
    function canEditCell(kind: string, columnId: string): bool {
        if (columnId === "good" || kind === "")
            return false
        if (columnId === "name")
            return kind !== "sequence"
        return kind !== "bin" && kind !== "sequence"
    }
    /// Tab, Shift+Tab, Enter and Shift+Enter (what the open editor leaves to the keymap).
    function stepsFieldKey(event): bool {
        return (event.modifiers & ~(Qt.ShiftModifier | Qt.KeypadModifier)) === 0
               && [Qt.Key_Tab, Qt.Key_Backtab, Qt.Key_Return, Qt.Key_Enter].includes(event.key)
    }
    /// The cell `columnDelta` cells along the row order (then on to the next row) or `rowDelta` rows down in the same
    /// column that can be edited, as { key, column }, or null at the end of the list.
    function stepTarget(key: string, columnId: string, columnDelta: int, rowDelta: int): var {
        const row = Library.items.rowOf(key)
        if (row < 0)
            return null
        if (rowDelta !== 0) {
            for (let r = row + rowDelta; r >= 0 && r < table.rows; r += rowDelta) {
                const k = Library.items.keyOf(r)
                if (root.canEditCell(Library.details(k).kind ?? "", columnId))
                    return { key: k, column: columnId }
            }
            return null
        }
        const ids = root.columns.filter(c => c.editable && c.id !== "good").map(c => c.id)
        let c = ids.indexOf(columnId) + columnDelta
        for (let r = row; r >= 0 && r < table.rows; r += columnDelta) {
            const k = Library.items.keyOf(r)
            const kind = Library.details(k).kind ?? ""
            for (; c >= 0 && c < ids.length; c += columnDelta) {
                if (root.canEditCell(kind, ids[c]))
                    return { key: k, column: ids[c] }
            }
            c = columnDelta > 0 ? 0 : ids.length - 1
        }
        return null
    }
    /// Scrolls the cell into view (a cell not built yet is made when it comes in sight).
    function showCell(key: string, columnId: string) {
        const row = Library.items.rowOf(key)
        const column = root.columns.findIndex(c => c.id === columnId)
        if (row >= 0 && column >= 0)
            table.positionViewAtCell(Qt.point(column, row), TableView.Contain)
    }
    /// Keeps the value typed, then opens the next cell (nothing further: the edit just ends).
    function stepField(columnDelta: int, rowDelta: int) {
        if (root.editingKey === "" || root.cellEditor === null)
            return
        const key = root.editingKey
        const column = root.editingColumn
        const value = root.cellEditor.text
        // Enter in the Name column only renames (the plain rename-commit).
        const target = rowDelta !== 0 && column === "name" ? null : root.stepTarget(key, column, columnDelta, rowDelta)
        root.finishEdit(value, true, target === null)
        if (target === null)
            return
        Qt.callLater(() => {
            root.startEdit(target.key, target.column)
            root.showCell(target.key, target.column)
        })
    }
    Connections {
        target: Library
        function onFieldStepRequested(columnDelta: int, rowDelta: int) {
            root.stepField(columnDelta, rowDelta)
        }
    }
    // The keymap's field keys are the Library's only while a cell is open.
    Binding {
        target: Library
        property: "fieldEditing"
        value: root.editingKey !== ""
    }
    // A click on the name of the item already selected renames it after the double-click interval (a double-click
    // opens it instead).
    property string pendingRename: ""
    Timer {
        id: renameTimer
        interval: Application.styleHints.mouseDoubleClickInterval + 40
        onTriggered: {
            if (root.pendingRename !== "")
                root.startEdit(root.pendingRename, "name")
            root.pendingRename = ""
        }
    }
    /// Up / Down (Shift extends): the selection moves `delta` rows. LibraryPanel moves it (it keeps the keyboard's place
    /// in the selection for every view: Shift+Up / Shift+Down grow and shrink the range) and calls showRow.
    signal stepRequested(int delta, bool extend)
    /// Scrolls the row `row` into view (-1: nothing).
    function showRow(row: int) {
        if (row >= 0 && row < table.rows)
            table.positionViewAtRow(row, TableView.Contain)
    }

    Keys.onPressed: event => {
        const extend = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Down) {
            root.stepRequested(1, extend)
            event.accepted = true
        } else if (event.key === Qt.Key_Up) {
            root.stepRequested(-1, extend)
            event.accepted = true
        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && Library.selection.length > 0) {
            const key = Library.selection[0]
            root.activated(key, Library.details(key).kind ?? "")
            event.accepted = true
        } else if (event.key === Qt.Key_F2 && Library.selection.length === 1) {
            root.startEdit(Library.selection[0], "name")
            event.accepted = true
        } else if (root.tree && (event.key === Qt.Key_Right || event.key === Qt.Key_Left)) {
            root.twirlKey(event.key === Qt.Key_Right, (event.modifiers & Qt.AltModifier) !== 0)
            event.accepted = true
        }
    }
    // The arrows, Return and F2 belong to the list while it has focus (Delete stays project.delete).
    // P1 round (#46): Right / Left (Alt: every level) twirl bins while the list shows the tree.
    // 0.7 (card 021): with a cell open Return belongs to the field keys (project.nextRowField), not to the list.
    Keys.onShortcutOverride: event => event.accepted = ((event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) === 0
                                                         && [Qt.Key_Up, Qt.Key_Down, Qt.Key_Return, Qt.Key_Enter, Qt.Key_F2].includes(event.key)
                                                         && !(root.editingKey !== "" && [Qt.Key_Return, Qt.Key_Enter].includes(event.key)))
                                                        || (root.tree && (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.ShiftModifier)) === 0
                                                            && [Qt.Key_Left, Qt.Key_Right].includes(event.key))

    // ---- Header ----
    Rectangle {
        id: header
        objectName: "libraryListHeader"
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.controlHeightSmall
        color: Theme.panel
        clip: true

        Row {
            id: headerRow
            x: -table.contentX
            height: parent.height

            Repeater {
                model: root.columns

                delegate: Item {
                    id: headerCell
                    required property var modelData
                    required property int index
                    readonly property bool sorted: Library.sortColumn === modelData.id
                    objectName: "libraryColumn_" + modelData.id
                    width: modelData.width
                    height: headerRow.height
                    Accessible.role: Accessible.ColumnHeader
                    Accessible.name: !headerCell.sorted ? headerCell.modelData.title
                                     : Library.sortAscending ? qsTr("%1, sorted ascending").arg(headerCell.modelData.title)
                                     : qsTr("%1, sorted descending").arg(headerCell.modelData.title)
                    Accessible.onPressAction: Library.sortBy(headerCell.modelData.id)

                    Rectangle {
                        anchors.fill: parent
                        color: headerTap.pressed ? Theme.hover : "transparent"
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.right: sortMark.left
                        anchors.leftMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        text: headerCell.modelData.title
                        elide: Text.ElideRight
                        color: headerCell.sorted ? Theme.textPrimary : Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        font.weight: Font.Medium
                    }
                    Icon {
                        id: sortMark
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        size: Theme.iconXs
                        visible: headerCell.sorted
                        name: Library.sortAscending ? "chevron-down" : "chevron-right"
                        rotation: Library.sortAscending ? 180 : 90
                        color: Theme.accentStroke
                    }
                    Rectangle {
                        anchors.right: parent.right
                        width: Theme.hairline
                        height: parent.height * 0.5
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.border
                    }
                    TapHandler {
                        id: headerTap
                        onTapped: Library.sortBy(headerCell.modelData.id)
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    // Drag a header sideways to move its column.
                    DragHandler {
                        id: moveDrag
                        target: null
                        xAxis.enabled: true
                        yAxis.enabled: false
                        onActiveChanged: {
                            if (active)
                                return
                            const dropX = headerCell.x + moveDrag.centroid.position.x
                            let to = 0
                            for (let i = 0; i < headerRow.children.length; ++i) {
                                const c = headerRow.children[i]
                                if (c.width !== undefined && dropX >= c.x && dropX < c.x + c.width)
                                    to = i
                            }
                            if (dropX >= headerRow.width)
                                to = root.columns.length - 1
                            Library.moveColumn(headerCell.index, Math.min(to, root.columns.length - 1))
                        }
                    }
                    // The right edge resizes the column.
                    MouseArea {
                        objectName: "libraryColumnResize_" + headerCell.modelData.id
                        anchors.right: parent.right
                        width: Theme.space(2)
                        height: parent.height
                        cursorShape: Qt.SplitHCursor
                        property real startX: 0
                        property real startWidth: 0
                        onPressed: mouse => {
                            startX = mapToItem(header, mouse.x, 0).x
                            startWidth = headerCell.width
                        }
                        onPositionChanged: mouse => {
                            if (pressed)
                                Library.setColumnWidth(headerCell.modelData.id,
                                                       Math.round(startWidth + mapToItem(header, mouse.x, 0).x - startX))
                        }
                        onReleased: table.forceLayout()
                    }
                }
            }
        }
    }
    Rectangle {
        anchors.top: header.bottom
        width: parent.width
        height: Theme.hairline
        color: Theme.border
    }

    // ---- Rows ----
    TableView {
        id: table
        objectName: "libraryTable"
        anchors.top: header.bottom
        anchors.topMargin: Theme.hairline
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        focus: true
        model: Library.items
        reuseItems: true
        keyNavigationEnabled: false // the list's own keys select rows (Library.selection)
        interactive: false // desktop: the wheel and the scroll bars scroll; drags move rows
        boundsBehavior: Flickable.StopAtBounds
        columnWidthProvider: column => column < root.columns.length ? root.columns[column].width : 0
        rowHeightProvider: () => root.rowHeight
        ScrollBar.vertical: ThinScrollBar {}
        ScrollBar.horizontal: ThinScrollBar {}
        Accessible.role: Accessible.Table
        Accessible.name: qsTr("Library list: %n item(s)", "", Library.items.count)

        Connections {
            target: Library
            function onColumnsChanged() {
                table.forceLayout()
            }
        }

        // 0.7 (card 023): with thumbnails on, Ctrl+wheel sizes them (as over the grid) instead of scrolling.
        WheelHandler {
            objectName: "libraryListThumbnailZoom"
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.ControlModifier
            enabled: Library.listThumbnails
            onWheel: event => Library.thumbnailScale += (event.angleDelta.y > 0 ? Library.thumbnailScaleStep
                                                                               : -Library.thumbnailScaleStep)
        }
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: event => {
                if (Library.listThumbnails && (event.modifiers & Qt.ControlModifier))
                    return
                const dy = event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.angleDelta.y / 120 * Theme.rowHeight * 3
                const dx = event.pixelDelta.x !== 0 ? event.pixelDelta.x : event.angleDelta.x / 120 * Theme.space(16)
                table.contentY = Math.max(0, Math.min(table.contentHeight - table.height, table.contentY - dy))
                table.contentX = Math.max(0, Math.min(Math.max(0, table.contentWidth - table.width), table.contentX - dx))
            }
        }

        delegate: Rectangle {
            id: cell

            required property int row
            required property int column
            required property string display
            required property string key
            required property string kind
            required property string columnId
            required property bool editable
            required property int label
            required property bool selected
            required property bool offline
            required property bool generated
            required property var subclip
            required property string binPath
            required property bool good
            required property bool searchBin
            required property bool cut // P1 round (#41): dimmed until Paste moves it
            required property int depth // P1 round (#46): the level under the bin shown
            required property bool expandable
            required property bool expanded
            required property int masterEffects // 0.6 (#71): the Name cell's fx chip
            required property string thumbnail // 0.7 (card 023): the Name cell's picture ("" in the other columns)

            readonly property bool editing: root.editingKey === key && root.editingColumn === columnId
            readonly property bool isName: columnId === "name"
            readonly property bool isGood: columnId === "good"
            /// P1 round (#46): where the Name cell's chevron sits (the tree indents each level).
            readonly property real indent: isName && root.tree ? Theme.space(1) + depth * Theme.space(4) : 0

            /// 0.7 (card 023): the Name cell of a media item or sequence shows its thumbnail (bins keep their folder icon).
            readonly property bool showsThumb: isName && Library.listThumbnails && kind !== "bin"

            implicitWidth: 120
            implicitHeight: root.rowHeight
            color: selected ? Theme.accentWash : (row % 2 === 1 ? Theme.sunken : "transparent")

            Accessible.role: Accessible.Cell
            Accessible.name: !cell.isName || !cell.expandable ? cell.display
                             : cell.expanded ? qsTr("%1, expanded").arg(cell.display) : qsTr("%1, collapsed").arg(cell.display)

            // P1 round (#46): a bin's twirl-down - the one chevron of a Name cell that has contents (Alt+click opens every
            // level inside). A Loader, so the cells of every other column and of media rows (the cells TableView keeps
            // alive) cost one bare item, not an icon.
            Loader {
                id: chevron
                objectName: cell.isName ? "libraryTwirl_" + cell.key : ""
                active: cell.isName && cell.expandable
                x: cell.indent
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.iconXs
                height: Theme.iconXs
                sourceComponent: Icon {
                    size: Theme.iconXs
                    name: "chevron-right"
                    rotation: cell.expanded ? 90 : 0
                    color: Theme.textTertiary
                    TapHandler {
                        id: chevronTap
                        margin: Theme.space(1.5)
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: root.toggleBin(cell.key, !cell.expanded, (chevronTap.point.modifiers & Qt.AltModifier) !== 0)
                    }
                }
            }
            // Name: the kind's icon; Label: the colour.
            Icon {
                id: kindIcon
                visible: cell.isName && !cell.showsThumb
                x: root.tree ? cell.indent + Theme.iconXs + Theme.space(1) : Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                size: Theme.iconXs
                name: cell.searchBin ? "folder-search" : cell.kind === "bin" ? "folder" : cell.kind === "sequence" ? "film"
                      : cell.kind === "audio" ? "music" : cell.kind === "title" ? "type"
                      : cell.kind === "subclip" ? "scissors" : cell.kind === "still" ? "image" : "film"
                color: cell.kind === "bin" ? Theme.accentStroke : Theme.textTertiary
            }
            // 0.7 (card 023): the thumbnail, 16:9 at the row's height; the kind's icon where there is no picture.
            Rectangle {
                id: thumbBox
                objectName: cell.showsThumb ? "libraryListThumb_" + cell.key : ""
                visible: cell.showsThumb
                x: root.tree ? cell.indent + Theme.iconXs + Theme.space(1) : Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                height: parent.height - Theme.space(1)
                width: Math.round(height * 16 / 9)
                radius: Theme.radiusBadge
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border
                clip: true
                opacity: cell.cut ? 0.45 : 1
                Image {
                    id: thumbImage
                    anchors.fill: parent
                    visible: status === Image.Ready
                    source: cell.showsThumb ? cell.thumbnail : ""
                    sourceSize.height: thumbBox.height * 2
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                }
                Icon {
                    anchors.centerIn: parent
                    visible: !thumbImage.visible
                    size: Math.min(Theme.iconMd, thumbBox.height - Theme.space(1))
                    name: cell.kind === "sequence" ? "film" : cell.kind === "audio" ? "music" : cell.kind === "title" ? "type"
                          : cell.kind === "subclip" ? "scissors" : cell.kind === "still" ? "image" : "film"
                    color: Theme.textTertiary
                }
            }
            Rectangle {
                id: labelChip
                visible: cell.columnId === "label" && cell.label >= 0
                x: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.space(2.5)
                height: Theme.space(2.5)
                radius: Theme.radiusBadge
                color: Theme.clipColor(Math.max(0, cell.label))
            }
            // 0.6 (card #71): the item has Master Clip Effects (every clip of it plays them).
            Rectangle {
                id: masterFxChip
                objectName: cell.isName ? "libraryMasterFx_" + cell.key : ""
                visible: cell.isName && cell.masterEffects > 0
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                width: masterFxLabel.implicitWidth + Theme.space(2)
                height: masterFxLabel.implicitHeight + Theme.space(0.5)
                radius: Theme.radiusBadge
                color: Theme.accentFill
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Master Clip Effects: %1").arg(cell.masterEffects)
                Text {
                    id: masterFxLabel
                    anchors.centerIn: parent
                    //: Chip in the Library list's Name cell of media with Master Clip Effects
                    text: qsTr("fx")
                    color: Theme.accentOnFill
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMicro
                    font.weight: Font.DemiBold
                }
            }
            Text {
                anchors.left: cell.isName ? (cell.showsThumb ? thumbBox.right : kindIcon.right)
                              : labelChip.visible ? labelChip.right : parent.left
                anchors.leftMargin: Theme.space(2)
                anchors.right: masterFxChip.visible ? masterFxChip.left : parent.right
                anchors.rightMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                visible: !cell.editing && !cell.isGood
                opacity: cell.cut ? 0.45 : 1
                text: cell.isName && cell.binPath !== "" ? cell.display + "  ·  " + cell.binPath : cell.display
                elide: Text.ElideRight
                color: cell.isName ? (cell.offline ? Theme.warning : Theme.textPrimary) : Theme.textSecondary
                font.family: cell.columnId === "duration" || cell.columnId === "in" || cell.columnId === "out"
                             || cell.columnId === "fps" || cell.columnId === "resolution" ? Theme.fontMono : Theme.fontSans
                font.pixelSize: cell.isName ? Theme.textSm : Theme.textXs
                font.weight: cell.isName ? Font.Medium : Font.Normal
            }
            // 0.2 (P.3): Good is a check box (a click toggles it, one undo step).
            Rectangle {
                objectName: "libraryGoodBox_" + cell.key
                visible: cell.isGood && cell.kind !== "bin" && cell.kind !== "sequence"
                x: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.space(3.5)
                height: width
                radius: Theme.radiusBadge
                color: cell.good ? Theme.accentFill : "transparent"
                border.width: Theme.hairline
                border.color: cell.good ? Theme.accentFill : Theme.borderStrong
                Accessible.role: Accessible.CheckBox
                Accessible.name: qsTr("Good")
                Accessible.checked: cell.good
                Icon {
                    anchors.centerIn: parent
                    visible: cell.good
                    name: "check"
                    size: Theme.iconXs
                    color: Theme.accentOnFill
                }
                TapHandler {
                    onTapped: Library.setMetadata([cell.key], "good", !cell.good)
                }
            }
            Loader {
                anchors.fill: parent
                active: cell.editing
                sourceComponent: FieldInput {
                    id: editor
                    objectName: "libraryCellEditor"
                    /// 0.7 (card 021): Tab, Shift+Tab, Enter and Shift+Enter are the keymap's (the dispatcher reads this).
                    readonly property bool fieldStepping: true
                    text: cell.isName ? cell.display : cell.display
                    Accessible.name: cell.isName ? qsTr("Name") : qsTr("Edit %1").arg((root.columns.find(c => c.id === cell.columnId) ?? {}).title ?? "")
                    font.pixelSize: Theme.textSm
                    Component.onCompleted: {
                        root.cellEditor = editor
                        forceActiveFocus()
                        selectAll()
                    }
                    Component.onDestruction: {
                        if (root.cellEditor === editor)
                            root.cellEditor = null
                    }
                    onAccepted: root.finishEdit(text, true)
                    Keys.onEscapePressed: event => {
                        root.finishEdit(text, false)
                        event.accepted = true
                    }
                    onActiveFocusChanged: if (!activeFocus && cell.editing) root.finishEdit(text, true)
                    // Typing keys stay in the field; the field-step keys go to the keymap.
                    Keys.onShortcutOverride: event => event.accepted = !root.stepsFieldKey(event)
                }
            }

            HoverHandler {
                enabled: !cell.editing
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                id: cellTap
                acceptedButtons: Qt.LeftButton
                onSingleTapped: {
                    table.forceActiveFocus()
                    const mods = cellTap.point.modifiers
                    const wasSelected = cell.selected && Library.selectionCount === 1
                    if (mods & Qt.ShiftModifier)
                        Library.select(cell.key, Library.Range)
                    else if (mods & (Qt.ControlModifier | Qt.MetaModifier))
                        Library.select(cell.key, Library.Toggle)
                    else
                        Library.select(cell.key, Library.Replace)
                    // A click on the name of the item already selected renames it (Premiere).
                    if (wasSelected && cell.editable && cell.isName && !(mods & (Qt.ShiftModifier | Qt.ControlModifier | Qt.MetaModifier))) {
                        root.pendingRename = cell.key
                        renameTimer.restart()
                    }
                }
                onDoubleTapped: {
                    renameTimer.stop()
                    root.pendingRename = ""
                    if (cell.editable && !cell.isName && !cell.isGood)
                        root.startEdit(cell.key, cell.columnId)
                    else
                        root.activated(cell.key, cell.kind)
                }
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: eventPoint => {
                    if (!cell.selected)
                        Library.select(cell.key, Library.Replace)
                    root.menuRequested(cell.key, cell, eventPoint.position)
                }
            }
            // Rows drag into bins (and media onto the timeline).
            Item {
                id: dragProxy
                anchors.fill: parent
                Drag.dragType: Drag.Automatic
                Drag.supportedActions: Qt.MoveAction | Qt.CopyAction
                DragHandler {
                    id: rowDrag
                    objectName: "libraryRowDragHandler"
                    /// Where a bin's drag last was (the release lands there).
                    property point lastScenePosition: Qt.point(0, 0)
                    target: null
                    enabled: !cell.editing && !cell.offline
                    // The drag's data is made when it starts, after the row is selected (no cell binds the selection).
                    // P1 round (#41): a bin row's drag is the Library's own (binDragMoved), not a system drag.
                    onActiveChanged: {
                        if (active) {
                            if (!cell.selected)
                                Library.select(cell.key, Library.Replace)
                            if (cell.kind === "bin") {
                                rowDrag.lastScenePosition = rowDrag.centroid.scenePosition
                                root.binDragMoved(rowDrag.lastScenePosition)
                                return
                            }
                            const keys = Library.selection.join(",")
                            // A sequence goes onto the timeline too, nested there (ProjectFile.dropMedia).
                            dragProxy.Drag.mimeData = { "application/x-wabcut-library": keys, "application/x-wabcut-media": cell.key }
                            dragProxy.Drag.active = true
                            return
                        }
                        if (cell.kind === "bin")
                            root.binDragFinished(rowDrag.lastScenePosition)
                        else
                            dragProxy.Drag.active = false
                    }
                    onCentroidChanged: {
                        if (!rowDrag.active || cell.kind !== "bin")
                            return
                        rowDrag.lastScenePosition = rowDrag.centroid.scenePosition
                        root.binDragMoved(rowDrag.lastScenePosition)
                    }
                }
            }
            DropArea {
                anchors.fill: parent
                enabled: cell.kind === "bin"
                keys: ["application/x-wabcut-library"]
                onDropped: drop => {
                    Library.moveItems(drop.getDataAsString("application/x-wabcut-library").split(","), cell.key)
                    drop.accept(Qt.MoveAction)
                }
                Rectangle {
                    anchors.fill: parent
                    visible: parent.containsDrag || (cell.kind === "bin" && root.dropTarget === cell.key)
                    color: "transparent"
                    border.width: Theme.selectionWidth
                    border.color: Theme.accentStroke
                }
            }
        }
    }

    // ---- Column chooser ----
    IconButton {
        id: chooserButton
        objectName: "libraryColumnsButton"
        anchors.right: parent.right
        anchors.top: parent.top
        height: header.height
        width: header.height
        compact: true
        iconName: "columns-3"
        tooltip: qsTr("Choose columns")
        onClicked: columnChooser.open()

        Popover {
            id: columnChooser
            objectName: "libraryColumnChooser"
            title: qsTr("Columns")
            x: chooserButton.width - width
            y: chooserButton.height + Theme.space(1)
            transformOrigin: Item.TopRight

            // Scrolls when the window is too short for all of it (the smallest window is 1100 x 700).
            Flickable {
                id: chooserScroll
                objectName: "libraryColumnChooserScroll"
                width: chooserColumn.implicitWidth + Theme.space(3)
                height: Math.min(chooserColumn.implicitHeight, Math.max(Theme.space(60), root.Window.height - Theme.space(32)))
                contentWidth: chooserColumn.implicitWidth
                contentHeight: chooserColumn.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar {}
                Column {
                    id: chooserColumn
                    spacing: Theme.space(1)
                    // Two columns of switches keep the list within a small window.
                    Grid {
                        columns: 2
                        columnSpacing: Theme.space(4)
                        rowSpacing: Theme.space(1)
                        Repeater {
                            model: Library.allColumns
                            delegate: Toggle {
                                required property var modelData
                                objectName: "libraryColumnToggle_" + modelData.id
                                text: modelData.title
                                checked: modelData.visible
                                enabled: modelData.id !== "name"
                                onToggled: Library.setColumnVisible(modelData.id, checked)
                            }
                        }
                    }
                    PillButton {
                        variant: "ghost"
                        compact: true
                        text: qsTr("Reset columns")
                        onClicked: Library.resetColumns()
                    }
                    // 0.7 (card 144): named layouts - which columns show, in what order and width.
                    Text {
                        text: qsTr("Saved layouts")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        font.weight: Font.Medium
                    }
                    Repeater {
                        model: Library.columnPresets
                        delegate: Row {
                            id: presetRow
                            required property string modelData
                            spacing: Theme.space(1)
                            PillButton {
                                objectName: "libraryColumnPreset_" + presetRow.modelData
                                compact: true
                                text: presetRow.modelData
                                tooltip: qsTr("Show the columns as saved in this layout")
                                onClicked: Library.applyColumnPreset(presetRow.modelData)
                            }
                            IconButton {
                                objectName: "libraryColumnPresetDelete_" + presetRow.modelData
                                compact: true
                                iconName: "trash-2"
                                tooltip: qsTr("Delete this layout")
                                onClicked: Library.deleteColumnPreset(presetRow.modelData)
                            }
                        }
                    }
                    Row {
                        spacing: Theme.space(1)
                        FieldInput {
                            id: presetName
                            objectName: "libraryColumnPresetName"
                            width: Theme.space(40)
                            placeholderText: qsTr("Layout name")
                            Accessible.name: qsTr("Name for the column layout")
                            onAccepted: saveLayout.clicked()
                        }
                        PillButton {
                            id: saveLayout
                            objectName: "libraryColumnPresetSave"
                            compact: true
                            text: qsTr("Save")
                            tooltip: qsTr("Save the columns as they are now under this name")
                            enabled: presetName.text.trim() !== ""
                            onClicked: {
                                if (Library.saveColumnPreset(presetName.text))
                                    presetName.text = ""
                            }
                        }
                    }
                }
            }
        }
    }
}
