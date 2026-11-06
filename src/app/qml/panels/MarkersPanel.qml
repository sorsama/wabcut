pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

// ● MARKERS (M5i, Markers): the sequence's markers or the selected clip's (clip markers live on its media and show on
// every clip of it), filtered by Premiere's eight colours and a search over names and comments. Click (or Enter) a row
// to move the playhead there, double-click to edit it in place, Delete removes it. The header adds a marker at the
// playhead and exports the sequence's markers as CSV or YouTube chapters. 0.6: a right click (or the Menu key) on a row
// opens the marker menu (MarkerContextMenu: Edit Marker…, Colour, Type, Go to Marker, Delete Marker).
// 0.7 (card 119): several markers at once. Ctrl-click a row to add it to the selection (or take it out), Shift-click to
// select the rows between, Ctrl+A for every listed row; Delete, Ctrl+X, Ctrl+C and Ctrl+V (paste at the playhead) act on
// the selection, and Alt+Left / Right (Shift: ten frames) moves it, each one undo step. A bar under the colour filters
// shows how many are selected with the same actions as buttons. The ruler's flags select and drag the same group.
Panel {
    id: root

    objectName: "markersPanel"
    title: qsTr("Markers")

    readonly property var colorNames: [qsTr("Green"), qsTr("Red"), qsTr("Purple"), qsTr("Orange"), qsTr("Yellow"),
                                       qsTr("White"), qsTr("Blue"), qsTr("Cyan")]
    readonly property var typeLabels: ({ chapter: qsTr("Chapter"), segmentation: qsTr("Segmentation"),
                                          webLink: qsTr("Web link"), source: qsTr("Source"), comment: "" })

    /// 0.6: the marker menu for row `row` (a right click at `point` in it, or the Menu key).
    property point menuPoint
    function openMarkerMenu(row: Item, point: point, index: int) {
        root.menuPoint = row.mapToItem(root, row.width / 2, row.height)
        markerMenu.openAt(row, point, index)
    }

    /// 0.7 (card 119): the group keys of the list. True when the key was one of them (Ctrl+A, Ctrl+X / C / V, Escape,
    /// Alt+Left / Right); Delete is the row's own handler.
    function groupKey(event: var): bool {
        if (event.matches(StandardKey.SelectAll)) {
            Markers.selectAll()
            return true
        }
        if (event.matches(StandardKey.Copy)) {
            Markers.copySelected()
            return true
        }
        if (event.matches(StandardKey.Cut)) {
            Markers.cutSelected()
            return true
        }
        if (event.matches(StandardKey.Paste)) {
            Markers.paste()
            return true
        }
        if (event.key === Qt.Key_Escape && Markers.selectionCount > 0) {
            Markers.clearSelection()
            return true
        }
        if ((event.modifiers & Qt.AltModifier) && (event.key === Qt.Key_Left || event.key === Qt.Key_Right)
                && Markers.selectionCount > 0) {
            Markers.nudgeSelected((event.key === Qt.Key_Left ? -1 : 1) * ((event.modifiers & Qt.ShiftModifier) ? 10 : 1))
            return true
        }
        return false
    }

    // Focus target for Window > Markers.
    function focusContent() {
        search.forceActiveFocus(Qt.TabFocusReason)
    }
    /// P1 round (#30, stream 1E): Edit > Find (Ctrl+F) while this panel is active: the search, its text selected.
    function focusFind() {
        search.forceActiveFocus(Qt.ShortcutFocusReason)
        search.selectAll()
    }

    leading: [
        IconButton {
            objectName: "markersAddButton"
            compact: true
            iconName: "bookmark"
            tooltip: qsTr("Add marker at the playhead")
            command: "marker.add"
            onClicked: {
                if (Markers.scope === "clip" && ProjectController.selectedClips.length > 0)
                    Markers.addClipMarker(ProjectController.selectedClips[0])
                else
                    Markers.add(false)
            }
        },
        IconButton {
            objectName: "markersExportButton"
            compact: true
            iconName: "file-down"
            tooltip: qsTr("Export the sequence's markers")
            onClicked: exportMenu.popup()
        },
        // 0.7: read a marker list from a spreadsheet's CSV, a text file (YouTube chapters) or JSON.
        IconButton {
            objectName: "markersImportButton"
            compact: true
            iconName: "import"
            tooltip: Markers.scope === "clip" ? qsTr("Import markers into the clip's media") : qsTr("Import markers into the sequence")
            onClicked: importMenu.popup()
        }
    ]

    MarkersImportMenu {
        id: importMenu
        onImportRequested: (atPlayhead, replace) => {
            importDialog.atPlayhead = atPlayhead
            importDialog.replace = replace
            importDialog.open()
        }
    }

    FileDialog {
        id: importDialog
        objectName: "markersImportDialog"
        property bool atPlayhead: false
        property bool replace: false
        title: qsTr("Import markers")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Marker lists (*.csv *.tsv *.txt *.json)"), qsTr("All files (*)")]
        onAccepted: Markers.importFromFile(selectedFile, atPlayhead, replace)
    }

    MarkersExportMenu {
        id: exportMenu
        onExportRequested: format => {
            exportDialog.format = format
            exportDialog.nameFilters = format === "csv" ? [qsTr("CSV files (*.csv)")] : [qsTr("Text files (*.txt)")]
            exportDialog.defaultSuffix = format === "csv" ? "csv" : "txt"
            exportDialog.open()
        }
    }

    FileDialog {
        id: exportDialog
        property string format: "csv"
        title: qsTr("Export markers")
        fileMode: FileDialog.SaveFile
        // P1 (#24, 1C): the menu's From In to Out; YouTube chapters use chapter markers only.
        onAccepted: Markers.exportToFile(format, selectedFile, exportMenu.range, format === "youtube")
    }

    Column {
        id: controls
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Theme.space(2)

        PillTabs {
            objectName: "markersScopeTabs"
            width: parent.width
            tabs: true
            model: [{ label: qsTr("Sequence") }, { label: qsTr("Clip") }]
            currentIndex: Markers.scope === "clip" ? 1 : 0
            onActivated: index => Markers.scope = index === 1 ? "clip" : "sequence"
            Accessible.name: qsTr("Markers of")
        }
        FieldInput {
            id: search
            objectName: "markersSearch"
            width: parent.width
            iconName: "search"
            clearOnEscape: true
            placeholderText: qsTr("Search markers")
            text: Markers.search
            onTextChanged: Markers.search = text
            Accessible.name: qsTr("Search markers")
        }
        Row {
            spacing: Theme.space(1)
            Repeater {
                model: 8
                delegate: Swatch {
                    required property int index
                    objectName: "markerFilter_" + index
                    swatchColor: Theme.markerColor(index)
                    tooltip: qsTr("Show %1 markers").arg(root.colorNames[index])
                    checked: (Markers.colorMask & (1 << index)) !== 0
                    onClicked: {
                        checked = Qt.binding(() => (Markers.colorMask & (1 << index)) !== 0)
                        Markers.colorMask = Markers.colorMask ^ (1 << index)
                    }
                }
            }
        }
        // 0.7 (card 119): what is selected, and what can be done with it.
        Flow {
            id: groupBar
            objectName: "markersGroupBar"
            width: parent.width
            spacing: Theme.space(1)
            visible: Markers.selectionCount > 0 || Markers.clipboardCount > 0

            Text {
                objectName: "markersSelectionCount"
                visible: Markers.selectionCount > 0
                height: Theme.controlHeightSmall
                verticalAlignment: Text.AlignVCenter
                text: qsTr("%n marker(s) selected", "", Markers.selectionCount)
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            IconButton {
                objectName: "markersCopyButton"
                compact: true
                iconName: "copy"
                tooltip: qsTr("Copy the selected markers")
                enabled: Markers.selectionCount > 0
                onClicked: Markers.copySelected()
            }
            IconButton {
                objectName: "markersCutButton"
                compact: true
                iconName: "scissors"
                tooltip: qsTr("Cut the selected markers")
                enabled: Markers.selectionCount > 0
                onClicked: Markers.cutSelected()
            }
            IconButton {
                objectName: "markersPasteButton"
                compact: true
                iconName: "clipboard-paste"
                tooltip: qsTr("Paste the copied markers at the playhead")
                enabled: Markers.clipboardCount > 0
                onClicked: Markers.paste()
            }
            IconButton {
                objectName: "markersDeleteSelectedButton"
                compact: true
                iconName: "trash-2"
                tooltip: qsTr("Delete the selected markers")
                enabled: Markers.selectionCount > 0
                onClicked: Markers.removeSelected()
            }
            IconButton {
                objectName: "markersClearSelectionButton"
                compact: true
                iconName: "x"
                tooltip: qsTr("Clear the selection")
                enabled: Markers.selectionCount > 0
                onClicked: Markers.clearSelection()
            }
        }
        Text {
            visible: Markers.scope === "clip"
            text: Markers.clipName !== "" ? qsTr("Clip: %1").arg(Markers.clipName) : qsTr("Select a clip to see its markers")
            color: Theme.textTertiary
            elide: Text.ElideRight
            width: parent.width
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    ListView {
        id: list
        objectName: "markersList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: controls.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: Theme.space(2)
        clip: true
        spacing: Theme.space(1)
        model: Markers.items
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        delegate: AbstractButton {
            id: row

            required property var modelData
            required property int index

            /// 0.7 (card 119): part of the selection (Markers.selection lists the selected indices).
            readonly property bool selected: Markers.selection.indexOf(modelData.index) >= 0
            /// The modifier keys held at the press that is being clicked (0 for a click from the keyboard).
            property int pressModifiers: 0

            objectName: "markerRow_" + modelData.index
            width: list.width
            height: Theme.controlHeight + Theme.space(4)
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            // A plain click selects the row and moves the playhead to it; Ctrl-click adds it to the selection (or takes it
            // out) and Shift-click selects the rows between, neither moving the playhead.
            onClicked: {
                const mods = row.pressModifiers
                row.pressModifiers = 0
                if (mods & Qt.ControlModifier)
                    Markers.select(modelData.index, "toggle")
                else if (mods & Qt.ShiftModifier)
                    Markers.select(modelData.index, "extend")
                else {
                    Markers.select(modelData.index)
                    Markers.seek(modelData.index)
                }
            }
            onDoubleClicked: editor.openFor(modelData.index, row, Qt.point(row.width / 2, row.height))
            // Delete removes the whole selection when this row is part of it, else just this row.
            Keys.onDeletePressed: {
                if (row.selected && Markers.selectionCount > 1)
                    Markers.removeSelected()
                else
                    Markers.remove(modelData.index)
            }
            Keys.onReturnPressed: Markers.seek(modelData.index)
            Keys.onShortcutOverride: event => {
                // The group keys are this list's while it has the focus, not the keymap's (the timeline's Copy / Paste /
                // Select All / Clear): the keymap lets a key stay with the focused item that claims it here.
                if (event.matches(StandardKey.SelectAll) || event.matches(StandardKey.Copy) || event.matches(StandardKey.Cut)
                        || event.matches(StandardKey.Paste) || event.key === Qt.Key_Delete
                        || (Markers.selectionCount > 0 && (event.key === Qt.Key_Escape
                            || ((event.modifiers & Qt.AltModifier) && (event.key === Qt.Key_Left || event.key === Qt.Key_Right)))))
                    event.accepted = true
            }
            // 0.6: the marker menu from the keyboard (the Menu key or Shift+F10).
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                    root.openMarkerMenu(row, Qt.point(Theme.space(4), row.height), modelData.index)
                    event.accepted = true
                } else if (root.groupKey(event)) {
                    event.accepted = true
                }
            }
            Accessible.role: Accessible.ListItem
            Accessible.name: row.selected ? qsTr("Marker %1 at %2, selected").arg(modelData.name).arg(modelData.inText)
                                          : qsTr("Marker %1 at %2").arg(modelData.name).arg(modelData.inText)

            contentItem: Item {
                Rectangle {
                    id: chip
                    x: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.space(1)
                    height: parent.height - Theme.space(2)
                    radius: Theme.radiusBadge / 2
                    color: Theme.markerColor(row.modelData.colorIndex)
                }
                Column {
                    anchors.left: chip.right
                    anchors.leftMargin: Theme.space(3)
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space(0.5)
                    Row {
                        width: parent.width
                        spacing: Theme.space(2)
                        Text {
                            width: Math.min(implicitWidth, parent.width - typeBadge.width - Theme.space(2))
                            text: row.modelData.name !== "" ? row.modelData.name : qsTr("Marker")
                            elide: Text.ElideRight
                            color: row.modelData.name !== "" ? Theme.textPrimary : Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                        }
                        Rectangle {
                            id: typeBadge
                            anchors.verticalCenter: parent.verticalCenter
                            visible: (root.typeLabels[row.modelData.type] ?? "") !== ""
                            width: visible ? typeText.implicitWidth + Theme.space(2) : 0
                            height: typeText.implicitHeight + Theme.space(0.5)
                            radius: Theme.radiusBadge
                            color: Theme.sunken
                            border.width: Theme.hairline
                            border.color: Theme.border
                            Text {
                                id: typeText
                                anchors.centerIn: parent
                                text: root.typeLabels[row.modelData.type] ?? ""
                                color: Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                            }
                        }
                    }
                    Row {
                        width: parent.width
                        spacing: Theme.space(2)
                        Text {
                            id: times
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.modelData.ranged ? qsTr("%1 · %2").arg(row.modelData.inText).arg(row.modelData.durationText)
                                                       : row.modelData.inText
                            color: row.modelData.atPlayhead ? Theme.accentStroke : Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: text !== ""
                            width: parent.width - times.width - Theme.space(2)
                            text: row.modelData.type === "webLink" && row.modelData.url !== "" ? row.modelData.url : row.modelData.comment
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                }
            }

            background: Rectangle {
                radius: Theme.radiusField
                color: row.down ? Theme.raised : row.selected ? Theme.accentWash : row.hovered ? Theme.hover
                                                 : row.modelData.atPlayhead ? Theme.sunken : "transparent"
                border.width: row.selected ? Theme.selectionWidth : 0
                border.color: Theme.accentStroke
                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                FocusRing {
                    anchors.margins: 0
                    targetRadius: Theme.radiusField
                    visible: row.visualFocus
                }
            }

            HoverHandler {
                enabled: row.enabled
                cursorShape: Qt.PointingHandCursor
            }
            // Only reads which modifier keys the press carries (Ctrl adds to the selection, Shift extends it); the press goes
            // on to the button.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: mouse => {
                    row.pressModifiers = mouse.modifiers
                    mouse.accepted = false
                }
            }
            // 0.6: a right click opens the marker menu.
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: eventPoint => root.openMarkerMenu(row, eventPoint.position, row.modelData.index)
            }
        }
    }

    EmptyState {
        objectName: "markersEmptyState"
        anchors.centerIn: list
        width: list.width - Theme.space(8)
        visible: list.count === 0
        illustration: Markers.totalCount > 0 ? "search" : "markers"
        //: %1 = the Add Marker shortcut
        text: Markers.totalCount > 0 ? qsTr("No markers match the search or the colours")
                                     : qsTr("No markers yet. Press %1 to add one at the playhead.")
                                           .arg((KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText("marker.add") : "") || "M")
    }

    MarkerEditor {
        id: editor
        parent: root
    }
    // 0.6: the rows' right-click menu; Edit Marker… opens the editor under the row once the menu has closed.
    MarkerContextMenu {
        id: markerMenu
        onEditRequested: index => editor.openFor(index, root, root.menuPoint)
    }
}
