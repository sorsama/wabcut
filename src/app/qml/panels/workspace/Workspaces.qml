pragma Singleton
import QtQuick

// Workspaces (0.1.3): the workspaces the title bar shows, in their order, each with its own panel layout. None is
// built in: every workspace is the user's, made from a preset layout (Edit, Color, Audio, Captions, Export) or as a
// copy of another one, and renamed, reordered or deleted at will (the last one stays). The first start makes one from
// the Edit preset.
//
// A workspace is a plain object (normalized() fills in anything missing):
//   { id, preset, name, layout, floating, volumeLanes }
//   * id: 100 and up. The ids 0-4 were the built-in workspaces (0 Edit, 1 Audio, 2 Export, 3 Color, 4 Captions); the
//     workspace keys, the welcome and Settings still use them to mean "the first workspace made from that preset"
//     (legacyPresets, Main.openPreset, which makes one when there is none).
//   * preset: the preset it was made from ("edit", "color", "audio", "captions" or "export"): Reset goes back to it.
//   * layout: the docking tree (DockManager): splits ({ split: "row" | "column", children, weights }) of panel groups
//     ({ group: id, panels: [catalog keys], current: the one shown }); a group shows its panels as tabs. null: every
//     panel is closed.
//   * floating: the groups in windows of their own, { group id: [x, y, width, height] }.
//   * volumeLanes: the timeline shows the audio volume rubber bands (the Audio preset).
// Main.qml lays the workbench out from the current one (DockManager loads its layout and keeps the changes here); the
// title bar's WorkspaceTabs create, rename, reorder, reset and delete them. Kept in Preferences.workspaceLayout as
// versioned JSON; version 1 (built-ins and column layouts) is read once: its own workspaces keep their panels.
QtObject {
    id: root

    readonly property int version: 2
    readonly property int firstId: 100
    readonly property var presetOrder: ["edit", "color", "audio", "captions", "export"]
    /// What the former built-in ids mean.
    readonly property var legacyPresets: ({ 0: "edit", 1: "audio", 2: "export", 3: "color", 4: "captions" })

    /// Every workspace in tab order. Replaced as a whole on every change, so bindings follow.
    property var all: []
    /// The ones the title bar shows (all of them).
    readonly property var shown: root.all

    // ---- The panel catalog ----
    // Every panel a layout can hold. `browser`: a rail panel (the rail opens it as a tab in the group of the browser
    // panels). `column`: where a closed panel opens again when nothing says otherwise ("left": with the browser panels,
    // "right": with the clip editors, "monitors": beside the other monitor, "timeline": along the bottom). `hue`: the dot
    // on its tab (design language v3; Theme.hue, "accent" follows the accent).
    readonly property var catalog: [
        { key: "library", label: qsTr("Library"), icon: "library", browser: true, column: "left", hue: "blue" },
        { key: "effects", label: qsTr("Effects"), icon: "sparkles", browser: true, column: "left", hue: "lime" },
        { key: "captions", label: qsTr("Transcript"), icon: "captions", browser: true, column: "left", hue: "pink" },
        { key: "audio", label: qsTr("Audio Tools"), icon: "audio-lines", browser: true, column: "left", hue: "mint" },
        { key: "essential", label: qsTr("Essential Sound"), icon: "sliders-horizontal", browser: true, column: "left", hue: "mint" },
        { key: "graphics", label: qsTr("Callouts"), icon: "layers", browser: true, column: "left", hue: "pink" },
        { key: "highlights", label: qsTr("Highlights"), icon: "zap", browser: true, column: "left", hue: "orange" },
        { key: "markers", label: qsTr("Markers"), icon: "bookmark", browser: true, column: "left", hue: "pink" },
        { key: "review", label: qsTr("Review"), icon: "message-square", browser: true, column: "left", hue: "blue" },
        { key: "shorts", label: qsTr("Shorts"), icon: "clapperboard", browser: true, column: "left", hue: "orange" },
        { key: "takes", label: qsTr("Takes"), icon: "repeat", browser: true, column: "left", hue: "violet" },
        { key: "generate", label: qsTr("Generate"), icon: "sparkles", browser: true, column: "left", hue: "violet" },
        { key: "scopes", label: qsTr("Scopes"), icon: "gauge", browser: false, column: "left", hue: "orange" },
        { key: "source", label: qsTr("Source Monitor"), icon: "film", browser: false, column: "monitors", hue: "violet" },
        { key: "program", label: qsTr("Program Monitor"), icon: "play", browser: false, column: "monitors", hue: "accent" },
        { key: "effectControls", label: qsTr("Effect Controls"), icon: "sliders-horizontal", browser: false, column: "right", hue: "lime" },
        { key: "frameInspector", label: qsTr("Explain This Frame"), icon: "search", browser: false, column: "right", hue: "blue" },
        { key: "captionStyle", label: qsTr("Caption Style"), icon: "type", browser: false, column: "right", hue: "pink" },
        { key: "mixer", label: qsTr("Audio Track Mixer"), icon: "volume-2", browser: false, column: "right", hue: "mint" },
        { key: "color", label: qsTr("Color"), icon: "contrast", browser: false, column: "right", hue: "orange" },
        { key: "export", label: qsTr("Export"), icon: "upload", browser: false, column: "right", hue: "lime" },
        { key: "timeline", label: qsTr("Timeline"), icon: "layers", browser: false, column: "timeline", hue: "violet" }
    ]
    readonly property var panelKeys: root.catalog.map(p => p.key)

    function panel(key: string): var {
        return root.catalog.find(p => p.key === key) ?? null
    }
    function panelLabel(key: string): string {
        return root.panel(key)?.label ?? ""
    }
    /// The colour of the panel's tab dot.
    function panelHue(key: string): color {
        return Theme.hue(root.panel(key)?.hue ?? "accent")
    }
    function isBrowserPanel(key: string): bool {
        return root.panel(key)?.browser === true
    }

    // ---- Presets: the layouts a workspace is made from ----
    function group(id: string, panels: var): var {
        return { group: id, panels: panels, current: panels[0] }
    }
    function presetLayout(preset: string): var {
        switch (preset) {
        case "color":
            // I.5: the Scopes, the Program Monitor and the Color panel; the monitor and the scopes get the height.
            return { split: "column", weights: [0.68, 0.32], children: [
                { split: "row", weights: [0.3, 0.4, 0.3],
                  children: [root.group("g1", ["scopes"]), root.group("g2", ["program"]), root.group("g3", ["color"])] },
                root.group("g4", ["timeline"])] }
        case "audio":
            // M5g: Audio Tools, the Program Monitor, the mixer wide; the timeline shows the volume rubber bands.
            return { split: "column", weights: [0.62, 0.38], children: [
                { split: "row", weights: [0.24, 0.3, 0.46],
                  children: [root.group("g1", ["audio"]), root.group("g2", ["program"]), root.group("g3", ["mixer"])] },
                root.group("g4", ["timeline"])] }
        case "captions":
            // C.7: the transcript and the cue list, the Program Monitor, the Caption Style panel.
            return { split: "column", weights: [0.5, 0.5], children: [
                { split: "row", weights: [0.27, 0.43, 0.3],
                  children: [root.group("g1", ["captions"]), root.group("g2", ["program"]), root.group("g3", ["captionStyle"])] },
                root.group("g4", ["timeline"])] }
        case "export":
            // The Program Monitor and the export settings, wide.
            return { split: "column", weights: [0.66, 0.34], children: [
                { split: "row", weights: [0.6, 0.4], children: [root.group("g1", ["program"]), root.group("g2", ["export"])] },
                root.group("g3", ["timeline"])] }
        }
        // Premiere's Editing workspace: the Library, Source and Program side by side, Effect Controls; the timeline.
        return { split: "column", weights: [0.5, 0.5], children: [
            { split: "row", weights: [0.22, 0.21, 0.21, 0.36],
              children: [root.group("g1", ["library"]), root.group("g2", ["source"]), root.group("g3", ["program"]),
                         root.group("g4", ["effectControls"])] },
            root.group("g5", ["timeline"])] }
    }
    function presetLabel(preset: string): string {
        switch (preset) {
        case "color": return qsTr("Color")
        case "audio": return qsTr("Audio")
        case "captions": return qsTr("Captions")
        case "export": return qsTr("Export")
        }
        return qsTr("Edit")
    }

    // ---- Reading ----
    function find(id: int): var {
        return root.all.find(w => w.id === id) ?? null
    }
    /// The workspace with this id; an unknown one is the first workspace (never null).
    function resolve(id: int): var {
        return root.find(id) ?? root.all[0] ?? root.normalized({ id: root.firstId, preset: "edit" })
    }
    function label(id: int): string {
        return root.find(id)?.name ?? ""
    }
    /// The first workspace made from `preset` (in tab order), or -1.
    function firstOf(preset: string): int {
        return root.all.find(w => w.preset === preset)?.id ?? -1
    }
    /// The former built-in id (0-4) of a workspace's preset: what the welcome and the tour compare with.
    function legacyIdOf(id: int): int {
        const preset = root.resolve(id).preset
        for (const k of Object.keys(root.legacyPresets)) {
            if (root.legacyPresets[k] === preset)
                return Number(k)
        }
        return 0
    }
    /// Where Wabcut starts for Preferences.startWorkspace: that workspace, or (a former built-in id) the first one made
    /// from its preset, else the first workspace; -1 when none is there yet (Main then makes one from the preset).
    function startId(saved: int): int {
        if (root.find(saved) !== null)
            return saved
        const preset = root.legacyPresets[saved]
        if (preset !== undefined)
            return root.firstOf(preset)
        return root.all[0]?.id ?? -1
    }
    /// The key in the tab's objectName ("workspaceTab_<key>"): the preset for the first workspace made from it,
    /// "custom<id>" otherwise.
    function keyOf(id: int): string {
        const w = root.find(id)
        if (w && root.firstOf(w.preset) === id)
            return w.preset
        return "custom" + id
    }
    /// The keymap command that opens it (Premiere's workspace keys; Export Media) for the first workspace of each preset,
    /// for the tooltips; "" for none.
    function commandOf(id: int): string {
        const w = root.find(id)
        if (!w || root.firstOf(w.preset) !== id)
            return ""
        switch (w.preset) {
        case "edit": return "window.workspace6"
        case "audio": return "window.workspace3"
        case "export": return "file.exportMedia"
        case "color": return "window.workspace5"
        case "captions": return "window.workspace4"
        }
        return ""
    }

    // ---- Layouts ----
    function isGroup(node: var): bool {
        return typeof node.group === "string"
    }
    /// A plain array copy of a list, or null. Lists read back from `all` can be sequence wrappers, which
    /// Array.isArray does not take for arrays.
    function asList(value: var): var {
        if (value === null || value === undefined || typeof value === "string" || typeof value.length !== "number")
            return null
        const out = []
        for (let i = 0; i < value.length; ++i)
            out.push(value[i])
        return out
    }
    function copy(value: var): var {
        return value === null || value === undefined ? null : JSON.parse(JSON.stringify(value))
    }
    /// The groups of a layout, in order.
    function groupsOf(node: var): var {
        if (!node)
            return []
        if (root.isGroup(node))
            return [node]
        let out = []
        for (const c of node.children)
            out = out.concat(root.groupsOf(c))
        return out
    }
    /// A layout (kept JSON, or one being edited) made well formed, or null when no panel is left: known panels, each
    /// once; groups with a unique id and the shown panel among theirs; splits of two or more children, each with a
    /// positive weight, none directly inside one running the same way (its children join the outer one).
    function normalizedLayout(raw: var): var {
        const seen = []
        const ids = []
        const fresh = []
        function walk(node, depth) {
            if (!node || typeof node !== "object" || depth > 12)
                return null
            if (typeof node.group === "string") {
                const panels = []
                for (const k of root.asList(node.panels) ?? []) {
                    if (typeof k === "string" && root.panelKeys.includes(k) && !seen.includes(k)) {
                        seen.push(k)
                        panels.push(k)
                    }
                }
                if (panels.length === 0)
                    return null
                const out = { group: node.group, panels: panels, current: panels.includes(node.current) ? node.current : panels[0] }
                if (/^g[0-9]{1,6}$/.test(node.group) && !ids.includes(node.group))
                    ids.push(node.group)
                else
                    fresh.push(out)
                return out
            }
            if (node.split !== "row" && node.split !== "column")
                return null
            const kids = root.asList(node.children) ?? []
            const weights = root.asList(node.weights) ?? []
            const out = { split: node.split, children: [], weights: [] }
            for (let i = 0; i < kids.length && i < 16; ++i) {
                const c = walk(kids[i], depth + 1)
                if (!c)
                    continue
                const w = typeof weights[i] === "number" && isFinite(weights[i]) && weights[i] > 0 ? weights[i] : 1
                if (!root.isGroup(c) && c.split === out.split) {
                    let sum = 0
                    for (const x of c.weights)
                        sum += x
                    for (let j = 0; j < c.children.length; ++j) {
                        out.children.push(c.children[j])
                        out.weights.push(w * c.weights[j] / sum)
                    }
                } else {
                    out.children.push(c)
                    out.weights.push(w)
                }
            }
            if (out.children.length === 0)
                return null
            return out.children.length === 1 ? out.children[0] : out
        }
        const result = walk(raw, 0)
        // Groups without a usable id get the next free ones.
        let next = 1
        for (const id of ids)
            next = Math.max(next, Number(id.slice(1)) + 1)
        for (const g of fresh)
            g.group = "g" + next++
        return result
    }
    /// The next free group id of a layout.
    function newGroupId(layout: var): string {
        let next = 1
        for (const g of root.groupsOf(layout))
            next = Math.max(next, Number(g.group.slice(1)) + 1)
        return "g" + next
    }
    /// Version 1's layout of a workspace (its left and right columns, the monitors and the timeline) as a layout.
    function layoutFromColumns(w: var, preset: string): var {
        const base = root.presetLayout(preset)
        const left = root.asList(w.left) ?? []
        const right = root.asList(w.right) ?? []
        const row = []
        if (left.length > 0)
            row.push({ group: "", panels: left, current: left[0] })
        if (w.source === true)
            row.push({ group: "", panels: ["source"], current: "source" })
        if (w.program !== false)
            row.push({ group: "", panels: ["program"], current: "program" })
        if (right.length > 0)
            row.push({ group: "", panels: right, current: right[0] })
        const children = []
        if (row.length > 0)
            children.push({ split: "row", children: row, weights: row.map(() => 1) })
        if (w.timeline !== false)
            children.push({ group: "", panels: ["timeline"], current: "timeline" })
        return root.normalizedLayout({ split: "column", children: children,
                                       weights: base.split === "column" ? base.weights : [1, 1] })
    }

    // ---- Normalizing ----
    /// The floating windows a workspace keeps, for groups its layout has.
    function cleanFloating(value: var, layout: var): var {
        const out = {}
        if (!value || typeof value !== "object")
            return out
        const ids = root.groupsOf(layout).map(g => g.group)
        for (const id of ids) {
            const g = root.asList(value[id])
            if (g && g.length === 4 && g.every(v => typeof v === "number" && isFinite(v)))
                out[id] = g
        }
        return out
    }
    /// A complete, valid workspace from stored or edited data: a known preset, a name, its layout (the preset's when
    /// it has none; null when every panel is closed) and the floating windows of its groups.
    function normalized(raw: var): var {
        const preset = root.presetOrder.includes(raw.preset) ? raw.preset : "edit"
        const name = typeof raw.name === "string" ? raw.name.trim().slice(0, 40) : ""
        const layout = raw.layout === undefined ? root.presetLayout(preset) : root.normalizedLayout(raw.layout)
        return {
            id: raw.id,
            preset: preset,
            name: name !== "" ? name : root.presetLabel(preset),
            layout: layout,
            floating: root.cleanFloating(raw.floating, layout),
            volumeLanes: typeof raw.volumeLanes === "boolean" ? raw.volumeLanes : preset === "audio"
        }
    }

    // ---- Changing ----
    function commit(list: var) {
        root.all = list
        root.save()
    }
    function replace(id: int, w: var) {
        root.commit(root.all.map(x => x.id === id ? root.normalized(w) : x))
    }
    /// Merges `patch` into the workspace.
    function update(id: int, patch: var) {
        const w = root.find(id)
        if (!w)
            return
        root.replace(id, Object.assign({}, w, patch))
    }
    function nextId(): int {
        let id = root.firstId
        for (const w of root.all)
            id = Math.max(id, w.id + 1)
        return id
    }
    function uniqueName(name: string): string {
        const taken = root.all.map(w => w.name)
        if (!taken.includes(name))
            return name
        for (let n = 2; ; ++n) {
            const candidate = qsTr("%1 %2").arg(name).arg(n)
            if (!taken.includes(candidate))
                return candidate
        }
    }
    /// A new workspace laid out like `fromId` (its panels, their places and sizes, its windows), right after it;
    /// returns its id.
    function create(fromId: int, name: string): int {
        const from = root.resolve(fromId)
        const id = root.nextId()
        const w = root.normalized(Object.assign(root.copy(from), {
            id: id, name: root.uniqueName(name !== "" ? name : qsTr("%1 copy").arg(from.name))
        }))
        const list = root.all.slice()
        const at = list.findIndex(x => x.id === from.id)
        list.splice(at < 0 ? list.length : at + 1, 0, w)
        root.commit(list)
        return id
    }
    /// A new workspace with a preset's layout, at the end (named after the preset unless `name` says otherwise);
    /// returns its id.
    function createFromPreset(preset: string, name: string): int {
        const id = root.nextId()
        const w = root.normalized({ id: id, preset: preset,
                                    name: root.uniqueName(name !== "" ? name : root.presetLabel(preset)) })
        root.commit(root.all.concat([w]))
        return id
    }
    function rename(id: int, name: string) {
        const n = name.trim().slice(0, 40)
        if (n === "" || !root.find(id))
            return
        root.update(id, { name: n })
    }
    /// Deletes a workspace; the last one stays.
    function remove(id: int) {
        if (root.all.length <= 1)
            return
        root.commit(root.all.filter(w => w.id !== id))
    }
    /// Moves a workspace to `index` among the tabs.
    function moveTo(id: int, index: int) {
        const w = root.find(id)
        if (!w)
            return
        const rest = root.all.filter(x => x.id !== id)
        rest.splice(Math.max(0, Math.min(rest.length, index)), 0, w)
        if (JSON.stringify(rest.map(x => x.id)) !== JSON.stringify(root.all.map(x => x.id)))
            root.commit(rest)
    }
    function move(id: int, delta: int) {
        const index = root.all.findIndex(w => w.id === id)
        if (index >= 0)
            root.moveTo(id, index + delta)
    }
    /// A workspace's layout back to its preset's (every panel docked again).
    function reset(id: int) {
        const w = root.find(id)
        if (!w)
            return
        root.replace(id, { id: id, preset: w.preset, name: w.name })
    }

    // ---- Keeping ----
    function save() {
        Preferences.workspaceLayout = JSON.stringify({ version: root.version, workspaces: root.all })
    }
    function restore() {
        let saved = null
        try {
            saved = Preferences.workspaceLayout !== "" ? JSON.parse(Preferences.workspaceLayout) : null
        } catch (e) {
            saved = null
        }
        const list = []
        const seen = []
        const entries = (saved ? root.asList(saved.workspaces) : null) ?? []
        for (const entry of entries) {
            if (!entry || typeof entry.id !== "number" || !Number.isInteger(entry.id) || entry.id < root.firstId
                    || seen.includes(entry.id))
                continue // version 1's built-ins (0-4) are gone
            seen.push(entry.id)
            if (saved.version === root.version) {
                list.push(root.normalized(entry))
            } else {
                const preset = root.presetOrder.includes(entry.preset) ? entry.preset : "edit"
                list.push(root.normalized({ id: entry.id, preset: preset, name: entry.name,
                                            layout: root.layoutFromColumns(entry, preset), volumeLanes: entry.volumeLanes }))
            }
        }
        // Never none: the first start (or a list that lost them all) gets one from the Edit preset.
        if (list.length === 0)
            list.push(root.normalized({ id: root.firstId, preset: "edit" }))
        root.all = list
        if (!saved || saved.version !== root.version)
            root.save()
    }

    Component.onCompleted: root.restore()
}
