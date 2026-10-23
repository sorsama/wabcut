pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Home's first page (design language v3), two rows that fit the window without scrolling: Continue (the last project,
// large) beside Start (New project, Open..., Import..., the drop well), then Recent (the latest projects as cards, the
// same cards as the Projects page) with All projects. Recent shows two rows of cards when there are enough projects and
// room, else one; the top row takes the room Recent leaves (up to 110 units); on short windows Start's drop well gives
// up some height and the cards get smaller, so a whole row fits with room below it. With no projects yet both islands
// say so in one quiet line (Recent points to Learn: the sample project and the guides live there).
Item {
    id: page
    objectName: "homePage"

    property bool revealed: true
    property bool dragActive: false
    property string newKeys: ""
    property string openKeys: ""
    property string importKeys: ""
    /// The Start island's width.
    readonly property real startWidth: page.width >= Theme.space(300) ? Theme.space(110) : Theme.space(88)
    /// The least height Start takes (its drop well at its smallest).
    readonly property real startMinHeight: start.implicitHeight - start.compressible
    // ---- Recent's size (from the page's size, not from Recent's own layout) ----
    readonly property real cardGap: Theme.space(3)
    readonly property real gridWidth: page.width - Theme.space(4) * 2
    /// Recent's padding, title row and spacing, and a little room under the cards (the grid never scrolls by a pixel).
    readonly property real recentChrome: Theme.space(4) * 2 + Theme.controlHeightSmall + Theme.space(3) + Theme.space(2)
    readonly property real roomForRecent: page.height - Theme.gap - page.startMinHeight - page.recentChrome
    /// The cards' minimum width: the usual one, or smaller so a whole row fits a short window.
    readonly property real cardMinWidth: page.cellHeightFor(Theme.space(52)) <= page.roomForRecent ? Theme.space(52)
        : Math.max(Theme.space(36), Math.min(Theme.space(52),
                                             Math.floor((page.roomForRecent - Theme.space(15) - page.cardGap) * 1.6 * 0.85)))
    readonly property int cardColumns: Math.max(1, Math.floor((page.gridWidth + page.cardGap) / (page.cardMinWidth + page.cardGap)))
    readonly property real cellHeight: page.cellHeightFor(page.cardMinWidth)
    /// Two rows when there are projects for them and the top row keeps a good size; else one.
    readonly property int recentRows: Home.recent.length > page.cardColumns
                                      && page.height - Theme.gap - page.recentChrome - 2 * page.cellHeight
                                         >= Math.max(start.implicitHeight, page.height * 0.42) ? 2 : 1
    readonly property real recentHeight: Home.recent.length === 0 ? page.recentChrome + Theme.space(16)
                                                                  : page.recentChrome + page.recentRows * page.cellHeight
    /// The top row's height: the room Recent leaves, at most 110 units (Recent keeps the rest), at least what Start needs
    /// (its drop well smaller on a short window).
    readonly property real topHeight: Math.max(page.startMinHeight,
                                               Math.min(Theme.space(110), page.height - Theme.gap - page.recentHeight))

    /// A grid cell's height for cards at least `minWidth` wide (ProjectGrid's sizes).
    function cellHeightFor(minWidth: real): real {
        const columns = Math.max(1, Math.floor((page.gridWidth + page.cardGap) / (minWidth + page.cardGap)))
        const cardWidth = Math.floor((page.gridWidth + page.cardGap) / columns - page.cardGap)
        return Math.round(cardWidth * 10 / 16) + Theme.space(15) + page.cardGap
    }

    signal openRequested(url file)
    signal returnRequested()
    signal revealRequested(url file)
    signal newProjectRequested()
    signal chooseProjectRequested()
    signal importRequested()
    signal importFolderRequested()
    signal allProjectsRequested()
    signal learnRequested()
    signal projectOpenRequested(var project)
    signal projectMenuRequested(var project, Item item, real x, real y)
    signal projectRemoveRequested(var project)

    /// The first recent card takes the keyboard focus (or the first Start action when there is none).
    function focusFirst() {
        if (recentGrid.count > 0)
            recentGrid.focusCurrent()
        else
            start.focusFirst()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.gap

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: page.topHeight
            Layout.minimumHeight: page.startMinHeight
            spacing: Theme.gap

            ContinueCard {
                id: continueCard
                Layout.fillWidth: true
                Layout.fillHeight: true
                order: 0
                revealed: page.revealed
                onOpenRequested: file => page.openRequested(file)
                onReturnRequested: page.returnRequested()
                onRevealRequested: file => page.revealRequested(file)
                onChooseProjectRequested: page.chooseProjectRequested()
            }
            StartCard {
                id: start
                Layout.preferredWidth: page.startWidth
                Layout.fillHeight: true
                order: 1
                revealed: page.revealed
                dragActive: page.dragActive
                newKeys: page.newKeys
                openKeys: page.openKeys
                importKeys: page.importKeys
                onNewProjectRequested: page.newProjectRequested()
                onOpenRequested: page.chooseProjectRequested()
                onImportRequested: page.importRequested()
                onImportFolderRequested: page.importFolderRequested()
            }
        }

        HomeCard {
            id: recent
            objectName: "homeRecentCard"
            Layout.fillWidth: true
            Layout.fillHeight: true
            order: 2
            revealed: page.revealed
            title: qsTr("Recent")
            hue: "blue"
            trailing: [
                PillButton {
                    objectName: "homeAllProjects"
                    compact: true
                    variant: "ghost"
                    iconName: "chevron-right"
                    LayoutMirroring.enabled: true // label first, chevron last
                    LayoutMirroring.childrenInherit: true
                    text: Home.recent.length > 0 ? qsTr("All projects (%1)").arg(Home.recent.length) : qsTr("All projects")
                    tooltip: qsTr("Show every project, starred ones and the list view")
                    onClicked: page.allProjectsRequested()
                }
            ]

            ProjectGrid {
                id: recentGrid
                objectName: "homeRecentCards"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: Home.recent.length > 0
                minCardWidth: page.cardMinWidth
                projects: Home.recent.slice(0, recentGrid.columns * page.recentRows)
                onOpenRequested: project => page.projectOpenRequested(project)
                onMenuRequested: (project, item, x, y) => page.projectMenuRequested(project, item, x, y)
                onRemoveRequested: project => page.projectRemoveRequested(project)
            }

            // No recent projects: one quiet line, and where the sample project and the guides are (Learn).
            ColumnLayout {
                objectName: "homeRecentEmpty"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: Home.recent.length === 0
                spacing: Theme.space(1)

                Item { Layout.fillHeight: true }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Projects you open or save show up here.")
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                HomeLink {
                    objectName: "homeRecentLearn"
                    Layout.alignment: Qt.AlignHCenter
                    iconName: "book-open"
                    text: qsTr("New to Wabcut? Start in Learn")
                    tooltip: qsTr("The sample project, the tour and the guides")
                    onClicked: page.learnRequested()
                }
                Item { Layout.fillHeight: true }
            }
        }
    }
}
