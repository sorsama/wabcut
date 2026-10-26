import QtQuick
import QtQuick.Window

// Panel docking (0.1.3): where the current workspace's panels are, and which groups float in windows of their own.
//
//   * The layout is a tree (`tree`, the workspace's: Workspaces.qml): a split ("row": side by side, "column": stacked)
//     of panel groups and further splits, with a weight for each child (the children share their split's room by
//     weight; the gutters between them move it). A group ({ group: id, panels, current }) shows one panel at a time,
//     with tabs when it holds several. The workbench (Main.qml) makes a DockSlot for each group (`groups`) and puts the
//     panels into the group they belong to (hostOf()); a closed panel waits, alive, outside the layout.
//   * Dragging a group's grip (DockHandle) or one of its tabs (DockTabs) shows where it would go: onto another group's
//     header or middle it joins that group as tabs, near its edge it docks beside it (that group's room is split), at
//     the work area's edge it runs along that whole side, and anywhere else (over the title bar, outside the main
//     window, or with Ctrl held) it gets a window of its own there (FloatingDock). Closing that window, or the
//     button in its header, docks it back where it was; dragging its grip onto the workbench docks it there.
//   * A panel's close button (its tab's ×, or the one in its header) closes it; openPanel() opens it again where it fits
//     (the panel focus keys and other reveals).
//   * UX rework (Premiere's frames): the group the user worked in last is the active one (`activeGroup`, drawn with an
//     accent outline): the Window menu opens a closed panel there (openPanelInActiveGroup), Shift+` maximizes it and
//     Ctrl+W closes its panel. A group can be maximized (`maximizedGroup`: it takes the whole work area, the others
//     hide) and restored. A tab's menu closes, undocks and maximizes (tabMenuAction).
//   * Every change is kept in the workspace (Workspaces.update): its layout and its floating windows' places. The active
//     and the maximized group are not kept.
QtObject {
    id: manager

    /// The main window and its work area (drop positions are in the work area's coordinates).
    required property Window mainWindow
    required property Item area
    /// The workspace whose layout this is (load() switches).
    property int workspace: -1
    /// The layout (see the header); null when every panel is closed.
    property var tree: null
    /// The groups of the layout (one row per group, role `gid`): the workbench makes a DockSlot for each.
    property ListModel groups: ListModel {}
    /// Each group's slot and the item its panels go into, by group id (DockSlots register themselves).
    property var docked: ({})
    /// Panels that cannot show now (the Source Monitor while the Trim view is open): a group shows another of its
    /// panels, or takes no room.
    property var unavailable: []
    /// Where each group is in the work area now (Main.qml's layout: { group id: {x, y, width, height} }).
    property var rects: ({})
    /// The groups that share one place now (Main.qml's layout: two monitors next to each other in too little room, one
    /// shown with a Source/Program switch). The Window menu opens no panel into them (it would split them apart).
    property var sharedGroups: []
    /// A gutter is being dragged.
    property bool resizing: false
    /// Writing the workspace (Main.qml then knows the change is ours and keeps the layout as it is).
    property bool saving: false
    /// UX rework: the group the user worked in last ("" none): a press in it (DockSlot's glass), one of its tabs chosen,
    /// a panel dropped there, or a panel of it focused through a focus key, the Window menu or Main.showBrowserPanel.
    /// Focus given at startup does not set it. Cleared when its group goes away and when a workspace loads.
    property string activeGroup: ""
    /// UX rework: the group that takes the whole work area while the others hide ("" none; Main.qml lays it out alone).
    /// Restored by toggling again, and when a workspace loads, a new group appears, its group goes away or floats, or a
    /// panel of another group is shown.
    property string maximizedGroup: ""

    // A grip or tab being dragged: its group's slot, the one panel being dragged ("": the whole group), the pointer
    // (work-area and screen coordinates), whether it left the main window, and what a drop would do now:
    // { kind: "none" | "float" | "join" | "beside" | "edge", target, side }.
    property DockSlot dragged: null
    property string draggedPanel: ""
    property point dragPoint: Qt.point(0, 0)
    property point dragGlobal: Qt.point(0, 0)
    property bool dragOutside: false
    property bool dragFloat: false
    property var drop: ({ kind: "none" })
    readonly property DockSlot target: manager.drop.target ?? null
    /// Where the panel would land, in the work area (the overlay draws it); empty when nothing would happen there.
    property rect dropRect: Qt.rect(0, 0, 0, 0)
    /// How close to the work area's edge the pointer docks along that whole side (px), how deep into a group its edge
    /// zones reach (of its width or height), and how tall its header is (dropping there joins it).
    readonly property real edgeBand: Theme.space(3)
    readonly property real sideZone: 0.28
    readonly property real headerZone: Theme.tabStripHeight

    /// What a drop would do now, for the overlay.
    readonly property string dropText: {
        const d = manager.dragged
        if (!d)
            return ""
        const what = manager.dragName()
        const t = manager.drop.target ? Workspaces.panelLabel(manager.shownPanel(manager.drop.target.dockKey)) : ""
        switch (manager.drop.kind) {
        case "float":
            return d.floating && manager.draggingWhole() ? qsTr("Release to move %1's window here").arg(what)
                                                         : qsTr("Release to open %1 in its own window").arg(what)
        case "join":
            return qsTr("Add %1 to %2's tabs").arg(what).arg(t)
        case "beside":
            switch (manager.drop.side) {
            case "left": return qsTr("Put %1 left of %2").arg(what).arg(t)
            case "right": return qsTr("Put %1 right of %2").arg(what).arg(t)
            case "top": return qsTr("Put %1 above %2").arg(what).arg(t)
            }
            return qsTr("Put %1 below %2").arg(what).arg(t)
        case "edge":
            switch (manager.drop.side) {
            case "left": return qsTr("Put %1 along the left edge").arg(what)
            case "right": return qsTr("Put %1 along the right edge").arg(what)
            case "top": return qsTr("Put %1 along the top").arg(what)
            }
            return qsTr("Put %1 along the bottom").arg(what)
        }
        if (manager.drop.shared === true)
            return qsTr("The two monitors share this space: drop left or right of them, or make the window wider to add tabs to a monitor")
        return qsTr("Drop on a panel's tabs or middle to add it there, near an edge to dock beside it, or outside the panels (or hold Ctrl) for a window")
    }

    // ---- Groups ----

    function isGroup(node: var): bool {
        return typeof node.group === "string"
    }
    function leafOf(gid: string): var {
        return Workspaces.groupsOf(manager.tree).find(g => g.group === gid) ?? null
    }
    /// The group a panel is docked in, or null (closed).
    function groupOfPanel(key: string): var {
        return Workspaces.groupsOf(manager.tree).find(g => g.panels.includes(key)) ?? null
    }
    /// The first group holding a panel of the catalog's `column` ("left": the browser panels, "right": the clip editors).
    function groupOfColumn(column: string): var {
        return Workspaces.groupsOf(manager.tree).find(g => g.panels.some(k => Workspaces.panel(k)?.column === column)) ?? null
    }
    function isOpen(key: string): bool {
        return manager.groupOfPanel(key) !== null
    }
    /// The panel a group shows: its current one, or (that one unavailable) the first that can show; "" for none.
    function shownPanel(gid: string): string {
        const g = manager.leafOf(gid)
        if (!g)
            return ""
        if (!manager.unavailable.includes(g.current))
            return g.current
        return g.panels.find(k => !manager.unavailable.includes(k)) ?? ""
    }
    function slotOf(gid: string): DockSlot {
        return manager.docked[gid]?.slot ?? null
    }
    function hostOf(gid: string): Item {
        return manager.docked[gid]?.host ?? null
    }
    /// The slot of the group that holds `key`, or null.
    function slotOfPanel(key: string): DockSlot {
        const g = manager.groupOfPanel(key)
        return g ? manager.slotOf(g.group) : null
    }
    function register(gid: string, slot: DockSlot, host: Item) {
        const next = Object.assign({}, manager.docked)
        next[gid] = { slot: slot, host: host }
        manager.docked = next
    }
    function unregister(gid: string, slot: DockSlot) {
        if (manager.docked[gid]?.slot !== slot)
            return
        const next = Object.assign({}, manager.docked)
        delete next[gid]
        manager.docked = next
    }
    /// The groups that take room in the work area now: { id: true } (not floating, a panel that can show).
    function presence(): var {
        const out = {}
        for (const g of Workspaces.groupsOf(manager.tree)) {
            const slot = manager.slotOf(g.group)
            out[g.group] = manager.shownPanel(g.group) !== "" && !(slot && slot.floating)
        }
        return out
    }

    // ---- Changing the layout ----

    /// Makes `next` the layout: the workbench follows, the workspace keeps it.
    function setTree(next: var) {
        // A group that is going away takes its window with it.
        const keep = Workspaces.groupsOf(next).map(g => g.group)
        for (const id of Object.keys(manager.docked)) {
            const slot = manager.slotOf(id)
            if (!keep.includes(id) && slot && slot.floating)
                manager.dockBack(slot, false)
        }
        const before = Workspaces.groupsOf(manager.tree).map(g => g.group)
        manager.tree = Workspaces.normalizedLayout(next)
        const after = Workspaces.groupsOf(manager.tree).map(g => g.group)
        // Group ids come back (Workspaces.newGroupId): the active one goes with its group. A maximized group gone, or a
        // new group (it would stay hidden), restores the layout.
        if (manager.activeGroup !== "" && !after.includes(manager.activeGroup))
            manager.activeGroup = ""
        if (manager.maximizedGroup !== "" && (!after.includes(manager.maximizedGroup) || after.some(id => !before.includes(id))))
            manager.maximizedGroup = ""
        manager.syncGroups()
        manager.save()
    }
    /// The groups model follows the layout (a group's slot stays while the group does).
    function syncGroups() {
        const ids = Workspaces.groupsOf(manager.tree).map(g => g.group)
        for (let i = manager.groups.count - 1; i >= 0; --i) {
            if (!ids.includes(manager.groups.get(i).gid))
                manager.groups.remove(i)
        }
        const have = []
        for (let i = 0; i < manager.groups.count; ++i)
            have.push(manager.groups.get(i).gid)
        for (const id of ids) {
            if (!have.includes(id))
                manager.groups.append({ gid: id })
        }
    }
    /// The layout without the panel `key` (its group closes when it was the last one there).
    function withoutPanel(node: var, key: string): var {
        function walk(n) {
            if (manager.isGroup(n)) {
                if (!n.panels.includes(key))
                    return n
                const panels = n.panels.filter(k => k !== key)
                if (panels.length === 0)
                    return null
                const i = n.panels.indexOf(key)
                return { group: n.group, panels: panels,
                         current: n.current === key ? panels[Math.min(i, panels.length - 1)] : n.current }
            }
            const out = { split: n.split, children: [], weights: [] }
            for (let i = 0; i < n.children.length; ++i) {
                const c = walk(n.children[i])
                if (c) {
                    out.children.push(c)
                    out.weights.push(n.weights[i])
                }
            }
            return out.children.length > 0 ? out : null
        }
        return node ? Workspaces.normalizedLayout(walk(Workspaces.copy(node))) : null
    }
    /// The layout without the group `gid`.
    function withoutGroup(node: var, gid: string): var {
        let out = Workspaces.copy(node)
        for (const k of manager.leafOf(gid)?.panels ?? [])
            out = manager.withoutPanel(out, k)
        return out
    }
    /// `leaf` goes on `side` of the group `targetId` in `node`: into the target's split when that runs the same way (with
    /// the target's share: the others make room), else in a new split in the target's place (half its room each).
    function insertBeside(node: var, targetId: string, leaf: var, side: string): var {
        const dir = side === "left" || side === "right" ? "row" : "column"
        const first = side === "left" || side === "top"
        function walk(n) {
            if (manager.isGroup(n))
                return n.group === targetId ? { split: dir, children: first ? [leaf, n] : [n, leaf], weights: [1, 1] } : n
            for (let i = 0; i < n.children.length; ++i) {
                const c = n.children[i]
                if (manager.isGroup(c) && c.group === targetId && n.split === dir) {
                    const w = n.weights[i]
                    n.children.splice(first ? i : i + 1, 0, leaf)
                    n.weights.splice(first ? i : i + 1, 0, w)
                    return n
                }
                n.children[i] = walk(c)
            }
            return n
        }
        return walk(Workspaces.copy(node))
    }
    /// 0.5: `leaf` goes left or right (`side`) of the group `targetId`, taking `share` of its room: the Audio Meters
    /// right of the timeline, or the timeline back left of them (the layout's least width still holds).
    function insertSharing(node: var, targetId: string, leaf: var, side: string, share: real): var {
        const first = side === "left"
        function walk(n) {
            if (manager.isGroup(n)) {
                if (n.group !== targetId)
                    return n
                return { split: "row", children: first ? [leaf, n] : [n, leaf],
                         weights: first ? [share, 1 - share] : [1 - share, share] }
            }
            for (let i = 0; i < n.children.length; ++i) {
                const c = n.children[i]
                if (manager.isGroup(c) && c.group === targetId && n.split === "row") {
                    const w = n.weights[i]
                    n.weights[i] = w * (1 - share)
                    n.children.splice(first ? i : i + 1, 0, leaf)
                    n.weights.splice(first ? i : i + 1, 0, w * share)
                    return n
                }
                n.children[i] = walk(c)
            }
            return n
        }
        return walk(Workspaces.copy(node))
    }
    /// `leaf` runs along the whole `side` of `node` (or is all of it when `node` is empty).
    function insertAtEdge(node: var, leaf: var, side: string): var {
        if (!node)
            return leaf
        const dir = side === "left" || side === "right" ? "row" : "column"
        const first = side === "left" || side === "top"
        const rest = Workspaces.copy(node)
        if (!manager.isGroup(rest) && rest.split === dir) {
            // A share like the others' (the average weight).
            let sum = 0
            for (const w of rest.weights)
                sum += w
            const w = sum / rest.weights.length
            rest.children.splice(first ? 0 : rest.children.length, 0, leaf)
            rest.weights.splice(first ? 0 : rest.weights.length, 0, w)
            return rest
        }
        return { split: dir, children: first ? [leaf, rest] : [rest, leaf], weights: first ? [1, 2] : [2, 1] }
    }
    /// The layout with `panels` added to the group `targetId` as tabs, the first of them shown.
    function withTabs(node: var, targetId: string, panels: var): var {
        const out = Workspaces.copy(node)
        const g = Workspaces.groupsOf(out).find(x => x.group === targetId)
        if (g) {
            for (const k of panels) {
                if (!g.panels.includes(k))
                    g.panels.push(k)
            }
            g.current = panels[0]
        }
        return out
    }

    /// The group `gid` (all its tabs) joins the group `targetId`.
    function joinGroup(gid: string, targetId: string) {
        const g = manager.leafOf(gid)
        if (!g || gid === targetId || !manager.leafOf(targetId))
            return
        const panels = [g.current].concat(g.panels.filter(k => k !== g.current))
        const slot = manager.slotOf(gid)
        if (slot && slot.floating)
            manager.dockBack(slot, false)
        manager.setTree(manager.withTabs(manager.withoutGroup(manager.tree, gid), targetId, panels))
    }
    /// The panel `key` joins the group `targetId` as a tab (and shows).
    function joinPanel(key: string, targetId: string) {
        const from = manager.groupOfPanel(key)
        if (!manager.leafOf(targetId) || (from && from.group === targetId))
            return
        if (from && from.panels.length === 1)
            return manager.joinGroup(from.group, targetId)
        manager.setTree(manager.withTabs(manager.withoutPanel(manager.tree, key), targetId, [key]))
    }
    /// The group `gid` docks on `side` of the group `targetId`.
    function moveGroupBeside(gid: string, targetId: string, side: string) {
        const g = manager.leafOf(gid)
        if (!g || gid === targetId)
            return
        const slot = manager.slotOf(gid)
        if (slot && slot.floating)
            manager.dockBack(slot, false)
        const rest = manager.withoutGroup(manager.tree, gid)
        if (rest && Workspaces.groupsOf(rest).some(x => x.group === targetId)) {
            // It shows where it lands: a maximized group gives the room back (the group keeps its id, so setTree sees
            // nothing new: a group back from its window would stay hidden).
            manager.expose(gid)
            manager.setTree(manager.insertBeside(rest, targetId, Workspaces.copy(g), side))
        }
    }
    /// The panel `key` docks on `side` of the group `targetId` in a group of its own.
    function movePanelBeside(key: string, targetId: string, side: string) {
        const from = manager.groupOfPanel(key)
        if (from && from.panels.length === 1)
            return manager.moveGroupBeside(from.group, targetId, side)
        const rest = manager.withoutPanel(manager.tree, key)
        if (!rest || !Workspaces.groupsOf(rest).some(x => x.group === targetId))
            return
        const leaf = { group: Workspaces.newGroupId(manager.tree), panels: [key], current: key }
        manager.setTree(manager.insertBeside(rest, targetId, leaf, side))
    }
    /// The group `gid` (or the one panel `key` of it, in a group of its own) runs along `side` of the work area.
    function putAtEdge(gid: string, key: string, side: string) {
        const g = manager.leafOf(gid)
        if (!g)
            return
        if (key === "" || g.panels.length === 1) {
            const slot = manager.slotOf(gid)
            if (slot && slot.floating)
                manager.dockBack(slot, false)
            manager.expose(gid) // it shows along that edge (see moveGroupBeside)
            manager.setTree(manager.insertAtEdge(manager.withoutGroup(manager.tree, gid), Workspaces.copy(g), side))
        } else {
            const leaf = { group: Workspaces.newGroupId(manager.tree), panels: [key], current: key }
            manager.setTree(manager.insertAtEdge(manager.withoutPanel(manager.tree, key), leaf, side))
        }
    }
    /// The panel `key` leaves its group for a group of its own beside it (right); returns the new group's id.
    function detach(key: string): string {
        const from = manager.groupOfPanel(key)
        if (!from)
            return ""
        if (from.panels.length === 1)
            return from.group
        const id = Workspaces.newGroupId(manager.tree)
        const rest = manager.withoutPanel(manager.tree, key)
        manager.setTree(manager.insertBeside(rest, from.group, { group: id, panels: [key], current: key }, "right"))
        return id
    }

    /// Shows the panel `key` in its group (its tab), raising its window when it floats (and restoring the layout when
    /// another group is maximized).
    function activate(key: string) {
        const g = manager.groupOfPanel(key)
        if (!g)
            return
        manager.expose(g.group)
        if (g.current !== key) {
            const next = Workspaces.copy(manager.tree)
            Workspaces.groupsOf(next).find(x => x.group === g.group).current = key
            manager.setTree(next)
        }
        const slot = manager.slotOf(g.group)
        if (slot && slot.floating)
            slot.floatWindow.requestActivate()
    }
    /// Opens the panel `key` (and shows it): where it is, else as a tab where panels like it are (Workspaces.catalog's
    /// `column`: with the browser panels, with the clip editors; none there: beside the monitors, left or right), a
    /// monitor beside the other one, the timeline along the bottom, else as a tab in the largest group. Returns the
    /// group's id. A maximized group other than that one is restored (the panel would stay hidden).
    function openPanel(key: string): string {
        const id = manager.placePanel(key)
        manager.expose(id)
        return id
    }
    function placePanel(key: string): string {
        const open = manager.groupOfPanel(key)
        if (open) {
            manager.activate(key)
            return open.group
        }
        if (!Workspaces.panelKeys.includes(key))
            return ""
        const groups = Workspaces.groupsOf(manager.tree)
        const column = Workspaces.panel(key).column
        const id = Workspaces.newGroupId(manager.tree)
        const leaf = { group: id, panels: [key], current: key }
        if (groups.length === 0) {
            manager.setTree(leaf)
            return id
        }
        // 0.5: the Audio Meters go right of the timeline, slim, as Premiere docks them (no timeline: along the bottom),
        // and the timeline comes back left of the Audio Meters when they are in a group of their own.
        const timelineGroup = key === "audioMeters" ? manager.groupOfPanel("timeline") : null
        if (timelineGroup) {
            manager.setTree(manager.insertSharing(manager.tree, timelineGroup.group, leaf, "right", 0.05))
            return id
        }
        const metersGroup = key === "timeline" ? manager.groupOfPanel("audioMeters") : null
        if (metersGroup && metersGroup.panels.length === 1) {
            manager.setTree(manager.insertSharing(manager.tree, metersGroup.group, leaf, "left", 0.95))
            return id
        }
        if (column === "timeline") {
            manager.setTree(manager.insertAtEdge(manager.tree, leaf, "bottom"))
            return id
        }
        if (column === "monitors") {
            const other = manager.groupOfPanel(key === "source" ? "program" : "source")
            if (other) {
                manager.setTree(manager.insertBeside(manager.tree, other.group, leaf, key === "source" ? "left" : "right"))
                return id
            }
        }
        let host = manager.groupOfColumn(column)
        if (!host && (column === "left" || column === "right")) {
            // None like it: beside the monitors (the browser panels on their left, the clip editors on their right).
            const monitor = column === "left" ? (manager.groupOfPanel("source") ?? manager.groupOfPanel("program"))
                                              : (manager.groupOfPanel("program") ?? manager.groupOfPanel("source"))
            if (monitor) {
                manager.setTree(manager.insertBeside(manager.tree, monitor.group, leaf, column))
                return id
            }
        }
        if (!host) {
            // The largest group on the workbench.
            let best = -1
            for (const g of groups) {
                const r = manager.rects[g.group]
                const size = r ? r.width * r.height : 0
                if (size > best) {
                    best = size
                    host = g
                }
            }
        }
        manager.setTree(manager.withTabs(manager.tree, host.group, [key]))
        return host.group
    }
    /// Closes the panel `key` (it waits outside the layout until it is opened again).
    function closePanel(key: string) {
        const g = manager.groupOfPanel(key)
        if (!g)
            return
        manager.setTree(manager.withoutPanel(manager.tree, key))
    }
    /// Closes the other panels of `key`'s group (the tab menu's Close Other Panels in Group); `key` shows.
    function closeOthers(key: string) {
        const g = manager.groupOfPanel(key)
        if (!g || g.panels.length < 2)
            return
        let next = manager.tree
        for (const k of g.panels) {
            if (k !== key)
                next = manager.withoutPanel(next, k)
        }
        manager.setTree(next)
    }

    // ---- UX rework: the active group, the Window menu, the maximized frame, the tab menu ----

    /// `gid` becomes the active group (when it exists).
    function setActiveGroup(gid: string) {
        if (gid !== "" && manager.leafOf(gid))
            manager.activeGroup = gid
    }
    /// The group whose panels hold `item` (up to its DockHolder), or "".
    function groupOfItem(item: Item): string {
        for (let p = item; p; p = p.parent) {
            if (p instanceof DockHolder)
                return ((p as DockHolder).dockSlot as DockSlot)?.dockKey ?? ""
        }
        return ""
    }
    /// The docked group under the mouse pointer (its slot's glass is hovered), or "".
    function groupUnderPointer(): string {
        for (const id of Object.keys(manager.docked)) {
            const slot = manager.slotOf(id)
            if (slot && slot.pointerInside && slot.visible && !slot.floating)
                return id
        }
        return ""
    }
    /// The Window menu (Premiere): a closed panel opens as a tab of the active group and shows there; an open one comes
    /// to the front where it is (its tab; its window raised). Without an active group, or when it is a monitor sharing
    /// its place with the other one (a tab there would split them into two narrow columns): where it fits (openPanel's
    /// column rule). The panel's group becomes the active one; returns its id.
    function openPanelInActiveGroup(key: string): string {
        const target = manager.leafOf(manager.activeGroup)
        if (manager.isOpen(key) || !target || !Workspaces.panelKeys.includes(key)
                || manager.sharedGroups.includes(target.group)) {
            const id = manager.openPanel(key)
            manager.setActiveGroup(id)
            return id
        }
        manager.expose(target.group)
        manager.setTree(manager.withTabs(manager.tree, target.group, [key]))
        const slot = manager.slotOf(target.group)
        if (slot && slot.floating)
            slot.floatWindow.requestActivate()
        return target.group
    }
    /// A maximized group other than `gid` gives the room back (so that `gid` shows).
    function expose(gid: string) {
        if (manager.maximizedGroup !== "" && gid !== "" && gid !== manager.maximizedGroup)
            manager.maximizedGroup = ""
    }
    /// Maximize / Restore Frame: the group `gid` takes the whole work area (the others hide), or, with a group maximized,
    /// the layout comes back (whichever group asks: a floating one too). A floating group is not maximized: it keeps its
    /// window. Returns whether a group is maximized.
    function toggleMaximize(gid: string): bool {
        if (manager.maximizedGroup !== "") {
            manager.maximizedGroup = ""
            return false
        }
        const slot = manager.slotOf(gid)
        if (slot && slot.floating)
            return false
        if (!manager.leafOf(gid) || manager.shownPanel(gid) === "")
            return false
        manager.maximizedGroup = gid
        manager.activeGroup = gid
        return true
    }
    /// A tab menu's choice (DockTabMenu) for the panel `key` of the group `gid`: "close", "closeOthers", "undock" (the
    /// panel in a window of its own), "undockGroup" (the group in a window, or docked back from one) or "maximize".
    function tabMenuAction(what: string, gid: string, key: string) {
        const slot = manager.slotOf(gid)
        switch (what) {
        case "close":
            manager.closePanel(key)
            return
        case "closeOthers":
            manager.closeOthers(key)
            return
        case "undock": {
            const g = manager.groupOfPanel(key)
            if (!g)
                return
            if (g.panels.length === 1) {
                const own = manager.slotOf(g.group)
                if (own && !own.floating)
                    manager.popOut(own)
                return
            }
            const id = manager.detach(key)
            const own = manager.slotOf(id)
            if (own)
                manager.popOut(own)
            manager.setActiveGroup(id)
            return
        }
        case "undockGroup":
            if (!slot)
                return
            if (slot.floating)
                manager.dockBack(slot, true)
            else
                manager.popOut(slot)
            return
        case "maximize":
            manager.toggleMaximize(gid)
            return
        }
    }

    /// Moves a gutter between two children (`before`, `after`: their indexes in the split at `path`, sizes in px) by
    /// `delta` px, each keeping at least `least`.
    function shiftWeight(path: var, before: int, after: int, delta: real, sizeBefore: real, sizeAfter: real, least: real) {
        const next = Workspaces.copy(manager.tree)
        let node = next
        for (const i of path) {
            if (!node || manager.isGroup(node) || i >= node.children.length)
                return
            node = node.children[i]
        }
        if (!node || manager.isGroup(node) || before >= node.weights.length || after >= node.weights.length)
            return
        const total = sizeBefore + sizeAfter
        if (total <= 0)
            return
        const keep = Math.min(least, total / 2)
        const size = Math.max(keep, Math.min(total - keep, sizeBefore + delta))
        const weight = node.weights[before] + node.weights[after]
        node.weights[before] = weight * size / total
        node.weights[after] = weight - node.weights[before]
        manager.tree = next
    }
    /// The two children either side of a gutter get even shares again.
    function evenWeights(path: var, before: int, after: int) {
        const next = Workspaces.copy(manager.tree)
        let node = next
        for (const i of path) {
            if (!node || manager.isGroup(node) || i >= node.children.length)
                return
            node = node.children[i]
        }
        if (!node || manager.isGroup(node) || before >= node.weights.length || after >= node.weights.length)
            return
        const half = (node.weights[before] + node.weights[after]) / 2
        node.weights[before] = half
        node.weights[after] = half
        manager.tree = next
    }

    // ---- Layout ----

    /// Lays `root` out in a w × h work area. `present[id]`: the group takes room now; `gap`: the gutter; `dualMin`: the
    /// least room two monitors side by side take (two single-monitor groups next to each other in less share it, one
    /// shown); `least`: what a child keeps at least. Returns { rects: { group id: {x, y, width, height} },
    /// gutters: [{ vertical, x, y, length, path, before, after, sizeBefore, sizeAfter }], merged, shared: [group ids] }:
    /// `vertical` gutters lie between stacked children (dragged up and down); `shared`: the groups sharing a place.
    function layout(root: var, w: real, h: real, present: var, gap: real, dualMin: real, least: real): var {
        const rects = {}
        const gutters = []
        const sharing = []
        let merged = false
        const takesRoom = node => manager.isGroup(node) ? present[node.group] === true : node.children.some(c => takesRoom(c))
        const monitorOf = node => {
            if (!manager.isGroup(node) || node.panels.length !== 1)
                return ""
            const k = node.panels[0]
            return k === "source" || k === "program" ? k : ""
        }
        function place(node, x, y, width, height, path) {
            if (manager.isGroup(node)) {
                rects[node.group] = { x: x, y: y, width: Math.max(0, width), height: Math.max(0, height) }
                return
            }
            const row = node.split === "row"
            const extent = row ? width : height
            const kids = []
            for (let i = 0; i < node.children.length; ++i) {
                if (takesRoom(node.children[i]))
                    kids.push({ node: node.children[i], index: i, size: 0, start: 0, end: 0 })
            }
            if (kids.length === 0)
                return
            const avail = Math.max(0, extent - gap * (kids.length - 1))
            let sum = 0
            for (const k of kids)
                sum += node.weights[k.index]
            for (const k of kids)
                k.size = avail * node.weights[k.index] / sum
            // Each child keeps `least` where the room allows (taken from the larger ones).
            const keep = Math.min(least, avail / kids.length)
            let short = 0
            let spare = 0
            for (const k of kids) {
                if (k.size < keep)
                    short += keep - k.size
                else
                    spare += k.size - keep
            }
            if (short > 0 && spare > 0) {
                for (const k of kids)
                    k.size = k.size < keep ? keep : k.size - short * (k.size - keep) / spare
            }
            let at = row ? x : y
            const far = row ? x + width : y + height
            for (let k = 0; k < kids.length; ++k) {
                kids[k].start = Math.round(at)
                kids[k].end = k === kids.length - 1 ? Math.round(far) : Math.round(at + kids[k].size)
                at += kids[k].size + gap
            }
            // Two monitors next to each other in too little room share it (one shows, with a switch).
            const shared = []
            if (row) {
                for (let k = 0; k + 1 < kids.length; ++k) {
                    const a = monitorOf(kids[k].node)
                    const b = monitorOf(kids[k + 1].node)
                    if (a !== "" && b !== "" && a !== b && kids[k + 1].end - kids[k].start < dualMin)
                        shared.push(k)
                }
            }
            for (const k of kids) {
                place(k.node, row ? k.start : x, row ? y : k.start, row ? k.end - k.start : width, row ? height : k.end - k.start,
                      path.concat([k.index]))
            }
            for (const k of shared) {
                const r = { x: kids[k].start, y: y, width: kids[k + 1].end - kids[k].start, height: height }
                rects[kids[k].node.group] = r
                rects[kids[k + 1].node.group] = r
                sharing.push(kids[k].node.group, kids[k + 1].node.group)
                merged = true
            }
            for (let k = 0; k + 1 < kids.length; ++k) {
                if (shared.includes(k))
                    continue
                const a = kids[k]
                const b = kids[k + 1]
                const center = (a.end + b.start) / 2
                gutters.push({
                    vertical: !row, x: row ? center : x, y: row ? y : center, length: row ? height : width,
                    path: path, before: a.index, after: b.index, sizeBefore: a.end - a.start, sizeAfter: b.end - b.start
                })
            }
        }
        if (root)
            place(root, 0, 0, w, h, [])
        return { rects: rects, gutters: gutters, merged: merged, shared: sharing }
    }

    // ---- Dragging a grip or a tab ----

    function dragName(): string {
        if (!manager.dragged)
            return ""
        if (manager.draggedPanel !== "")
            return Workspaces.panelLabel(manager.draggedPanel)
        const g = manager.leafOf(manager.dragged.dockKey)
        const label = Workspaces.panelLabel(manager.shownPanel(manager.dragged.dockKey))
        return g && g.panels.length > 1 ? qsTr("%1 and its tabs").arg(label) : label
    }
    /// The whole group moves (its grip, or the tab of its only panel).
    function draggingWhole(): bool {
        const g = manager.dragged ? manager.leafOf(manager.dragged.dockKey) : null
        return manager.draggedPanel === "" || (g !== null && g.panels.length === 1)
    }

    function slotAt(p: point): DockSlot {
        for (const id of Object.keys(manager.docked)) {
            const s = manager.slotOf(id)
            if (!s || s.floating || !s.visible || s.width <= 0 || s.height <= 0)
                continue
            // Dragging a whole group: not onto itself.
            if (s === manager.dragged && manager.draggingWhole())
                continue
            const at = s.mapToItem(manager.area, 0, 0)
            if (p.x >= at.x && p.x < at.x + s.width && p.y >= at.y && p.y < at.y + s.height)
                return s
        }
        return null
    }

    /// The grip of `slot` (`panel` "") or the tab of its panel `panel` starts to be dragged.
    function beginDrag(slot: DockSlot, panel: string) {
        manager.dragged = slot
        manager.draggedPanel = panel
        manager.drop = { kind: "none" }
        manager.dropRect = Qt.rect(0, 0, 0, 0)
        manager.dragOutside = false
        manager.dragFloat = false
    }

    /// The pointer moved to `globalPoint` (screen coordinates) with `modifiers` held.
    function moveDrag(slot: DockSlot, globalPoint: point, modifiers: int) {
        if (slot !== manager.dragged)
            return
        manager.dragGlobal = globalPoint
        const inWindow = manager.mainWindow.contentItem.mapFromGlobal(globalPoint.x, globalPoint.y)
        manager.dragOutside = inWindow.x < 0 || inWindow.y < 0 || inWindow.x >= manager.mainWindow.width
                              || inWindow.y >= manager.mainWindow.height
        manager.dragPoint = manager.area.mapFromGlobal(globalPoint.x, globalPoint.y)
        manager.dragFloat = (modifiers & Qt.ControlModifier) !== 0
        manager.drop = manager.dropAt(manager.dragPoint)
        manager.dropRect = manager.previewOf(manager.drop)
    }

    /// What dropping at `p` (work-area coordinates) does (see the header).
    function dropAt(p: point): var {
        if (manager.dragOutside || manager.dragFloat)
            return { kind: "float" }
        const a = manager.area
        if (p.x < 0 || p.y < 0 || p.x >= a.width || p.y >= a.height)
            return { kind: "float" }
        if (p.x < manager.edgeBand)
            return { kind: "edge", side: "left" }
        if (p.x >= a.width - manager.edgeBand)
            return { kind: "edge", side: "right" }
        if (p.y < manager.edgeBand)
            return { kind: "edge", side: "top" }
        if (p.y >= a.height - manager.edgeBand)
            return { kind: "edge", side: "bottom" }
        const t = manager.slotAt(p)
        // Over a gutter: nothing happens there.
        if (!t)
            return { kind: "none" }
        const own = t === manager.dragged
        const at = t.mapToItem(a, 0, 0)
        // Its tabs (or the header of its one panel) and its middle: join it.
        if (p.y - at.y < manager.headerZone)
            return own ? { kind: "none" } : manager.besideShared({ kind: "join", target: t })
        const fx = (p.x - at.x) / t.width
        const fy = (p.y - at.y) / t.height
        const near = { left: fx, right: 1 - fx, top: fy, bottom: 1 - fy }
        let side = "left"
        for (const s of ["right", "top", "bottom"]) {
            if (near[s] < near[side])
                side = s
        }
        if (near[side] < manager.sideZone)
            return manager.besideShared({ kind: "beside", target: t, side: side })
        return own ? { kind: "none" } : manager.besideShared({ kind: "join", target: t })
    }
    /// A drop onto one of two monitors that share their place (sharedGroups, in left-right pairs): a tab there or a
    /// group above or below it would split them into two narrow columns, so it does nothing (the overlay says why); left
    /// of the pair or right of it, it docks beside the pair's outer monitor so they keep sharing.
    function besideShared(d: var): var {
        const i = manager.sharedGroups.indexOf(d.target.dockKey)
        if (i < 0 || (manager.dragged === d.target && manager.draggingWhole()))
            return d
        if (d.kind === "beside" && (d.side === "left" || d.side === "right")) {
            const pair = i - (i % 2)
            const outer = manager.slotOf(manager.sharedGroups[d.side === "left" ? pair : pair + 1])
            if (outer && outer !== manager.dragged)
                return { kind: "beside", target: outer, side: d.side }
        }
        return { kind: "none", shared: true }
    }

    /// Where a drop would put the panel, in the work area's coordinates.
    function previewOf(d: var): rect {
        const a = manager.area
        const g = Theme.gap
        if (d.kind === "join" || d.kind === "beside") {
            const at = d.target.mapToItem(a, 0, 0)
            const w = d.target.width
            const h = d.target.height
            if (d.kind === "join")
                return Qt.rect(at.x, at.y, w, h)
            switch (d.side) {
            case "left": return Qt.rect(at.x, at.y, Math.round((w - g) / 2), h)
            case "right": return Qt.rect(at.x + Math.round((w + g) / 2), at.y, w - Math.round((w + g) / 2), h)
            case "top": return Qt.rect(at.x, at.y, w, Math.round((h - g) / 2))
            }
            return Qt.rect(at.x, at.y + Math.round((h + g) / 2), w, h - Math.round((h + g) / 2))
        }
        if (d.kind === "edge") {
            const across = d.side === "left" || d.side === "right" ? a.width : a.height
            const band = Math.round(Math.max(Theme.space(40), Math.min(across * 0.3, Theme.space(100))))
            switch (d.side) {
            case "left": return Qt.rect(0, 0, band, a.height)
            case "right": return Qt.rect(a.width - band, 0, band, a.height)
            case "top": return Qt.rect(0, 0, a.width, band)
            }
            return Qt.rect(0, a.height - band, a.width, band)
        }
        if (d.kind === "float" && !manager.dragOutside && manager.dragged) {
            // The window it gets, where it would open (inside the main window, the overlay can show it).
            const p = manager.dragPoint
            const s = manager.dragged
            const w = Math.min(s.floating ? s.floatWindow.width : s.width, a.width / 2)
            const h = Math.min(s.floating ? s.floatWindow.height : s.height, a.height / 2)
            return Qt.rect(p.x - Theme.space(10), p.y - Theme.space(4), Math.max(Theme.space(40), w),
                           Math.max(Theme.space(30), h))
        }
        return Qt.rect(0, 0, 0, 0)
    }

    function endDrag(slot: DockSlot) {
        if (slot !== manager.dragged)
            return
        const d = manager.drop
        const at = manager.dragGlobal
        const whole = manager.draggingWhole()
        const key = manager.draggedPanel
        const gid = slot.dockKey
        // The panel that moves (a whole group: the one it shows): its group is the active one after the drop.
        const moved = key !== "" ? key : manager.shownPanel(gid)
        manager.dragged = null
        manager.draggedPanel = ""
        manager.drop = { kind: "none" }
        manager.dropRect = Qt.rect(0, 0, 0, 0)
        manager.dragOutside = false
        manager.dragFloat = false
        const place = Qt.rect(at.x - Theme.space(10), at.y - Theme.space(4), slot.width, slot.height)
        switch (d.kind) {
        case "float":
            if (whole && slot.floating) {
                slot.floatWindow.x = Math.round(place.x)
                slot.floatWindow.y = Math.round(place.y)
                manager.save()
            } else if (whole) {
                manager.popOutAt(slot, place)
            } else {
                const id = manager.detach(key)
                const own = manager.slotOf(id)
                if (own)
                    manager.popOutAt(own, place)
            }
            break
        case "join":
            if (whole)
                manager.joinGroup(gid, d.target.dockKey)
            else
                manager.joinPanel(key, d.target.dockKey)
            break
        case "beside":
            if (whole)
                manager.moveGroupBeside(gid, d.target.dockKey, d.side)
            else
                manager.movePanelBeside(key, d.target.dockKey, d.side)
            break
        case "edge":
            manager.putAtEdge(gid, whole ? "" : key, d.side)
            break
        }
        if (d.kind !== "none")
            manager.setActiveGroup(manager.groupOfPanel(moved)?.group ?? "")
    }

    // ---- Floating windows ----

    property Component floatComponent: Component {
        FloatingDock {}
    }

    /// Pops `slot` out into a window of its own, a little offset from where it is docked.
    function popOut(slot: DockSlot) {
        const g = slot.mapToGlobal(0, 0)
        manager.popOutAt(slot, Qt.rect(g.x + Theme.space(8), g.y + Theme.space(8), slot.width, slot.height))
    }

    function popOutAt(slot: DockSlot, rect: rect) {
        if (slot.floating)
            return
        // A maximized group leaving the work area gives the room back.
        if (manager.maximizedGroup === slot.dockKey)
            manager.maximizedGroup = ""
        const w = Math.max(480, Math.round(rect.width))
        const h = Math.max(320, Math.round(rect.height))
        const win = manager.floatComponent.createObject(manager.mainWindow, {
            slot: slot, x: Math.round(rect.x), y: Math.round(rect.y), width: w, height: h
        }) as FloatingDock
        if (!win)
            return
        slot.floatWindow = win
        slot.holder.parent = win.contentItem
        win.requestActivate()
        manager.save()
    }

    /// The slot's panels back in the main window (`keep`: the group stays where it was docked - it shows there, so a
    /// maximized group gives the room back - and the workspace remembers it; a group going away or moving elsewhere
    /// does neither: its caller does).
    function dockBack(slot: DockSlot, keep: bool) {
        const win = slot.floatWindow
        if (!win)
            return
        slot.holder.parent = slot
        slot.floatWindow = null
        win.visible = false
        win.destroy()
        if (keep) {
            manager.expose(slot.dockKey)
            manager.save()
            manager.mainWindow.requestActivate()
        }
    }

    /// The current workspace's layout back to its preset's (every panel docked again).
    function reset() {
        Workspaces.reset(manager.workspace)
    }

    // ---- Workspaces ----

    /// Shows the workspace `id`'s layout; returns its floating windows (Main.qml opens them once the editor shows).
    function load(id: int): var {
        for (const gid of Object.keys(manager.docked)) {
            const slot = manager.slotOf(gid)
            if (slot && slot.floating)
                manager.dockBack(slot, false)
        }
        const w = Workspaces.find(id)
        manager.workspace = id
        // Another layout: no group is active or maximized there yet.
        manager.activeGroup = ""
        manager.maximizedGroup = ""
        manager.tree = w ? Workspaces.copy(w.layout) : null
        manager.syncGroups()
        return w ? w.floating : ({})
    }

    function save() {
        if (manager.workspace < 0)
            return
        const floating = {}
        for (const gid of Object.keys(manager.docked)) {
            const w = manager.slotOf(gid)?.floatWindow
            if (w)
                floating[gid] = [w.x, w.y, w.width, w.height]
        }
        manager.saving = true
        Workspaces.update(manager.workspace, { layout: manager.tree, floating: floating })
        manager.saving = false
    }

    function restoreFloating(floating: var) {
        for (const gid of Object.keys(floating ?? {})) {
            const s = manager.slotOf(gid)
            const g = Workspaces.asList(floating[gid])
            if (s && g && g.length === 4 && g.every(v => typeof v === "number" && isFinite(v)) && manager.onScreen(g))
                manager.popOutAt(s, Qt.rect(g[0], g[1], g[2], g[3]))
        }
    }

    // Qt.application, untyped: its `screens` (Qt Quick's application object) is not on the QQmlApplication type qmllint knows.
    readonly property var application: Qt.application

    function onScreen(g: var): bool {
        for (const screen of manager.application.screens ?? []) {
            if (g[0] < screen.virtualX + screen.width && g[0] + g[2] > screen.virtualX
                    && g[1] < screen.virtualY + screen.height && g[1] + g[3] > screen.virtualY)
                return true
        }
        return false
    }
}
