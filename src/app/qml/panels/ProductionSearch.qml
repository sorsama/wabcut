pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Search across all projects of the production (0.7, card 094; the Production panel's search): one field, a type filter
// (All / Sequences / Video / Audio / Stills) and a "search in" choice (everything, the name, the comment, the file path,
// ...). Typing searches after a short pause (Enter at once); the projects are read on a worker thread (Production.searching,
// a progress bar and Stop), a project open in this session as it is in memory, a file that did not change not at all.
// The results (Production.searchResults, best first) show the item's name, the project it is in, its bins and what the
// words were found in; each row has Import (a copy comes into the project being edited, one undo step), Browse (the
// project open read-only next to this one, the item pointed at) and Open (edit the project). Enter or a double click on a
// row browses it; Down from the field enters the list, Esc in the field clears it.
// While a search is typed or a type is chosen the results replace the production's project list (`active`).
Item {
    id: root

    objectName: "productionSearch"

    /// Edit the project of a result.
    signal openProjectRequested(url file)

    /// The results replace the project list.
    readonly property bool active: searchField.text.trim() !== "" || root.type !== ""
    property string type: ""
    property string field: ""
    property alias text: searchField.text
    /// The height this item may take in its panel (0 = no limit): the list of results shortens (and scrolls) to fit it, so on a
    /// small panel the rows are not pushed out of view.
    property real maxHeight: 0

    /// "Search in": [{ id, name }].
    readonly property var fields: [
        { id: "", name: qsTr("Everything") },
        { id: "name", name: qsTr("Name") },
        { id: "comment", name: qsTr("Comment") },
        { id: "description", name: qsTr("Description") },
        { id: "path", name: qsTr("File path") },
        { id: "tapeName", name: qsTr("Tape name") },
        { id: "logNote", name: qsTr("Log note") },
        { id: "scene", name: qsTr("Scene") },
        { id: "marker", name: qsTr("Markers") },
        { id: "bin", name: qsTr("Bins") }
    ]
    readonly property var types: [
        { id: "", name: qsTr("All") },
        { id: "sequence", name: qsTr("Sequences") },
        { id: "video", name: qsTr("Video") },
        { id: "audio", name: qsTr("Audio") },
        { id: "still", name: qsTr("Stills") }
    ]

    function icon(kind: string): string {
        return kind === "sequence" ? "clapperboard" : kind === "audio" ? "music" : kind === "title" ? "type"
               : kind === "still" ? "image" : "film"
    }
    /// Whether the list of results is shown (collapse() hides it while a result's project is browsed; a new search shows it).
    property bool resultsShown: true
    function collapse() {
        root.resultsShown = false
    }
    function runSearch() {
        searchTimer.stop()
        root.resultsShown = true
        if (root.active)
            Production.search(searchField.text, root.field, root.type, "")
        else
            Production.clearSearch()
    }
    function focusField() {
        searchField.forceActiveFocus(Qt.TabFocusReason)
    }

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight
    height: implicitHeight

    Timer {
        id: searchTimer
        interval: 250
        onTriggered: root.runSearch()
    }
    Connections {
        target: Production
        // Another production: the results of the old one are gone.
        function onProductionChanged() {
            if (root.active)
                searchTimer.restart()
        }
    }

    Column {
        id: column
        width: parent.width
        spacing: Theme.space(2)

        FieldInput {
            id: searchField
            objectName: "productionSearchField"
            width: parent.width
            iconName: "search"
            clearOnEscape: true
            placeholderText: qsTr("Search every project of this production")
            Accessible.name: qsTr("Search every project of this production")
            onTextChanged: searchTimer.restart()
            onAccepted: root.runSearch()
            Keys.onDownPressed: event => {
                if (resultList.count > 0) {
                    resultList.currentIndex = Math.max(0, resultList.currentIndex)
                    resultList.forceActiveFocus(Qt.TabFocusReason)
                    event.accepted = true
                }
            }
        }

        Flow {
            width: parent.width
            spacing: Theme.space(2)

            // The kind of item: one pill each, the chosen one filled; they wrap on a narrow panel instead of eliding.
            Flow {
                id: typeTabs
                objectName: "productionSearchTypes"
                width: parent.width
                spacing: Theme.space(1)
                /// The chosen kind's index in `types`; setting it chooses and searches.
                property int currentIndex: 0
                onCurrentIndexChanged: {
                    root.type = root.types[typeTabs.currentIndex].id
                    root.runSearch()
                }
                Accessible.role: Accessible.Grouping
                Accessible.name: qsTr("Kind of item to find")
                Repeater {
                    model: root.types
                    delegate: PillButton {
                        id: typePill
                        required property var modelData
                        required property int index
                        objectName: "productionSearchType_" + typePill.modelData.id
                        compact: true
                        variant: typeTabs.currentIndex === typePill.index ? "primary" : "secondary"
                        text: typePill.modelData.name
                        Accessible.role: Accessible.RadioButton
                        Accessible.checkable: true
                        Accessible.checked: typeTabs.currentIndex === typePill.index
                        onClicked: typeTabs.currentIndex = typePill.index
                    }
                }
            }
            Row {
                spacing: Theme.space(2)
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Search in")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                PillButton {
                    id: fieldButton
                    objectName: "productionSearchIn"
                    compact: true
                    iconName: "chevron-down"
                    text: root.fields.find(f => f.id === root.field)?.name ?? ""
                    tooltip: qsTr("Where to look for the words")
                    onClicked: fieldMenu.popup(fieldButton, 0, fieldButton.height + Theme.space(1))
                    OptionsMenu {
                        id: fieldMenu
                        options: root.fields
                        current: root.field
                        onChosen: id => {
                            root.field = id
                            root.runSearch()
                        }
                    }
                }
            }
        }

        // What it is doing, or what it found; a thin bar while the projects are read, with Stop.
        Item {
            width: parent.width
            height: statusRow.implicitHeight
            visible: Production.searching || Production.searched

            Row {
                id: statusRow
                width: parent.width
                spacing: Theme.space(2)
                Text {
                    id: statusText
                    objectName: "productionSearchStatus"
                    width: parent.width - (stopButton.visible ? stopButton.width + Theme.space(2) : 0)
                    anchors.verticalCenter: parent.verticalCenter
                    text: Production.searching ? Production.searchStatus : Production.searchSummary
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    Accessible.role: Accessible.StaticText
                    Accessible.name: statusText.text
                }
                PillButton {
                    id: stopButton
                    objectName: "productionSearchStop"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    visible: Production.searching
                    text: qsTr("Stop")
                    onClicked: Production.cancelSearch()
                }
            }
        }
        Rectangle {
            objectName: "productionSearchProgress"
            width: parent.width
            height: Theme.space(1)
            radius: height / 2
            visible: Production.searching
            color: Theme.raised
            Rectangle {
                width: Production.searchProgress >= 0 ? parent.width * Production.searchProgress : parent.width * 0.25
                height: parent.height
                radius: height / 2
                color: Theme.accentFill
            }
        }

        ListView {
            id: resultList
            objectName: "productionSearchResults"
            width: parent.width
            height: Math.min(contentHeight, root.maxHeight > 0 ? Math.max(Theme.space(24), root.maxHeight - resultList.y) : Theme.space(80))
            visible: root.active && root.resultsShown && Production.searchResults.length > 0
            clip: true
            model: Production.searchResults
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationEnabled: true
            currentIndex: -1
            ScrollBar.vertical: ThinScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Search results")
            Keys.onReturnPressed: root.browse(resultList.currentIndex)
            Keys.onEnterPressed: root.browse(resultList.currentIndex)
            Keys.onEscapePressed: root.focusField()

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property bool selected: ListView.isCurrentItem
                objectName: "productionSearchResult_" + index
                width: resultList.width
                height: texts.implicitHeight + Theme.space(3)
                radius: Theme.radiusClip
                color: row.selected ? Theme.accentWash : rowHover.hovered ? Theme.hover : "transparent"
                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1, in project %2").arg(row.modelData.name).arg(row.modelData.projectName)
                Accessible.description: row.modelData.location !== "" ? row.modelData.location : ""
                Accessible.selected: row.selected

                Icon {
                    id: kindIcon
                    x: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    size: Theme.iconXs
                    name: root.icon(row.modelData.kind)
                    color: row.modelData.offline ? Theme.warning : Theme.textTertiary
                }
                Column {
                    id: texts
                    anchors.left: kindIcon.right
                    anchors.leftMargin: Theme.space(2)
                    anchors.right: actions.left
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space(0.5)
                    Text {
                        width: parent.width
                        text: row.modelData.name
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Text {
                        width: parent.width
                        // The project (and "editing" when it is the one being edited), the bins, what the words matched.
                        text: row.modelData.projectName + (row.modelData.current ? qsTr(" (editing)") : row.modelData.projectOpen ? qsTr(" (open)") : "")
                              + (row.modelData.location !== "" ? " · " + row.modelData.location : "")
                              + (row.modelData.meta !== "" ? " · " + row.modelData.meta : "")
                              + (row.modelData.matched.length > 0 && row.modelData.matched.join() !== qsTr("name")
                                 ? " · " + qsTr("found in %1").arg(row.modelData.matched.join(", ")) : "")
                        elide: Text.ElideRight
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                }
                Row {
                    id: actions
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space(1)
                    anchors.verticalCenter: parent.verticalCenter
                    // On hover or while the row is the current one; always reachable by the keyboard (Tab / the row's keys).
                    opacity: rowHover.hovered || row.selected || importButton.activeFocus || browseButton.activeFocus
                             || openButton.activeFocus ? 1 : 0
                    IconButton {
                        id: importButton
                        objectName: "productionSearchImport_" + row.index
                        compact: true
                        iconName: "import"
                        visible: !row.modelData.current
                        tooltip: qsTr("Import into this project")
                        onClicked: Production.importResult(row.index)
                    }
                    IconButton {
                        id: browseButton
                        objectName: "productionSearchBrowse_" + row.index
                        compact: true
                        iconName: "layers"
                        tooltip: row.modelData.current ? qsTr("Show in the Library") : qsTr("Browse the project read-only")
                        onClicked: root.browse(row.index)
                    }
                    IconButton {
                        id: openButton
                        objectName: "productionSearchOpen_" + row.index
                        compact: true
                        iconName: "film"
                        visible: !row.modelData.current
                        tooltip: qsTr("Open the project to edit")
                        onClicked: root.openProjectRequested(row.modelData.project)
                    }
                }
                HoverHandler {
                    id: rowHover
                }
                TapHandler {
                    onSingleTapped: resultList.currentIndex = row.index
                    onDoubleTapped: root.browse(row.index)
                }
                FocusRing {
                    targetRadius: row.radius
                    visible: row.selected && resultList.activeFocus
                }
            }
        }
    }

    function browse(index: int) {
        if (index >= 0)
            Production.browseResult(index)
    }
}
