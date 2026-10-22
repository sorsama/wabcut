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
//   * drag rows onto a bin row, a crumb or a bin tab to move them; media rows drag onto the timeline too.
FocusScope {
    id: root
    objectName: "libraryList"
    activeFocusOnTab: true

    signal activated(string key, string kind)
    signal menuRequested(string key, Item target, point position)

    readonly property var columns: Library.columns
    property string editingKey: ""
    property string editingColumn: ""

    function startEdit(key, column) {
        root.editingKey = key
        root.editingColumn = column
    }
    function finishEdit(value, commit) {
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
        table.forceActiveFocus()
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
    // Selects the row `delta` rows from the current one (keyboard).
    function step(delta, extend) {
        const keys = Library.selection
        const from = keys.length > 0 ? Library.items.rowOf(keys[keys.length - 1]) : -1
        const to = Math.max(0, Math.min(table.rows - 1, from + delta))
        if (to < 0 || table.rows === 0)
            return
        const key = Library.items.keyOf(to)
        Library.select(key, extend ? Library.Range : Library.Replace)
        table.positionViewAtRow(to, TableView.Contain)
    }

    Keys.onPressed: event => {
        const extend = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Down) {
            root.step(1, extend)
            event.accepted = true
        } else if (event.key === Qt.Key_Up) {
            root.step(-1, extend)
            event.accepted = true
        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && Library.selection.length > 0) {
            const key = Library.selection[0]
            root.activated(key, Library.details(key).kind ?? "")
            event.accepted = true
        } else if (event.key === Qt.Key_F2 && Library.selection.length === 1) {
            root.startEdit(Library.selection[0], "name")
            event.accepted = true
        }
    }
    // The arrows, Return and F2 belong to the list while it has focus (Delete stays project.delete).
    Keys.onShortcutOverride: event => event.accepted = (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) === 0
                                                        && [Qt.Key_Up, Qt.Key_Down, Qt.Key_Return, Qt.Key_Enter, Qt.Key_F2].includes(event.key)

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
        rowHeightProvider: () => Theme.rowHeight
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

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: event => {
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

            readonly property bool editing: root.editingKey === key && root.editingColumn === columnId
            readonly property bool isName: columnId === "name"
            readonly property bool isGood: columnId === "good"

            implicitWidth: 120
            implicitHeight: Theme.rowHeight
            color: selected ? Theme.accentWash : (row % 2 === 1 ? Theme.sunken : "transparent")

            Accessible.role: Accessible.Cell
            Accessible.name: display

            // Name: the kind's icon; Label: the colour.
            Icon {
                id: kindIcon
                visible: cell.isName
                x: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                size: Theme.iconXs
                name: cell.searchBin ? "folder-search" : cell.kind === "bin" ? "folder" : cell.kind === "sequence" ? "film"
                      : cell.kind === "audio" ? "music" : cell.kind === "title" ? "type"
                      : cell.kind === "subclip" ? "scissors" : cell.kind === "still" ? "image" : "film"
                color: cell.kind === "bin" ? Theme.accentStroke : Theme.textTertiary
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
            Text {
                anchors.left: cell.isName ? kindIcon.right : labelChip.visible ? labelChip.right : parent.left
                anchors.leftMargin: Theme.space(2)
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                visible: !cell.editing && !cell.isGood
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
                    objectName: "libraryCellEditor"
                    text: cell.isName ? cell.display : cell.display
                    Accessible.name: cell.isName ? qsTr("Name") : qsTr("Edit %1").arg((root.columns.find(c => c.id === cell.columnId) ?? {}).title ?? "")
                    font.pixelSize: Theme.textSm
                    Component.onCompleted: {
                        forceActiveFocus()
                        selectAll()
                    }
                    onAccepted: root.finishEdit(text, true)
                    Keys.onEscapePressed: event => {
                        root.finishEdit(text, false)
                        event.accepted = true
                    }
                    onActiveFocusChanged: if (!activeFocus && cell.editing) root.finishEdit(text, true)
                    Keys.onShortcutOverride: event => event.accepted = true
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
                    target: null
                    enabled: !cell.editing && !cell.offline
                    // The drag's data is made when it starts, after the row is selected (no cell binds the selection).
                    onActiveChanged: {
                        if (active) {
                            if (!cell.selected)
                                Library.select(cell.key, Library.Replace)
                            const keys = Library.selection.join(",")
                            // A sequence goes onto the timeline too, nested there (ProjectFile.dropMedia).
                            dragProxy.Drag.mimeData = cell.kind === "bin"
                                                      ? { "application/x-wabcut-library": keys }
                                                      : { "application/x-wabcut-library": keys,
                                                          "application/x-wabcut-media": cell.key }
                        }
                        dragProxy.Drag.active = active
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
                    visible: parent.containsDrag
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

            Column {
                spacing: Theme.space(1)
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
                PillButton {
                    variant: "ghost"
                    compact: true
                    text: qsTr("Reset columns")
                    onClicked: Library.resetColumns()
                }
            }
        }
    }
}
