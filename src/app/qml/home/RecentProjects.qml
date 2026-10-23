pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The Projects page (design language v3): every recent project (Home.recent, newest first) or only the starred ones
// (Home.projectEntries: a starred project stays even after it left the recent list), sorted by when it was last opened
// or by name, as a grid of cards (ProjectGrid) or a list of rows (ProjectRow), narrowed by Home's search (`query`, by
// name). The filter, the sort and the view are remembered (Preferences "home/project*"). A project's menu, Locate...,
// Remove and its Undo run in HomeScreen (`menuRequested`, `removeRequested`); the toolbar's "..." holds Clear Unstarred
// Projects... (`clearUnstarredRequested`). Lists scroll inside the page; the page itself never does.
HomeCard {
    id: card
    objectName: "homeProjectsPage"

    /// Home's search text: only projects whose name contains it (any case).
    property string query: ""
    /// "all" or "starred" (remembered).
    property string filter: Preferences.homeProjectFilter === "starred" ? "starred" : "all"
    /// "opened" (last opened first) or "name" (remembered).
    property string sort: Preferences.homeProjectSort === "name" ? "name" : "opened"
    /// "grid" or "list" (remembered).
    property string view: Preferences.homeProjectView === "list" ? "list" : "grid"
    // Read again whenever the starred list or the recent list changes.
    readonly property var starredPaths: Preferences.homeStarred !== "" ? Preferences.starredProjects() : []
    readonly property int starredCount: card.starredPaths.length
    readonly property var source: card.filter === "starred"
                                  ? (Home.recent.length >= 0 ? Home.projectEntries(card.starredPaths) : [])
                                  : Home.recent
    readonly property var shown: {
        const q = card.query.trim().toLowerCase()
        const list = card.source.filter(p => q === "" || String(p.name).toLowerCase().includes(q))
        if (card.sort === "name")
            return list.slice().sort((a, b) => String(a.name).localeCompare(String(b.name)))
        // Newest first; a starred project that left the recent list (no date) goes last.
        return list.slice().sort((a, b) => String(b.opened ?? "").localeCompare(String(a.opened ?? "")))
    }
    readonly property bool listView: card.view === "list"

    signal openRequested(var project)
    signal menuRequested(var project, Item item, real x, real y)
    signal removeRequested(var project)
    signal clearUnstarredRequested()
    signal newProjectRequested()
    signal chooseProjectRequested()

    function setFilter(value: string) {
        card.filter = value
        Preferences.homeProjectFilter = value
    }
    function setSort(value: string) {
        card.sort = value
        Preferences.homeProjectSort = value
    }
    function setView(value: string) {
        card.view = value
        Preferences.homeProjectView = value
    }
    /// The current card or row takes the keyboard focus.
    function focusList() {
        if (card.listView)
            list.step(0)
        else
            grid.focusCurrent()
    }

    padding: Theme.space(4)

    // Filter, sort, count, grid / list, the page's menu.
    RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: false
        spacing: Theme.space(2)

        PillTabs {
            objectName: "homeProjectFilter"
            Layout.preferredWidth: Theme.space(48)
            model: [qsTr("All"), card.starredCount > 0 ? qsTr("Starred %1").arg(card.starredCount) : qsTr("Starred")]
            currentIndex: card.filter === "starred" ? 1 : 0
            Accessible.name: qsTr("Show projects")
            onActivated: index => card.setFilter(index === 1 ? "starred" : "all")
        }
        PillTabs {
            objectName: "homeProjectSort"
            Layout.preferredWidth: Theme.space(52)
            model: [qsTr("Last opened"), qsTr("Name")]
            currentIndex: card.sort === "name" ? 1 : 0
            Accessible.name: qsTr("Sort projects")
            onActivated: index => card.setSort(index === 1 ? "name" : "opened")
        }
        Item { Layout.fillWidth: true }
        Text {
            objectName: "homeProjectCount"
            text: qsTr("%n project(s)", "", card.shown.length)
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Rectangle {
            implicitWidth: viewRow.implicitWidth + Theme.space(2)
            implicitHeight: viewRow.implicitHeight + Theme.space(2)
            radius: height / 2
            color: Theme.sunken
            Row {
                id: viewRow
                anchors.centerIn: parent
                spacing: Theme.space(0.5)
                IconButton {
                    objectName: "homeGridView"
                    compact: true
                    iconName: "layout-grid"
                    iconSize: Theme.iconXs
                    checked: !card.listView
                    tooltip: qsTr("Show projects as a grid")
                    onClicked: card.setView("grid")
                }
                IconButton {
                    objectName: "homeListView"
                    compact: true
                    iconName: "list"
                    iconSize: Theme.iconXs
                    checked: card.listView
                    tooltip: qsTr("Show projects as a list")
                    onClicked: card.setView("list")
                }
            }
        }
        IconButton {
            id: moreButton
            objectName: "homeProjectsMenuButton"
            compact: true
            variant: "secondary"
            iconName: "ellipsis"
            iconSize: Theme.iconXs
            tooltip: qsTr("More project actions")
            onClicked: headerMenu.popup(moreButton, 0, moreButton.height + Theme.space(1))
        }
    }

    // The projects (or what to do when there are none).
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: Theme.space(40)

        ColumnLayout {
            objectName: "homeProjectsEmpty"
            anchors.centerIn: parent
            width: Math.min(parent.width, Theme.space(120))
            visible: card.shown.length === 0
            spacing: Theme.space(3)

            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: card.query.trim() !== "" ? qsTr("No projects match “%1”").arg(card.query.trim())
                    : card.filter === "starred" ? qsTr("No starred projects")
                    : qsTr("No projects yet")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textTitle
                font.weight: Theme.weightTitle
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: card.query.trim() !== "" ? qsTr("Try another name or filter.")
                    : card.filter === "starred" ? qsTr("Star a project to keep it here.")
                    : qsTr("Projects you open or save show up here.")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                visible: Home.recent.length === 0 && card.query.trim() === "" && card.filter === "all"
                spacing: Theme.space(2)
                PillButton {
                    objectName: "homeProjectsEmptyNew"
                    variant: "primary"
                    iconName: "plus"
                    text: qsTr("New project")
                    onClicked: card.newProjectRequested()
                }
                PillButton {
                    objectName: "homeProjectsEmptyOpen"
                    iconName: "folder-open"
                    text: qsTr("Open…")
                    tooltip: qsTr("Open a project file")
                    onClicked: card.chooseProjectRequested()
                }
            }
        }

        // ---- Grid ----
        ProjectGrid {
            id: grid
            anchors.fill: parent
            visible: !card.listView && card.shown.length > 0
            projects: card.listView ? [] : card.shown
            onOpenRequested: project => card.openRequested(project)
            onMenuRequested: (project, item, x, y) => card.menuRequested(project, item, x, y)
            onRemoveRequested: project => card.removeRequested(project)
        }

        // ---- List ----
        ColumnLayout {
            anchors.fill: parent
            visible: card.listView && card.shown.length > 0
            spacing: Theme.space(1)

            // Column labels (the rows use the same widths).
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: false
                Layout.leftMargin: Theme.space(3)
                Layout.rightMargin: Theme.space(2)
                spacing: Theme.space(3.5)
                Item { implicitWidth: Theme.space(16) }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Name")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMicro + 1
                    font.weight: Theme.weightTitle
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: Theme.microTracking
                }
                Text {
                    Layout.preferredWidth: list.openedWidth
                    text: qsTr("Opened")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMicro + 1
                    font.weight: Theme.weightTitle
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: Theme.microTracking
                }
                Text {
                    Layout.preferredWidth: list.formatWidth
                    text: qsTr("Format")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMicro + 1
                    font.weight: Theme.weightTitle
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: Theme.microTracking
                }
                Item { implicitWidth: Theme.controlHeight + Theme.controlHeightSmall + Theme.space(3.5) }
            }

            ListView {
                id: list
                objectName: "homeRecentRows"
                Layout.fillWidth: true
                Layout.fillHeight: true
                readonly property real openedWidth: list.width >= Theme.space(150) ? Theme.space(36) : Theme.space(28)
                readonly property real formatWidth: list.width >= Theme.space(150) ? Theme.space(28) : Theme.space(22)
                /// The projects the list shows (its model: given in takeRows, not bound).
                readonly property var rows: card.listView ? card.shown : []

                function step(delta: int) {
                    list.forceLayout() // a new list's rows are made at the next layout: now
                    const next = Math.max(0, Math.min(list.count - 1, list.currentIndex + delta))
                    list.currentIndex = next
                    list.positionViewAtIndex(next, ListView.Contain)
                    const item = list.itemAtIndex(next)
                    if (item)
                        item.forceActiveFocus(Qt.TabFocusReason)
                }
                /// A new list (a project removed, unstarred under Starred, put back by Undo): the rows are made again
                /// and the view sets the current one back to the first, so where the keyboard focus was is noted
                /// first; the row now at that place takes it.
                function takeRows() {
                    const hadFocus = list.activeFocus
                    const at = list.currentIndex
                    list.model = list.rows
                    if (hadFocus)
                        Qt.callLater(list.keepFocusAt, at)
                }
                /// The row at `at` takes the focus, if the focus is still in the list.
                function keepFocusAt(at: int) {
                    if (!list.activeFocus || list.count === 0)
                        return
                    list.currentIndex = Math.max(0, Math.min(list.count - 1, at))
                    list.step(0)
                }

                Component.onCompleted: list.model = list.rows
                onRowsChanged: list.takeRows()
                spacing: Theme.space(0.5)
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                keyNavigationEnabled: false
                highlightFollowsCurrentItem: false
                reuseItems: false
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Projects")
                ScrollBar.vertical: ThinScrollBar {}
                onCountChanged: {
                    if (list.currentIndex >= list.count)
                        list.currentIndex = Math.max(0, list.count - 1)
                }

                delegate: ProjectRow {
                    id: row
                    required property var modelData
                    required property int index
                    objectName: "homeRecent_" + index
                    width: list.width
                    project: modelData
                    hueName: Theme.hueOrder[index % Theme.hueOrder.length]
                    seed: index + 1
                    current: index === list.currentIndex
                    openedWidth: list.openedWidth
                    formatWidth: list.formatWidth
                    onActiveFocusChanged: {
                        if (activeFocus)
                            list.currentIndex = index
                    }
                    onOpenRequested: card.openRequested(row.modelData)
                    onRemoveRequested: card.removeRequested(row.modelData)
                    onMenuRequested: (x, y) => card.menuRequested(row.modelData, row, x, y)
                    onStepRequested: direction => list.step(direction)
                }
            }
        }
    }

    ProjectsHeaderMenu {
        id: headerMenu
        unstarredCount: Home.recent.filter(p => !card.starredPaths.includes(p.path)).length
        onChosen: action => {
            if (action === "clearUnstarred")
                card.clearUnstarredRequested()
        }
    }
}
