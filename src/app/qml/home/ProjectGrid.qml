pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Projects as a grid of cards (ProjectCard), for the Home page's Recent and the Projects page. Cards share the width
// (at least `minCardWidth` each) and scroll inside the grid when they do not fit. One card is the grid's Tab stop (the
// current one); the arrows move between cards (Up/Down by a row), Enter opens, Delete removes, the Menu key or
// Shift+F10 opens the menu (the owner shows it: `menuRequested`). A new list (a project removed, unstarred under
// Starred, put back by Undo) makes the cards again: when the keyboard focus was in the grid, the card now at the
// current place takes it, so the keys go on working.
GridView {
    id: grid
    objectName: "homeRecentList"

    /// Home.recent entries (or projectEntries) to show.
    property var projects: []
    property real minCardWidth: Theme.space(52)
    readonly property real gap: Theme.space(3)
    readonly property int columns: Math.max(1, Math.floor((width + gap) / (minCardWidth + gap)))
    readonly property real cardWidth: Math.floor((width + gap) / columns - gap)
    /// A card's height for `cardWidth` (picture 16:10, two lines, padding).
    readonly property real cardHeight: Math.round(cardWidth * 10 / 16) + Theme.space(15)

    signal openRequested(var project)
    signal menuRequested(var project, Item item, real x, real y)
    signal removeRequested(var project)

    /// Moves the current card by `delta` and gives it the keyboard focus.
    function step(delta: int) {
        grid.forceLayout() // a new list's cards are made at the next layout: now
        const next = Math.max(0, Math.min(grid.count - 1, grid.currentIndex + delta))
        grid.currentIndex = next
        grid.positionViewAtIndex(next, GridView.Contain)
        const item = grid.itemAtIndex(next)
        if (item)
            item.forceActiveFocus(Qt.TabFocusReason)
    }
    /// The current card takes the keyboard focus.
    function focusCurrent() {
        if (grid.count === 0)
            return
        grid.currentIndex = Math.max(0, Math.min(grid.count - 1, grid.currentIndex))
        grid.step(0)
    }
    /// A new list (a project removed, unstarred under Starred, put back by Undo): the cards are made again and the view
    /// sets the current one back to the first, so where the keyboard focus was is noted first; the card now at that
    /// place takes it.
    function takeProjects() {
        const hadFocus = grid.activeFocus
        const at = grid.currentIndex
        grid.model = grid.projects
        if (hadFocus)
            Qt.callLater(grid.keepFocusAt, at)
    }
    /// The card at `at` takes the focus, if the focus is still in the grid.
    function keepFocusAt(at: int) {
        if (!grid.activeFocus || grid.count === 0)
            return
        grid.currentIndex = Math.max(0, Math.min(grid.count - 1, at))
        grid.step(0)
    }

    // The model is `projects`, given here and not bound (takeProjects notes the focus before the cards go).
    Component.onCompleted: grid.model = grid.projects
    onProjectsChanged: grid.takeProjects()
    cellWidth: (grid.width + grid.gap) / grid.columns
    cellHeight: grid.cardHeight + grid.gap
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    interactive: grid.contentHeight > grid.height
    keyNavigationEnabled: false
    highlightFollowsCurrentItem: false
    currentIndex: 0
    reuseItems: false
    activeFocusOnTab: false
    Accessible.role: Accessible.List
    Accessible.name: qsTr("Projects")
    ScrollBar.vertical: ThinScrollBar {}
    onCountChanged: {
        if (grid.currentIndex >= grid.count)
            grid.currentIndex = Math.max(0, grid.count - 1)
    }

    delegate: ProjectCard {
        id: card
        required property var modelData
        required property int index
        objectName: "homeRecent_" + index
        width: grid.cardWidth
        height: grid.cardHeight
        project: modelData
        hueName: Theme.hueOrder[index % Theme.hueOrder.length]
        seed: index + 1
        current: index === grid.currentIndex
        onActiveFocusChanged: {
            if (activeFocus)
                grid.currentIndex = index
        }
        onOpenRequested: grid.openRequested(card.modelData)
        onRemoveRequested: grid.removeRequested(card.modelData)
        onMenuRequested: (x, y) => grid.menuRequested(card.modelData, card, x, y)
        onStepRequested: direction => grid.step(direction === -2 ? -grid.columns : direction === 2 ? grid.columns : direction)
    }
}
