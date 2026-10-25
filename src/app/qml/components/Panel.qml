pragma ComponentBehavior: Bound
import QtQuick

// A workbench panel (design language v3): an island on the black stage, radius 22, the panel surface, no shadow.
// Docked (the outermost panel of a DockSlot) it opens with a tab strip: a dark band with the panel's tab rising from it
// (hue dot and name; a group of several panels shows the group's DockTabs there instead, which DockSlot lays over the
// strip), then the `leading` tools, and at the right the `trailing` tools and the dock's grip, pop-out and close buttons
// (DockHandle). Undocked with a `title` it shows a "● Title" header. Children go into the padded body below.
// 0.4: `leading` / `trailing` are lists of Items (assigned as before: `trailing: [ IconButton {...}, ... ]`); each tool
// sits in a slot of its row. When the strip is short the tools hide one at a time - the leading ones from their right
// end first, then the trailing ones from their left end (the trailing row's last tool hides last) - and a chevron button
// (`panelToolsOverflow`, "More tools", Premiere's "»") just before the dock's handle lists the hidden buttons
// (`panelToolsMenu`: icon, the tool's `menuText` or text or tooltip, shortcut, a check when checkable, greyed when
// disabled; a row clicks the button once the menu has closed - a checkable one toggles). Tools that are not buttons
// (badges, toggles, fields, the timeline's palette) just hide; a tool its panel hides itself (its own `visible: false`)
// is neither shown nor listed. A hidden slot is 0 wide and clips (never `visible: false`: a child of a hidden item reads
// `visible` false, and the menu needs the tool's own); its tool, and every button inside it, leaves the Tab chain
// meanwhile. A lone panel's tab takes the keyboard too: Tab reaches it, the Menu key or Shift+F10 opens
// its tab menu, and its tooltip names the panel and its focus key.
Rectangle {
    id: root

    property string title: ""
    /// The DockSlot this panel is the outermost panel of (found through its holder, which goes along when the slot
    /// floats), else null: a panel inside another one never carries the dock's handle. A binding over the whole parent
    /// chain, so it follows the panel when an ancestor moves (0.1.3 workspaces move panels between the columns).
    readonly property DockSlot dockHost: findDockHost()
    function findDockHost(): DockSlot {
        for (let p = root.parent; p; p = p.parent) {
            if (p instanceof Panel)
                return null
            if (p instanceof DockHolder)
                return (p as DockHolder).dockSlot as DockSlot
        }
        return null
    }
    property int padding: Theme.pad
    // Bottom padding follows the density (compact mode) unless the panel uses a custom padding.
    property int bottomPadding: padding === Theme.pad ? Theme.panelBottomPad : padding
    readonly property bool hasHeader: title !== ""
    /// Docked, in a group of several panels: the group's tabs (DockTabs, owned by the slot) cover the strip's left.
    readonly property bool tabbed: dockHost !== null && dockHost.tabbed
    /// The tab strip: docked panels with a title, and every panel of a tabbed group.
    readonly property bool hasStrip: dockHost !== null && (hasHeader || tabbed)
    readonly property bool hasTopRow: hasStrip || hasHeader
    default property alias contentData: body.data
    /// The header's tools (0.4: lists of Items, see above).
    property list<Item> leading
    property list<Item> trailing
    readonly property alias body: body

    /// 0.4: which tools show: { leading: [bool], trailing: [bool] } in each row's order, and `hidden`: the button tools
    /// that have no room, in on-screen order (the More tools menu lists them).
    readonly property var toolFit: root.fitTools()
    readonly property var hiddenTools: root.toolFit.hidden
    readonly property real toolSpacing: Theme.space(1)
    /// The More tools button and its gap.
    readonly property real overflowWidth: Theme.controlHeightSmall + Theme.space(1)

    /// A tool's width in its row: 0 while its panel hides it.
    function toolWidth(tool: Item): real {
        return tool !== null && tool.visible ? Math.max(0, tool.width) : 0
    }
    /// Buttons go into the More tools menu when they have no room; anything else just hides.
    function listable(tool: Item): bool {
        return tool instanceof IconButton || tool instanceof PillButton
    }
    /// The tools that fit in `header.room`: everything when it can; else the More tools button's room is kept and the
    /// trailing tools fit from their right end, then the leading ones from their left end, each row stopping at the
    /// first tool that does not fit.
    function fitTools(): var {
        const lead = []
        const trail = []
        for (let i = 0; i < root.leading.length; ++i)
            lead.push(root.leading[i])
        for (let i = 0; i < root.trailing.length; ++i)
            trail.push(root.trailing[i])
        const leadShown = lead.map(() => true)
        const trailShown = trail.map(() => true)
        const leadWidth = root.naturalRowWidth(lead)
        const trailWidth = root.naturalRowWidth(trail)
        const both = leadWidth > 0 && trailWidth > 0 ? Theme.space(2) : 0
        if (leadWidth + trailWidth + both <= header.room)
            return { leading: leadShown, trailing: trailShown, hidden: [] }

        let left = header.room - root.overflowWidth
        let trailUsed = 0
        let stopped = false
        for (let i = trail.length - 1; i >= 0; --i) {
            const w = root.toolWidth(trail[i] as Item)
            if (w <= 0)
                continue
            const need = w + (trailUsed > 0 ? root.toolSpacing : 0)
            if (!stopped && need <= left) {
                left -= need
                trailUsed += need
            } else {
                stopped = true
                trailShown[i] = false
            }
        }
        if (trailUsed > 0)
            left -= Theme.space(2)
        let leadUsed = 0
        stopped = false
        for (let i = 0; i < lead.length; ++i) {
            const w = root.toolWidth(lead[i] as Item)
            if (w <= 0)
                continue
            const need = w + (leadUsed > 0 ? root.toolSpacing : 0)
            if (!stopped && need <= left) {
                left -= need
                leadUsed += need
            } else {
                stopped = true
                leadShown[i] = false
            }
        }
        const hidden = []
        for (let i = 0; i < lead.length; ++i) {
            const tool = lead[i] as Item
            if (!leadShown[i] && root.toolWidth(tool) > 0 && root.listable(tool))
                hidden.push(tool)
        }
        for (let i = 0; i < trail.length; ++i) {
            const tool = trail[i] as Item
            if (!trailShown[i] && root.toolWidth(tool) > 0 && root.listable(tool))
                hidden.push(tool)
        }
        return { leading: leadShown, trailing: trailShown, hidden: hidden }
    }
    /// The width all the tools of `row` (an array) take, with their spacing.
    function naturalRowWidth(row: var): real {
        let sum = 0
        let n = 0
        for (let i = 0; i < row.length; ++i) {
            const w = root.toolWidth(row[i] as Item)
            if (w > 0) {
                sum += w
                ++n
            }
        }
        return sum + Math.max(0, n - 1) * root.toolSpacing
    }
    /// Whether the tool at `index` of the row `side` ("leading" | "trailing") shows.
    function toolShown(side: string, index: int): bool {
        const row = side === "leading" ? root.toolFit.leading : root.toolFit.trailing
        return row[index] === true
    }
    /// The x of the tool at `index` in its row (shown tools only take room).
    function toolX(side: string, index: int): real {
        const tools = side === "leading" ? root.leading : root.trailing
        let x = 0
        for (let i = 0; i < index && i < tools.length; ++i) {
            const w = root.toolShown(side, i) ? root.toolWidth(tools[i]) : 0
            if (w > 0)
                x += w + root.toolSpacing
        }
        return x
    }
    /// The width the shown tools of the row `side` take.
    function rowWidth(side: string): real {
        const tools = side === "leading" ? root.leading : root.trailing
        let sum = 0
        let n = 0
        for (let i = 0; i < tools.length; ++i) {
            const w = root.toolShown(side, i) ? root.toolWidth(tools[i]) : 0
            if (w > 0) {
                sum += w
                ++n
            }
        }
        return sum + Math.max(0, n - 1) * root.toolSpacing
    }

    // A hidden button's row in the More tools menu runs it once the menu has closed.
    property var pendingTool: null
    /// Runs a hidden tool from the More tools menu as a click would (AbstractButton.click(), Qt 6.8: a checkable tool
    /// toggles, then toggled() and clicked() are emitted; nothing while it is disabled).
    function runTool(tool: var) {
        if (typeof tool.click === "function")
            tool.click()
        else
            tool.clicked()
    }
    /// A hidden tool's row label: its `menuText` when it declares one (a short name that does not change with its
    /// state, e.g. "Keyframe lane" for a checkable tool whose tooltip says "Hide the keyframe lane"), else its text,
    /// else its tooltip.
    function menuLabel(tool: var): string {
        if (!tool)
            return ""
        const own = String(tool.menuText ?? "")
        if (own !== "")
            return own
        const text = String(tool.text ?? "")
        return text !== "" ? text : String(tool.tooltip ?? "")
    }

    radius: Theme.radiusPanel
    color: Theme.panel
    border.width: Theme.hairline
    border.color: Theme.border
    // A screen reader names the region by its title (M5n).
    Accessible.role: Accessible.Pane
    Accessible.name: root.title

    // A tool's place in its row: as wide as the tool while it shows, 0 (clipped) while it has no room. The tool is
    // moved in here, centred on the strip's line; it leaves the Tab chain while hidden.
    component ToolSlot: Item {
        id: toolSlot

        required property Item modelData
        required property int index
        property string side: "leading"
        readonly property var tool: toolSlot.modelData
        readonly property bool shown: root.toolShown(toolSlot.side, toolSlot.index)
        // The tool's own focus policy while it is hidden (-1: not changed).
        property int savedPolicy: -1

        x: root.toolX(toolSlot.side, toolSlot.index)
        width: toolSlot.shown ? root.toolWidth(toolSlot.tool) : 0
        height: parent ? parent.height : 0
        clip: true
        // A hidden tool that is not a listed button (the timeline's palette, a field, a badge) has no focus policy of
        // its own to turn off, but the buttons inside it do: the slot is disabled while hidden, which takes everything
        // in it out of the Tab chain. A listed button stays enabled (its menu row greys with the tool's own `enabled`)
        // and leaves the chain through its focus policy (applyFocus).
        enabled: toolSlot.shown || root.listable(toolSlot.tool)

        function placeTool() {
            const tool = toolSlot.tool
            if (!tool)
                return
            tool.parent = toolSlot
            tool.x = 0
            tool.y = Qt.binding(() => (toolSlot.height - tool.height) / 2)
        }
        function applyFocus() {
            const tool = toolSlot.tool
            if (!tool || tool.focusPolicy === undefined)
                return
            if (!toolSlot.shown && toolSlot.savedPolicy < 0) {
                toolSlot.savedPolicy = tool.focusPolicy
                tool.focusPolicy = Qt.NoFocus
            } else if (toolSlot.shown && toolSlot.savedPolicy >= 0) {
                tool.focusPolicy = toolSlot.savedPolicy
                toolSlot.savedPolicy = -1
            }
        }
        onShownChanged: toolSlot.applyFocus()
        Component.onCompleted: {
            toolSlot.placeTool()
            toolSlot.applyFocus()
        }
    }

    // The tab strip's band: the panel's top corners, square at its foot (clipped from a taller rounded rectangle so its
    // corners match the panel's).
    Item {
        id: strip
        visible: root.hasStrip
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.hasStrip ? Theme.tabStripHeight : 0
        clip: true

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: parent.height + root.radius * 2
            radius: root.radius
            color: Theme.sunken
        }

        // A lone panel's tab (a group's tabs are the slot's DockTabs, laid over this spot).
        PanelTab {
            id: ownTab
            objectName: "panelTab"
            visible: !root.tabbed
            x: Theme.space(3)
            anchors.bottom: parent.bottom
            width: Math.min(naturalWidth, Math.max(Theme.space(16), header.width * 0.5))
            text: root.dockHost ? Workspaces.panelLabel(root.dockHost.currentPanel) || root.title : root.title
            dotColor: root.dockHost ? Workspaces.panelHue(root.dockHost.currentPanel) : Theme.accentStroke
            surfaceColor: root.color
            trailingSpace: Theme.space(4)
        }
        // UX rework: a lone panel's tab works like a group's tab (DockTabs): dragging it moves the group, a right-click
        // opens the tab menu (DockTabMenu), a double-click maximizes the group. Docked panels only.
        // 0.4: Tab reaches it (a focus ring), the Menu key or Shift+F10 opens its menu, a tooltip names it and its key.
        Loader {
            active: root.dockHost !== null && !root.tabbed
            x: ownTab.x
            y: ownTab.y
            width: ownTab.width
            height: ownTab.height
            sourceComponent: Item {
                id: ownTabArea
                objectName: "panelTabArea"

                readonly property string panelKey: root.dockHost ? root.dockHost.currentPanel : ""
                readonly property string label: ownTab.text
                readonly property string shortcut: {
                    const command = root.dockHost ? (root.dockHost.panelCommands[ownTabArea.panelKey] ?? "") : ""
                    return command !== "" && KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText(command) : ""
                }
                /// "Library  [Shift+1]": the tab's tooltip (its full name and its focus key).
                readonly property string tooltipText: ownTabArea.shortcut !== ""
                                                      ? qsTr("%1  [%2]").arg(ownTabArea.label).arg(ownTabArea.shortcut)
                                                      : ownTabArea.label

                function isMenuKey(event: KeyEvent): bool {
                    return event.key === Qt.Key_Menu
                           || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier) !== 0)
                }
                function openMenu() {
                    root.dockHost?.openTabMenu(ownTabArea.panelKey, ownTabArea, 0, ownTabArea.height)
                }

                activeFocusOnTab: true
                Accessible.role: Accessible.PageTab
                Accessible.name: ownTabArea.label
                Accessible.description: ownTabArea.tooltipText
                Accessible.selected: true
                Keys.onShortcutOverride: event => event.accepted = ownTabArea.isMenuKey(event)
                Keys.onPressed: event => {
                    if (ownTabArea.isMenuKey(event)) {
                        event.accepted = true
                        ownTabArea.openMenu()
                    }
                }

                FocusRing {
                    targetRadius: Theme.radiusTab
                    visible: ownTabArea.activeFocus
                }
                HoverHandler {
                    id: ownTabHover
                    cursorShape: ownTabDrag.active ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                }
                Loader {
                    active: ownTabHover.hovered && !ownTabDrag.active
                    sourceComponent: Tooltip {
                        parent: ownTabArea
                        visible: true
                        text: ownTabArea.label
                        shortcut: ownTabArea.shortcut
                    }
                }
                TapHandler {
                    onDoubleTapped: root.dockHost?.toggleMaximize()
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: eventPoint => root.dockHost?.openTabMenu(root.dockHost.currentPanel, ownTabArea,
                                                                       eventPoint.position.x, eventPoint.position.y)
                }
                DragHandler {
                    id: ownTabDrag
                    target: null
                    onActiveChanged: {
                        if (active)
                            root.dockHost?.beginPanelDrag(root.dockHost.currentPanel)
                        else
                            root.dockHost?.endDrag()
                    }
                    onCentroidChanged: {
                        if (active)
                            root.dockHost?.moveDrag(ownTabArea.mapToGlobal(centroid.position.x, centroid.position.y),
                                                    centroid.modifiers)
                    }
                }
            }
        }
    }

    Item {
        id: header
        visible: root.hasTopRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: root.hasStrip ? 0 : root.padding
        anchors.rightMargin: root.hasStrip ? Theme.space(2) : root.padding
        anchors.topMargin: root.hasStrip ? 0 : Theme.panelHeaderTop
        height: root.hasStrip ? Theme.tabStripHeight : root.hasHeader ? Theme.controlHeightSmall : 0

        // Where the tools start: after the tab (or the group's tabs) in the strip, after the title otherwise.
        readonly property real toolsX: root.hasStrip
                                       ? (root.tabbed ? Theme.space(3) + root.dockHost.tabsWidth : ownTab.x + ownTab.width)
                                         + Theme.space(4)
                                       : titleHeader.implicitWidth + Theme.space(3)
        // The dock's handle and its gap.
        readonly property real handleWidth: dockTools.width > 0 ? dockTools.width + Theme.space(2) : 0
        // Room beside the tab or title (and the dock's handle) for both rows of tools.
        readonly property real room: width - toolsX - handleWidth

        SectionHeader {
            id: titleHeader
            visible: !root.hasStrip
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
        }

        Item {
            id: leadingRow
            objectName: "panelLeadingTools"
            x: header.toolsX
            // In the strip the tools sit on the tab's line, a little below the band's middle.
            y: root.hasStrip ? Theme.space(0.5) : 0
            width: root.rowWidth("leading")
            height: parent.height

            Repeater {
                model: root.leading
                delegate: ToolSlot {
                    side: "leading"
                }
            }
        }

        Item {
            id: trailingRow
            objectName: "panelTrailingTools"
            x: header.width - header.handleWidth - (overflowButton.visible ? root.overflowWidth : 0) - width
            y: root.hasStrip ? Theme.space(0.5) : 0
            width: root.rowWidth("trailing")
            height: parent.height

            Repeater {
                model: root.trailing
                delegate: ToolSlot {
                    side: "trailing"
                }
            }
        }

        // 0.4: the buttons that have no room (More tools, Premiere's "»"), just before the dock's handle.
        IconButton {
            id: overflowButton
            objectName: "panelToolsOverflow"
            visible: root.hiddenTools.length > 0
            x: header.width - header.handleWidth - width
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: root.hasStrip ? Theme.space(0.5) : 0
            compact: true
            // Premiere's "»": not the "···" some panels use for their own menu (the Library's, Color's), which may
            // sit right beside it.
            iconName: "chevron-right"
            tooltip: qsTr("More tools")
            onClicked: toolsMenu.popup(overflowButton, 0, overflowButton.height)
        }

        Loader {
            id: dockTools
            active: root.dockHost !== null && root.hasStrip
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: Theme.space(0.5)
            sourceComponent: DockHandle {
                host: root.dockHost
            }
        }
    }

    // The More tools menu: a row per hidden button, in on-screen order (Wabcut's ContextMenu: this file imports QtQuick
    // only).
    ContextMenu {
        id: toolsMenu
        objectName: "panelToolsMenu"

        Instantiator {
            model: root.hiddenTools
            delegate: ContextMenuItem {
                id: toolRow
                required property var modelData
                readonly property var tool: toolRow.modelData
                objectName: "panelToolsMenu_" + (toolRow.tool ? toolRow.tool.objectName : "")
                text: root.menuLabel(toolRow.tool)
                iconName: toolRow.tool ? String(toolRow.tool.iconName ?? "") : ""
                command: toolRow.tool ? String(toolRow.tool.command ?? "") : ""
                checkable: toolRow.tool !== null && toolRow.tool.checkable === true
                checked: toolRow.tool !== null && toolRow.tool.checked === true
                enabled: toolRow.tool !== null && toolRow.tool.enabled
                onTriggered: {
                    root.pendingTool = toolRow.tool
                    toolRow.checked = Qt.binding(() => toolRow.tool !== null && toolRow.tool.checked === true)
                }
            }
            onObjectAdded: (index, object) => toolsMenu.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => toolsMenu.removeItem(object as ContextMenuItem)
        }
        // The row does what a click on the tool does: AbstractButton.click() toggles a checkable tool and emits
        // toggled() and clicked() (a tool that acts in onToggled, the Media Browser's Metadata toggle, works too).
        onClosed: {
            const tool = root.pendingTool
            root.pendingTool = null
            if (tool)
                Qt.callLater(() => root.runTool(tool))
        }
    }

    // A dock's panel without a strip (a monitor switching with the other one): the handle over the top-right corner.
    HoverHandler {
        id: cornerHover
        enabled: root.dockHost !== null && !root.hasStrip
    }
    Loader {
        id: cornerTools
        active: root.dockHost !== null && !root.hasStrip
        z: 2
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Theme.space(2)
        anchors.rightMargin: Theme.space(2)
        opacity: cornerHover.hovered || ((cornerTools.item as DockHandle)?.busy ?? false) ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
        sourceComponent: DockHandle {
            host: root.dockHost
        }
    }

    Item {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: root.hasTopRow ? header.bottom : parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.padding
        anchors.rightMargin: root.padding
        anchors.topMargin: root.hasTopRow ? Theme.panelHeaderGap : root.padding
        anchors.bottomMargin: root.bottomPadding
    }
}
