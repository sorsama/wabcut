import QtQuick
import QtQuick.Window

// A panel group's place in the workbench (0.1.3 panel docking): Main.qml makes one for each group of the current
// workspace's layout (DockManager), lays it out where the layout says, and puts the group's panels into it; with
// several panels it shows their tabs (DockTabs), laid over the shown panel's tab strip (design language v3: the panel
// reserves `tabsWidth` for them). Its grip (DockHandle, in the shown panel's header) or a tab dragged
// onto another group joins it or docks beside it; the slot pops out into a window of its own (FloatingDock), which
// docks it back when it closes. The slot's children live in its holder: that is what moves into the floating window
// and back.
// UX rework (Premiere's frames): a press anywhere in the group makes it the active group (DockManager.activeGroup),
// which wears a 2 px accent outline; a tab's right-click opens the tab menu (DockTabMenu), its double-click maximizes
// the group.
// 0.4: the tabs may take the whole strip but the handle's and the More tools button's room (`toolsReserve`); a crowded
// group shrinks them to readable widths, then lists the rest behind its tab list button (DockTabs).
Item {
    id: slot

    /// The group's id in the layout ("g1", ...).
    required property string dockKey
    /// The panel the group shows (a Workspaces.catalog key): its header's close button closes it.
    property string currentPanel: ""
    /// The group's panels in tab order (Main.qml follows the layout; a tab being dragged stays put meanwhile).
    property var tabPanels: []
    /// Several panels: the group's tabs show over the strip, and the shown panel leaves them `tabsWidth` of it.
    readonly property bool tabbed: tabPanels.length > 1
    readonly property real tabsWidth: tabbed ? tabs.width : 0
    /// 0.4: the strip's room the tabs leave at its right end: the dock's handle (grip, pop-out, close), the shown
    /// panel's More tools button and their gaps.
    readonly property real toolsReserve: Theme.iconSm + Theme.space(2) + 3 * Theme.controlHeightSmall + Theme.space(6)
    /// 0.4: the panels' focus keys (Premiere's Window shortcuts) for the tabs' tooltips and the tab list.
    readonly property var panelCommands: tabs.panelCommands
    /// The window title while it floats.
    property string title: ""
    property DockManager manager: null
    /// Something to show: a floating window hides while this is false.
    property bool active: true
    /// The window it floats in (a FloatingDock), else null.
    property Window floatWindow: null
    readonly property bool floating: floatWindow !== null
    /// The active group (the Window menu opens closed panels here; Shift+` maximizes it; Ctrl+W closes its panel).
    readonly property bool highlighted: manager !== null && manager.activeGroup === dockKey
    /// The group takes the whole work area (Maximize Frame).
    readonly property bool maximized: manager !== null && manager.maximizedGroup === dockKey
    /// The mouse pointer is over the group (` maximizes the group under it).
    readonly property bool pointerInside: glassHover.hovered
    /// The tab menu (a DockTabMenu), once a tab's right-click made it; null before.
    readonly property QtObject tabMenu: tabMenuLoader.item
    default property alias content: holder.data
    readonly property alias holder: holder

    /// The grip starts to drag the whole group; a tab (`panel`) drags just that panel.
    function beginDrag() {
        manager?.beginDrag(slot, "")
    }
    function beginPanelDrag(panel: string) {
        manager?.beginDrag(slot, panel)
    }
    /// The grip moved to `globalPoint` (screen coordinates) with `modifiers` held (Ctrl: a window of its own).
    function moveDrag(globalPoint: point, modifiers: int) {
        manager?.moveDrag(slot, globalPoint, modifiers)
    }
    function endDrag() {
        manager?.endDrag(slot)
    }
    function popOut() {
        manager?.popOut(slot)
    }
    function dockBack() {
        manager?.dockBack(slot, true)
    }
    /// Closes the panel the group shows.
    function closeCurrent() {
        if (slot.currentPanel !== "")
            manager?.closePanel(slot.currentPanel)
    }
    /// Maximize / Restore Frame for this group (a double-click on one of its tabs).
    function toggleMaximize() {
        manager?.toggleMaximize(slot.dockKey)
    }
    /// The tab menu (DockTabMenu) for the panel `panel`, at (`x`, `y`) in `item` (its tab).
    function openTabMenu(panel: string, item: Item, x: real, y: real) {
        if (!slot.manager)
            return
        slot.manager.setActiveGroup(slot.dockKey)
        tabMenuLoader.active = true
        const menu = tabMenuLoader.item as DockTabMenu
        if (!menu)
            return
        menu.panel = panel
        menu.popup(item, x, y)
    }

    DockHolder {
        id: holder
        objectName: "dockHolder_" + slot.dockKey
        dockSlot: slot
        anchors.fill: parent
        // In its own window the panel keeps the workbench's gutter around it.
        anchors.margins: slot.floating ? Theme.gap : 0

        // The group's tabs, over the shown panel's strip (its left part; the panel's tools follow them). In the holder,
        // so they float with the group; above the panels, which Main adds later.
        DockTabs {
            id: tabs
            objectName: "dockTabs_" + slot.dockKey
            host: slot
            z: 10
            visible: slot.tabbed
            x: Theme.space(3)
            y: 0
            height: Theme.tabStripHeight
            // As wide as its tabs, at most the whole strip but the room of the handle and the More tools button (0.4: no
            // 62 % cap; the tabs shrink to readable widths, then the rest go into the tab list).
            width: slot.tabbed ? Math.min(naturalWidth, Math.max(Theme.space(24), holder.width - Theme.space(3) - slot.toolsReserve))
                               : 0
            panels: slot.tabPanels
            current: slot.currentPanel
            onActivated: key => {
                slot.manager?.activate(key)
                slot.manager?.setActiveGroup(slot.dockKey)
            }
            onCloseRequested: key => slot.manager?.closePanel(key)
        }

        // The active group's outline (Premiere's highlighted frame): 2 px of the accent along the island's edge, over
        // its panels. Keyboard focus rings stay the white (black on light) rings inside.
        Rectangle {
            objectName: "dockActiveOutline_" + slot.dockKey
            anchors.fill: parent
            z: 90
            visible: slot.highlighted
            radius: Theme.radiusPanel
            color: "transparent"
            border.width: Theme.focusRingWidth
            border.color: Theme.accentStroke
        }

        // The glass: sees every press in the group without taking it (a PointHandler only watches; the panels' own
        // controls keep their presses) and makes the group the active one. In the holder, so it goes along to a
        // floating window.
        Item {
            objectName: "dockGlass_" + slot.dockKey
            anchors.fill: parent
            z: 100

            PointHandler {
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                onActiveChanged: {
                    if (active)
                        slot.manager?.setActiveGroup(slot.dockKey)
                }
            }
        }

        // Whether the pointer is over the group (` maximizes the group under it). On the holder, under its panels: a
        // HoverHandler on the glass above them took the hover the panels need (the timeline lost its edit cursors);
        // a parent's HoverHandler sees hover over its children without taking it.
        HoverHandler {
            id: glassHover
        }

        // The tab menu, made on first use (its host set here: inside the component `slot` would not be this id).
        Loader {
            id: tabMenuLoader
            active: false
            sourceComponent: DockTabMenu {}
            onLoaded: (tabMenuLoader.item as DockTabMenu).host = slot
        }
    }
}

