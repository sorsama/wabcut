pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models

// ● EFFECTS (UX rework 2026-10-23, docs/effects.md "The Effects panel"): Premiere's Effects panel as a folder tree -
// Favorites, Presets, Lumetri Presets, Audio Effects, Audio Transitions, Video Effects, Video Transitions, Audio
// Plug-ins, then the custom bins - in three views (List, Compact, Icons), under a toolbar: the search, the view switch,
// the filter menu, New Custom Bin and the panel menu. The rows come from EffectsTree (C++: the visible rows in
// Premiere's order, the nested presets bins, the folders' open state kept between runs, the search and the filter);
// this file draws them and handles the mouse, the keyboard, the menus, the drops and the dialogs.
//   * Double-click or Enter on an item applies it to every selected clip (one undo step). Drag it onto the timeline,
//     Effect Controls, a mixer strip, or a custom bin here; a preset of yours dragged onto a presets bin moves there; a
//     .wabcut-presets.json file dropped from Explorer is imported (into the presets bin it lands on).
//   * Keyboard: Up/Down through the rows, Left/Right close or open a folder or go to its parent, Enter applies or opens,
//     F2 renames a custom bin or one of your presets bins, Delete (and Backspace on a folder) deletes what can be deleted
//     after asking, Menu or Shift+F10 opens the row's menu, Ctrl+/ makes a custom bin (keymap). Down in the search goes
//     to the list and Up at its top comes back.
//   * The view is kept in Preferences (effects/view), the folders' state by EffectsTree, the plug-in grouping by Plugins.
Panel {
    id: root
    objectName: "effectsPanel"

    KeymapScope.scope: "effects"

    title: qsTr("Effects")

    // Delete anywhere in the panel belongs to the panel, the toolbar's buttons included (a click leaves the keyboard on
    // them): the keymap's Clear would delete the timeline's selected clips. Text fields take it first; the list
    // handles it itself.
    Keys.onShortcutOverride: event => event.accepted = root.claimsDelete(event)
    Keys.onPressed: event => {
        if (!root.claimsDelete(event))
            return
        event.accepted = true
        root.keyboardFocus = true
        root.deleteRow(root.currentKey)
    }

    // ---- State ----
    /// "list", "compact" or "icons" (Preferences.effectsView).
    readonly property string view: ["list", "compact", "icons"].includes(Preferences.effectsView) ? Preferences.effectsView : "list"
    /// The filter menu's choice (EffectsTree.filter): "all", "video", "audio", "transitions", "presets", "favorites",
    /// "plugins". Not kept between runs.
    property string filterKey: "all"
    /// The selected row: a folder key or an item's "<folderKey>|<itemId>" (EffectsTree row keys), kept across changes.
    property string currentKey: ""
    /// The folders around the selected row (innermost first): the selection falls back on them when its row goes.
    property var currentAncestors: []
    property int lastIndex: 0
    /// The folder renamed in place ("" = none).
    property string renamingKey: ""
    /// The status line (it clears after a few seconds).
    property string status: ""
    /// The selection shows the focus ring: the list has the focus and the keyboard moved last.
    property bool keyboardFocus: false
    property bool pointerFocus: false
    readonly property bool showFocus: list.activeFocus && keyboardFocus
    /// The list has the keyboard: the selected row is the accessible focus (screen readers follow it as it moves).
    readonly property bool treeFocused: list.activeFocus
    /// In the Icons view, the tiles row of the selected tile and its column: a deleted tile hands over to its neighbour.
    property string currentTilesKey: ""
    property int currentColumn: 0
    readonly property bool searching: tree.searching
    /// The default video and audio transitions (stream T's Editing settings): DEFAULT on their rows.
    readonly property var defaultTransitions: [Editing.defaultVideoTransition, Editing.defaultAudioTransition]
    /// This panel asked for a new custom bin (it names the bin in place when the bridge says it is made).
    property bool expectingBin: false
    /// A drag over the list: the row under it and the folder it would go to.
    property string dropRowKey: ""
    property string dropFolderKey: ""
    readonly property string effectMime: "application/x-wabcut-effect"

    // ---- Metrics of the three views ----
    readonly property int rowHeight: view === "compact" ? Theme.space(6) : Theme.controlHeight
    readonly property int indentStep: view === "compact" ? Theme.space(3) : Theme.space(4)
    readonly property int textSize: view === "compact" ? Theme.textSm : Theme.textMd
    readonly property int iconSize: view === "compact" ? Theme.iconXs : Theme.iconSm
    readonly property int tileGap: Theme.space(1.5)
    readonly property int tileHeight: Theme.space(21)
    readonly property real tileArea: Math.max(Theme.space(22), list.width - 2 * indentStep - Theme.space(2))
    readonly property int tileColumns: Math.max(1, Math.floor((tileArea + tileGap) / (Theme.space(22) + tileGap)))
    readonly property real tileWidth: Math.floor((tileArea - (tileColumns - 1) * tileGap) / tileColumns)

    // Focus target for the Effects shortcut (Shift+7) and the Window menu: the search.
    function focusContent() {
        search.forceActiveFocus(Qt.TabFocusReason)
    }

    function showStatus(message: string) {
        root.status = message
        if (message !== "")
            statusTimer.restart()
        else
            statusTimer.stop()
    }
    function report(error: string, done: string) {
        root.showStatus(error !== "" ? error : done)
    }

    /// Whether the keyboard focus is in this panel (the keymap's requests go to the panel the user is in).
    function holdsFocus(): bool {
        for (let item = root.Window.activeFocusItem; item; item = item.parent) {
            if (item === root)
                return true
        }
        return false
    }

    // ---- Rows ----
    // The functions that read the row maps EffectsTree.get() returns are untyped on purpose: Qt 6.11's AOT
    // compiler turns a property read on such a map into a lookup on QVariant's (null) meta-object, which crashed
    // the first drop on a folder. Untyped, the engine interprets them.
    /// The row (or Icons-view tile) with key `key`, a map of its EffectsTree roles; {} when it does not show.
    function rowData(key) {
        const i = tree.indexOf(key)
        if (i < 0)
            return ({})
        const r = tree.get(i)
        if (r.rowType !== "tiles")
            return r
        const tiles = r.tiles
        for (let k = 0; k < tiles.length; ++k) {
            if (tiles[k].rowKey === key)
                return tiles[k]
        }
        return ({})
    }
    /// The key of row `i` (in a tiles row, of its tile `column`, the last one when it has fewer).
    function keyAt(i, column) {
        const r = tree.get(i)
        if (r.rowType !== "tiles")
            return r.rowKey ?? ""
        const tiles = r.tiles
        return tiles.length > 0 ? tiles[Math.max(0, Math.min(column, tiles.length - 1))].rowKey : ""
    }
    /// The tile column of `key` (0 outside the Icons view).
    function columnOf(key) {
        const r = tree.get(tree.indexOf(key))
        if (r.rowType !== "tiles")
            return 0
        const tiles = r.tiles
        for (let k = 0; k < tiles.length; ++k) {
            if (tiles[k].rowKey === key)
                return k
        }
        return 0
    }
    /// The name of item `itemId` as the panel shows it.
    function itemName(itemId) {
        const i = tree.indexOfItem(itemId)
        if (i >= 0) {
            const r = tree.get(i)
            if (r.rowType !== "tiles")
                return r.name
            const tiles = r.tiles
            for (let k = 0; k < tiles.length; ++k) {
                if (tiles[k].itemId === itemId)
                    return tiles[k].name
            }
        }
        if (itemId.startsWith("preset:"))
            return itemId.slice(7)
        // Not on show (its folder is closed): its name in a custom bin it went into.
        const groups = EffectPresets.customGroups("")
        for (let g = 0; g < groups.length; ++g) {
            const bins = groups[g].bins
            for (let b = 0; b < bins.length; ++b) {
                const items = bins[b].items
                for (let k = 0; k < items.length; ++k) {
                    if (items[k].id === itemId)
                        return items[k].name
                }
            }
        }
        return itemId
    }

    /// Selects `key` (shows it; an item becomes EffectPresets' current item, which Backspace deletes).
    function select(key) {
        root.currentKey = key
        const i = tree.indexOf(key)
        if (i < 0)
            return
        root.lastIndex = i
        const ancestors = []
        for (let p = tree.parentIndex(i); p >= 0; p = tree.parentIndex(p))
            ancestors.push(tree.get(p).rowKey)
        root.currentAncestors = ancestors
        root.noteTile(i)
        list.positionViewAtIndex(i, ListView.Contain)
        const r = root.rowData(key)
        if (r.rowType === "item")
            root.makeCurrentItem(r)
        else
            root.makeCurrentItem(({}))
    }
    /// Remembers the tiles row (Icons view) and the column of the selection, which shows at row `i`.
    function noteTile(i) {
        const shown = tree.get(i)
        root.currentTilesKey = shown.rowType === "tiles" ? shown.rowKey : ""
        root.currentColumn = root.columnOf(root.currentKey)
    }
    function makeCurrentItem(r) {
        EffectPresets.currentItem = r.item ?? ""
        EffectPresets.currentBin = (r.removable ?? false) ? r.customBin : ""
    }
    /// A press on a row: it is selected and the list takes the keyboard (no focus ring: the mouse did it).
    function pressRow(key: string) {
        root.keyboardFocus = false
        root.pointerFocus = true
        list.forceActiveFocus(Qt.MouseFocusReason)
        root.pointerFocus = false
        root.select(key)
    }
    /// After the rows changed: a selected row that went (deleted, its folder closed) hands over to its folder or the
    /// row now in its place; in the Icons view a tile that went hands over to the tile now in its place (or the last one
    /// before it) while its folder still shows tiles. A search only hides rows for a while: the selection waits for them.
    /// Runs after every model change: a tiles row that keeps its key only changes its data.
    function keepCurrent() {
        if (root.currentKey === "")
            return
        const i = tree.indexOf(root.currentKey)
        if (i >= 0) { // still there: note where it is now (the view or the number of columns may have changed)
            root.noteTile(i)
            return
        }
        if (tree.searching || root.filterKey !== "all")
            return
        const tilesKey = root.currentTilesKey
        const hash = tilesKey.lastIndexOf("#")
        if (hash > 0) {
            const base = tilesKey.slice(0, hash + 1)
            const n = parseInt(tilesKey.slice(hash + 1))
            for (let k = n; k >= 0; --k) {
                const t = tree.indexOf(base + k)
                if (t >= 0) {
                    root.select(root.keyAt(t, k === n ? root.currentColumn : 1000))
                    return
                }
            }
        }
        const ancestors = root.currentAncestors
        for (let k = 0; k < ancestors.length; ++k) {
            if (tree.indexOf(ancestors[k]) >= 0) {
                root.select(ancestors[k])
                return
            }
        }
        if (tree.count > 0)
            root.select(root.keyAt(Math.min(root.lastIndex, tree.count - 1), 0))
        else
            root.currentKey = ""
    }

    // ---- Keyboard ----
    /// Delete (Shift too, not Ctrl/Alt/Meta) outside an in-place rename: the panel's, wherever its focus is.
    function claimsDelete(event): bool {
        return event.key === Qt.Key_Delete && root.renamingKey === ""
               && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
    }
    function claimsKey(event) {
        if (root.renamingKey !== "")
            return false
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
            return false
        switch (event.key) {
        case Qt.Key_Up:
        case Qt.Key_Down:
        case Qt.Key_Left:
        case Qt.Key_Right:
        case Qt.Key_Home:
        case Qt.Key_End:
        case Qt.Key_PageUp:
        case Qt.Key_PageDown:
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Delete: // the keymap's Clear would delete the timeline's selected clips
        case Qt.Key_F2:
        case Qt.Key_Menu:
            return true
        case Qt.Key_F10:
            return (event.modifiers & Qt.ShiftModifier) !== 0
        case Qt.Key_Backspace: // on an item it stays the keymap's Delete Custom Item (it asks through the panel)
            return (root.rowData(root.currentKey).rowType ?? "") === "folder"
        default:
            return false
        }
    }
    function handleKey(event) {
        if (!root.claimsKey(event))
            return false
        root.keyboardFocus = true
        const key = event.key
        if (tree.count === 0)
            return true
        if (tree.indexOf(root.currentKey) < 0) // a selection whose row just went hands over first
            root.keepCurrent()
        if (tree.indexOf(root.currentKey) < 0) { // nothing selected yet: the first row
            if (key !== Qt.Key_Delete && key !== Qt.Key_F2 && key !== Qt.Key_Return && key !== Qt.Key_Enter)
                root.select(root.keyAt(key === Qt.Key_End ? tree.count - 1 : 0, 0))
            return true
        }
        switch (key) {
        case Qt.Key_Up:
            root.moveBy(-1)
            break
        case Qt.Key_Down:
            root.moveBy(1)
            break
        case Qt.Key_PageUp:
            root.moveBy(-root.pageRows())
            break
        case Qt.Key_PageDown:
            root.moveBy(root.pageRows())
            break
        case Qt.Key_Home:
            root.select(root.keyAt(0, 0))
            break
        case Qt.Key_End:
            root.select(root.keyAt(tree.count - 1, 1000))
            break
        case Qt.Key_Left:
            root.goLeft()
            break
        case Qt.Key_Right:
            root.goRight()
            break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            root.activate(root.currentKey)
            break
        case Qt.Key_Delete:
        case Qt.Key_Backspace:
            root.deleteRow(root.currentKey)
            break
        case Qt.Key_F2:
            root.startRename(root.currentKey)
            break
        case Qt.Key_Menu:
        case Qt.Key_F10:
            root.openMenuAtCurrent()
            break
        }
        return true
    }
    function pageRows(): int {
        return Math.max(1, Math.floor(list.height / root.rowHeight) - 1)
    }
    function moveBy(delta) {
        const i = tree.indexOf(root.currentKey)
        const column = root.columnOf(root.currentKey)
        if (delta < 0 && i === 0) { // Up at the top: back to the search
            search.forceActiveFocus(Qt.BacktabFocusReason)
            return
        }
        root.select(root.keyAt(Math.max(0, Math.min(tree.count - 1, i + delta)), column))
    }
    function goLeft() {
        const i = tree.indexOf(root.currentKey)
        const r = tree.get(i)
        if (r.rowType === "folder" && r.expanded) {
            tree.setExpanded(r.rowKey, false)
            return
        }
        if (r.rowType === "tiles") {
            const column = root.columnOf(root.currentKey)
            if (column > 0) {
                root.select(r.tiles[column - 1].rowKey)
                return
            }
        }
        const p = tree.parentIndex(i)
        if (p >= 0)
            root.select(tree.get(p).rowKey)
    }
    function goRight() {
        const i = tree.indexOf(root.currentKey)
        const r = tree.get(i)
        if (r.rowType === "folder") {
            if (!r.expanded)
                tree.setExpanded(r.rowKey, true)
            else if (i + 1 < tree.count && tree.get(i + 1).depth > r.depth)
                root.select(root.keyAt(i + 1, 0))
            return
        }
        if (r.rowType === "tiles") {
            const column = root.columnOf(root.currentKey)
            if (column + 1 < r.tiles.length)
                root.select(r.tiles[column + 1].rowKey)
        }
    }

    // ---- Actions ----
    function toggleFolder(key: string) {
        tree.toggle(key)
    }
    /// Enter / double-click: a folder opens or closes; an item goes on every selected clip.
    function activate(key) {
        const r = root.rowData(key)
        if (r.rowType === "folder") {
            tree.toggle(key)
            return
        }
        if (r.rowType !== "item")
            return
        if ((r.problem ?? "") !== "") {
            root.showStatus(r.problem)
            return
        }
        const result = tree.applyToSelection(r.itemId)
        root.showStatus(result.message ?? result.error ?? "")
    }
    function openMenu(key, target, x, y) {
        const r = root.rowData(key)
        if ((r.rowType ?? "") !== "")
            itemMenu.popupFor(r, target, x, y)
    }
    /// Menu or Shift+F10: the selected row's menu, under its name.
    function openMenuAtCurrent() {
        const i = tree.indexOf(root.currentKey)
        if (i < 0)
            return
        list.positionViewAtIndex(i, ListView.Contain)
        const delegate = list.itemAtIndex(i)
        if (!delegate)
            return
        const depth = tree.get(i).depth ?? 0
        const x = root.view === "icons" && tree.get(i).rowType === "tiles"
                ? Math.min(depth, 2) * root.indentStep + root.columnOf(root.currentKey) * (root.tileWidth + root.tileGap) + Theme.space(4)
                : depth * root.indentStep + Theme.space(10)
        root.openMenu(root.currentKey, delegate, x, Math.min(delegate.height, root.rowHeight))
    }

    // Rename in place: custom bins and the user's presets bins.
    function startRename(key) {
        const r = root.rowData(key)
        if (r.rowType !== "folder")
            return
        if (!r.renamable) {
            root.showStatus(qsTr("Only custom bins and your presets bins can be renamed"))
            return
        }
        root.select(key)
        root.renamingKey = key
    }
    /// Keeps the name typed in place. `focusLost`: the field lost the keyboard (a click on the search or another panel);
    /// the keyboard then stays where the user put it, and comes back to the list only when nothing has it.
    function commitRename(text, focusLost) {
        const key = root.renamingKey
        if (key === "")
            return
        root.renamingKey = ""
        const r = root.rowData(key)
        const name = text.trim()
        const holder = root.Window.activeFocusItem
        if (!focusLost || !holder || holder === root.Window.contentItem)
            list.forceActiveFocus(Qt.OtherFocusReason)
        if (name === "" || name === r.title)
            return
        const error = (r.customBin ?? "") !== "" ? EffectPresets.renameCustomBin(r.customBin, name)
                                                 : EffectPresets.renamePresetsBin(r.presetsBin, name)
        root.report(error, qsTr("Renamed %1 to %2").arg(r.title).arg(name))
    }
    function cancelRename() {
        root.renamingKey = ""
        list.forceActiveFocus(Qt.OtherFocusReason)
    }

    // New bins: shown, selected and named in place.
    function newCustomBin(item: string) {
        root.expectingBin = true
        if (EffectPresets.newCustomBin(item) === "")
            root.expectingBin = false // the bridge's statusMessage says why
    }
    function newPresetsBin(parentPath) {
        const existing = EffectPresets.presetBins
        const base = qsTr("Presets Bin")
        const pathOf = name => parentPath === "" ? name : parentPath + "/" + name
        const taken = path => {
            for (let k = 0; k < existing.length; ++k) {
                if (existing[k].toLowerCase() === path.toLowerCase())
                    return true
            }
            return false
        }
        let name = base
        for (let n = 2; taken(pathOf(name)); ++n)
            name = base + " " + n
        const path = pathOf(name)
        const error = EffectPresets.newPresetsBin(path)
        if (error !== "") {
            root.showStatus(error)
            return
        }
        Qt.callLater(() => root.revealNew(EffectPresets.presetsBinKey(path), qsTr("Made %1").arg(name)))
    }
    function revealNew(key, message) {
        if (tree.indexOf(key) < 0 && (tree.searching || root.filterKey !== "all")) {
            search.text = ""
            root.filterKey = "all"
        }
        if (tree.reveal(key) < 0)
            return
        root.select(key)
        root.keyboardFocus = true
        list.forceActiveFocus(Qt.OtherFocusReason)
        root.renamingKey = key
        root.showStatus(message)
    }
    /// Shows item `itemId` in its own folder and selects it (after an import: the first preset imported).
    function revealItemId(itemId) {
        const i = tree.revealItem(itemId)
        if (i < 0)
            return
        const r = tree.get(i)
        if (r.rowType !== "tiles") {
            root.select(r.rowKey)
            return
        }
        const tiles = r.tiles
        for (let k = 0; k < tiles.length; ++k) {
            if (tiles[k].itemId === itemId) {
                root.select(tiles[k].rowKey)
                return
            }
        }
    }

    // Deletes ask first (EffectsConfirmDialog); library changes are not undo steps.
    function deleteRow(key) {
        const r = root.rowData(key)
        if (r.rowType === "folder") {
            if (!r.deletable) {
                root.showStatus(qsTr("%1 comes with Wabcut and cannot be deleted").arg(r.title))
                return
            }
            if ((r.customBin ?? "") !== "") {
                const bin = r.customBin
                confirmDialog.ask(qsTr("Delete the custom bin %1? The effects in it stay in their own folders.").arg(bin),
                                  qsTr("Delete"),
                                  () => root.report(EffectPresets.deleteCustomItem({ customBin: bin }), qsTr("Deleted %1").arg(bin)))
                return
            }
            const path = r.presetsBin
            const title = r.title
            const presets = r.userPresets
            confirmDialog.ask(presets === 1 ? qsTr("Delete the presets bin %1 and the preset in it? It leaves your custom bins and Favorites too.").arg(title)
                              : presets > 1 ? qsTr("Delete the presets bin %1 and the %2 presets in it? They leave your custom bins and Favorites too.").arg(title).arg(presets)
                              : qsTr("Delete the presets bin %1?").arg(title),
                              qsTr("Delete"),
                              () => root.report(EffectPresets.deleteCustomItem({ presetsBin: path }), qsTr("Deleted %1").arg(title)))
            return
        }
        if (r.rowType !== "item")
            return
        root.makeCurrentItem(r)
        const info = EffectPresets.currentItemInfo()
        if (!info.deletable) {
            root.showStatus(info.reason)
            return
        }
        root.askDelete(info)
    }
    /// The confirmation for EffectPresets.currentItemInfo() (a preset of yours, or an item out of a custom bin).
    function askDelete(info) {
        confirmDialog.ask(info.question, info.confirmText, () => {
            EffectPresets.currentItem = info.item
            EffectPresets.currentBin = info.bin
            root.report(EffectPresets.deleteCurrentItem(),
                        info.action === "removeFromBin" ? qsTr("Removed %1 from %2").arg(info.name).arg(info.bin)
                                                        : qsTr("Deleted %1").arg(info.name))
        })
    }

    // ---- Drops ----
    /// The folder a drop on row `key` goes to: the folder itself, or the one an item or a tiles row is in.
    function dropFolder(key) {
        const i = tree.indexOf(key)
        if (i < 0)
            return ({})
        const r = tree.get(i)
        if (r.rowType === "folder")
            return r
        const p = tree.parentIndex(i)
        return p >= 0 ? tree.get(p) : ({})
    }
    function presetsFilesIn(urls) {
        const files = []
        for (let k = 0; k < (urls ? urls.length : 0); ++k) {
            if (EffectPresets.isPresetsFile(urls[k]))
                files.push(urls[k])
        }
        return files
    }
    /// Whether folder `target` takes the dragged item `itemId`.
    function takes(target, itemId) {
        if (itemId === "")
            return false
        if (target.canHoldItems ?? false)
            return true
        if ((target.canHoldPresets ?? false) && itemId.startsWith("preset:")) {
            const preset = EffectPresets.preset(itemId.slice(7))
            return preset.builtIn === false && (preset.bin ?? "").toLowerCase() !== (target.presetsBin ?? "").toLowerCase()
        }
        return false
    }
    function dropEnter(key, drag) {
        const target = root.dropFolder(key)
        let ok = false
        if (drag.hasUrls)
            ok = (target.canHoldPresets ?? false) && root.presetsFilesIn(drag.urls).length > 0
        else if (drag.formats.indexOf(root.effectMime) >= 0)
            ok = root.takes(target, drag.getDataAsString(root.effectMime))
        root.dropRowKey = ok ? key : ""
        root.dropFolderKey = ok ? (target.rowKey ?? "") : ""
        return ok
    }
    function dropLeave(key) {
        if (root.dropRowKey === key) {
            root.dropRowKey = ""
            root.dropFolderKey = ""
        }
    }
    // The library changes after the drop event: they change the rows under the pointer, the DropArea's own too.
    function dropOn(key, drop) {
        const target = root.dropFolder(key)
        root.dropRowKey = ""
        root.dropFolderKey = ""
        if (drop.hasUrls) {
            const files = root.presetsFilesIn(drop.urls)
            const bin = (target.canHoldPresets ?? false) ? target.presetsBin : undefined
            drop.accept(Qt.CopyAction)
            Qt.callLater(() => root.importFiles(files, bin))
            return
        }
        const itemId = drop.getDataAsString(root.effectMime)
        drop.accept(Qt.CopyAction)
        Qt.callLater(() => root.takeDropped(target, itemId))
    }
    /// A dropped effect, transition, preset or plug-in into folder `target`: a custom bin keeps it, a presets bin takes
    /// a preset of yours (moved there).
    function takeDropped(target, itemId) {
        if (target.canHoldItems ?? false) {
            root.report(EffectPresets.addToCustomBin(target.customBin, itemId),
                        qsTr("Added %1 to %2").arg(root.itemName(itemId)).arg(target.customBin))
        } else if ((target.canHoldPresets ?? false) && itemId.startsWith("preset:")) {
            const name = itemId.slice(7)
            root.report(EffectPresets.movePreset(name, target.presetsBin), qsTr("Moved %1 to %2").arg(name).arg(target.title))
        }
    }
    /// Presets files dropped from Explorer: into presets bin `bin`, or (undefined) each preset into its own.
    function importFiles(files, bin) {
        presetFiles.importBin = bin
        presetFiles.acceptImport(files)
    }

    // ---- The model ----
    EffectsTree {
        id: tree
        objectName: "effectsTree"
        query: search.text
        filter: root.filterKey
        showPlugins: Preferences.effectsShowPlugins
        columns: root.view === "icons" ? root.tileColumns : 1
    }

    Connections {
        target: tree
        // Any change may take the selected row away: removed rows, and in the Icons view a tiles row whose tiles changed
        // (it keeps its key, so only its data changes).
        function onRowsRemoved() {
            Qt.callLater(root.keepCurrent)
        }
        function onRowsInserted() {
            Qt.callLater(root.keepCurrent)
        }
        function onRowsMoved() {
            Qt.callLater(root.keepCurrent)
        }
        function onDataChanged() {
            Qt.callLater(root.keepCurrent)
        }
        function onModelReset() {
            Qt.callLater(root.keepCurrent)
        }
        function onQueryChanged() {
            Qt.callLater(() => {
                const i = tree.indexOf(root.currentKey)
                if (!tree.searching && i >= 0)
                    list.positionViewAtIndex(i, ListView.Contain)
                else
                    list.positionViewAtBeginning()
            })
        }
    }

    Connections {
        target: EffectPresets
        // Any New Custom Bin (the panel menu, an item's Add to Custom Bin, Ctrl+/): the panel the user is in names it.
        function onCustomBinCreated(name: string) {
            if (!root.expectingBin && !root.holdsFocus())
                return
            root.expectingBin = false
            const key = EffectPresets.customBinKey(name)
            Qt.callLater(() => root.revealNew(key, qsTr("Made %1").arg(name)))
        }
        // Backspace (the keymap's Delete Custom Item): ask first.
        function onDeleteCurrentItemRequested(info: var) {
            if (root.holdsFocus() || root.visible)
                root.askDelete(info)
        }
        function onStatusMessage(text: string) {
            if (root.visible)
                root.showStatus(text)
        }
        // A renamed bin has a new key: the selection follows it.
        function onBinRenamed(oldKey: string, newKey: string) {
            const k = root.currentKey
            const custom = oldKey.startsWith("custom:")
            if (k === oldKey || k.startsWith(oldKey + "|") || k.startsWith(oldKey + "#") || (!custom && k.startsWith(oldKey + "/"))) {
                root.currentKey = newKey + k.slice(oldKey.length)
                Qt.callLater(() => root.select(root.currentKey))
            }
        }
    }

    // ---- Toolbar: search, view, filter, New Custom Bin, panel menu ----
    Item {
        id: toolbar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        // A narrow panel (the search would get less than 192 px) puts the tools on a second row: the view switch at
        // its left, the buttons at its right.
        readonly property bool narrow: width < viewSwitch.width + tools.width + Theme.space(3) + Theme.space(48)
        readonly property real toolsHeight: Math.max(viewSwitch.height, tools.height)
        height: narrow ? search.height + Theme.space(2) + toolsHeight : Math.max(search.height, toolsHeight)

        FieldInput {
            id: search
            objectName: "effectsSearch"
            anchors.left: parent.left
            anchors.right: toolbar.narrow ? parent.right : viewSwitch.left
            anchors.rightMargin: toolbar.narrow ? 0 : Theme.space(2)
            anchors.top: parent.top
            iconName: "search"
            clearOnEscape: true
            placeholderText: qsTr("Search effects")
            rightPadding: matches.visible ? matches.implicitWidth + Theme.space(5) : Theme.space(3.5)
            Accessible.name: qsTr("Search effects and transitions")
            Accessible.description: matches.visible ? matches.text : ""
            // Down (or Enter) goes to the list, on the first match while searching.
            Keys.onDownPressed: root.enterList()
            Keys.onReturnPressed: root.enterList()
            Keys.onEnterPressed: root.enterList()

            Text {
                id: matches
                objectName: "effectsMatchCount"
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(3.5)
                anchors.verticalCenter: parent.verticalCenter
                visible: tree.searching
                text: tree.matchCount === 1 ? qsTr("1 match") : qsTr("%1 matches").arg(tree.matchCount)
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }

        // The view switch: a segmented pill (the chosen view on the accent).
        Rectangle {
            id: viewSwitch
            objectName: "effectsViewSwitch"
            x: toolbar.narrow ? 0 : tools.x - width - Theme.space(1)
            y: toolbar.narrow ? search.height + Theme.space(2) + (toolbar.toolsHeight - height) / 2 : (search.height - height) / 2
            width: viewButtons.width + Theme.space(1)
            height: viewButtons.height + Theme.space(1)
            radius: height / 2
            color: Theme.sunken
            Accessible.role: Accessible.Grouping
            Accessible.name: qsTr("View")

            Row {
                id: viewButtons
                anchors.centerIn: parent
                IconButton {
                    id: listViewButton
                    objectName: "effectsView_list"
                    compact: true
                    checkable: true
                    checked: root.view === "list"
                    iconName: "list"
                    tooltip: qsTr("List view")
                    onClicked: {
                        Preferences.effectsView = "list"
                        listViewButton.checked = Qt.binding(() => root.view === "list")
                    }
                }
                IconButton {
                    id: compactViewButton
                    objectName: "effectsView_compact"
                    compact: true
                    checkable: true
                    checked: root.view === "compact"
                    iconName: "align-left"
                    tooltip: qsTr("Compact view")
                    onClicked: {
                        Preferences.effectsView = "compact"
                        compactViewButton.checked = Qt.binding(() => root.view === "compact")
                    }
                }
                IconButton {
                    id: iconsViewButton
                    objectName: "effectsView_icons"
                    compact: true
                    checkable: true
                    checked: root.view === "icons"
                    iconName: "layout-grid"
                    tooltip: qsTr("Icon view")
                    onClicked: {
                        Preferences.effectsView = "icons"
                        iconsViewButton.checked = Qt.binding(() => root.view === "icons")
                    }
                }
            }
        }

        Row {
            id: tools
            anchors.right: parent.right
            y: toolbar.narrow ? search.height + Theme.space(2) + (toolbar.toolsHeight - height) / 2 : (search.height - height) / 2
            spacing: Theme.space(1)

            IconButton {
                id: filterButton
                objectName: "effectsFilterButton"
                compact: true
                checkable: true
                checked: root.filterKey !== "all"
                iconName: "funnel"
                tooltip: root.filterKey === "all" ? qsTr("Filter") : qsTr("Filter: %1").arg(root.filterLabel(root.filterKey))
                onClicked: {
                    filterButton.checked = Qt.binding(() => root.filterKey !== "all")
                    panelMenu.popupFilter(filterButton, 0, filterButton.height)
                }
            }
            IconButton {
                objectName: "effectsNewBinButton"
                compact: true
                iconName: "folder-plus"
                tooltip: qsTr("New custom bin")
                command: "effects.newCustomBin"
                onClicked: root.newCustomBin("")
            }
            IconButton {
                id: panelMenuButton
                objectName: "effectsPanelMenuButton"
                compact: true
                iconName: "ellipsis"
                tooltip: qsTr("Effects panel menu")
                onClicked: panelMenu.popupAt(panelMenuButton, 0, panelMenuButton.height)
            }
        }
    }

    function filterLabel(key: string): string {
        switch (key) {
        case "video": return qsTr("Video")
        case "audio": return qsTr("Audio")
        case "transitions": return qsTr("Transitions")
        case "presets": return qsTr("Presets")
        case "favorites": return qsTr("Favorites")
        case "plugins": return qsTr("Audio Plug-ins")
        default: return qsTr("All")
        }
    }
    /// From the search to the list: the selected row if it shows, else the first item (the first match).
    function enterList() {
        if (tree.count === 0)
            return
        if (tree.indexOf(root.currentKey) < 0 || tree.searching) {
            let first = 0
            for (let i = 0; i < tree.count; ++i) {
                if (tree.get(i).rowType !== "folder") {
                    first = i
                    break
                }
            }
            root.select(root.keyAt(first, 0))
        }
        root.keyboardFocus = true
        list.forceActiveFocus(Qt.TabFocusReason)
    }

    // ---- The library could not be read ----
    Rectangle {
        id: banner
        objectName: "effectsLibraryError"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: toolbar.bottom
        anchors.topMargin: visible ? Theme.space(2) : 0
        visible: EffectPresets.libraryError !== ""
        height: visible ? bannerText.implicitHeight + Theme.space(4) : 0
        radius: Theme.radiusField
        color: Qt.alpha(Theme.warning, 0.10)
        border.width: Theme.hairline
        border.color: Qt.alpha(Theme.warning, 0.55)

        Icon {
            id: bannerIcon
            x: Theme.space(3)
            y: Theme.space(2) + (bannerText.fontInfo.pixelSize * 1.35 - height) / 2
            name: "triangle-alert"
            size: Theme.iconSm
            color: Theme.warning
        }
        Text {
            id: bannerText
            anchors.left: bannerIcon.right
            anchors.leftMargin: Theme.space(2)
            anchors.right: parent.right
            anchors.rightMargin: Theme.space(3)
            y: Theme.space(2)
            wrapMode: Text.Wrap
            text: qsTr("Your presets, custom bins and favourites could not be read: %1").arg(EffectPresets.libraryError)
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    // ---- The tree ----
    // Presets files from Explorer dropped between the folders (or on a folder that holds no presets): imported, each
    // preset into the bin it was exported from.
    DropArea {
        id: fileDrop
        anchors.fill: list
        onEntered: drag => {
            drag.accepted = drag.hasUrls && root.presetsFilesIn(drag.urls).length > 0
        }
        onDropped: drop => {
            const files = root.presetsFilesIn(drop.urls)
            drop.accept(Qt.CopyAction)
            Qt.callLater(() => root.importFiles(files, undefined))
        }
    }

    ListView {
        id: list
        objectName: "effectsList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: banner.bottom
        anchors.bottom: statusText.visible ? statusText.top : parent.bottom
        anchors.topMargin: Theme.space(2)
        anchors.bottomMargin: statusText.visible ? Theme.space(1.5) : 0
        clip: true
        model: tree
        currentIndex: -1
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: false
        highlightFollowsCurrentItem: false
        activeFocusOnTab: true
        ScrollBar.vertical: ThinScrollBar {}

        Accessible.role: Accessible.Tree
        Accessible.name: qsTr("Effects")

        delegate: DelegateChooser {
            role: "rowType"
            DelegateChoice {
                roleValue: "folder"
                EffectsTreeFolderRow {
                    panel: root
                }
            }
            DelegateChoice {
                roleValue: "item"
                EffectsTreeItemRow {
                    panel: root
                }
            }
            DelegateChoice {
                roleValue: "tiles"
                EffectsTreeTilesRow {
                    panel: root
                }
            }
        }

        Keys.onShortcutOverride: event => event.accepted = root.claimsKey(event)
        Keys.onPressed: event => event.accepted = root.handleKey(event)
        onActiveFocusChanged: {
            if (!activeFocus)
                return
            if (!root.pointerFocus)
                root.keyboardFocus = true
            // Back from a dialog that deleted the selected row: its neighbour or folder first, else the first row (a
            // click selects its own row).
            if (root.pointerFocus)
                return
            if (tree.indexOf(root.currentKey) < 0)
                root.keepCurrent()
            if (tree.indexOf(root.currentKey) < 0 && tree.count > 0)
                root.select(root.keyAt(0, 0))
        }
    }

    // A drop target outline while presets files from Explorer are over the list.
    Rectangle {
        anchors.fill: list
        visible: fileDrop.containsDrag
        radius: Theme.radiusField
        color: Qt.alpha(Theme.accentFill, 0.08)
        border.width: Theme.focusRingWidth
        border.color: Theme.accentStroke
    }

    EmptyState {
        objectName: "effectsNoMatch"
        anchors.left: list.left
        anchors.right: list.right
        anchors.top: list.top
        anchors.topMargin: Theme.space(6)
        visible: tree.count === 0
        illustration: tree.searching ? "search" : "effects"
        text: tree.searching ? qsTr("No effect matches “%1”").arg(search.text.trim())
            : root.filterKey === "favorites" ? qsTr("No favourites yet: right-click an effect and choose Add to Favorites.")
            : root.filterKey !== "all" ? qsTr("Nothing here with this filter.")
            : qsTr("No effects")
        actionText: tree.searching || root.filterKey !== "all" ? qsTr("Show all") : ""
        onActionTriggered: {
            search.text = ""
            root.filterKey = "all"
        }
    }

    // ---- Status line (clears after a few seconds) ----
    Text {
        id: statusText
        objectName: "effectsStatus"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.status !== ""
        text: root.status
        wrapMode: Text.Wrap
        maximumLineCount: 3
        elide: Text.ElideRight
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
    Timer {
        id: statusTimer
        interval: 6000
        onTriggered: root.status = ""
    }

    // ---- Menus and dialogs ----
    EffectsPanelMenu {
        id: panelMenu
        filter: root.filterKey
        onFilterChosen: key => root.filterKey = key
        onNewCustomBinRequested: root.newCustomBin("")
        onNewPresetsBinRequested: root.newPresetsBin("")
        onImportRequested: presetFiles.importInto(undefined)
        onExportRequested: presetFiles.exportBin("", "")
        onExpandAllRequested: tree.expandAll("")
        onCollapseAllRequested: tree.collapseAll("")
    }
    EffectItemMenu {
        id: itemMenu
        tree: tree
        onStatus: message => root.showStatus(message)
        onRenameRequested: key => root.startRename(key)
        onDeleteRequested: key => root.deleteRow(key)
        onNewCustomBinRequested: item => root.newCustomBin(item)
        onNewPresetsBinRequested: parentPath => root.newPresetsBin(parentPath)
        onImportRequested: bin => presetFiles.importInto(bin)
        onExportBinRequested: (path, title) => presetFiles.exportBin(path, title)
        onExportPresetRequested: name => presetFiles.exportOne(name)
    }
    EffectsPresetFiles {
        id: presetFiles
        objectName: "effectsPresetFiles"
        onStatus: message => root.showStatus(message)
        onImported: names => root.revealItemId("preset:" + names[0])
    }
    EffectsConfirmDialog {
        id: confirmDialog
        onClosed: {
            if (root.visible)
                list.forceActiveFocus(Qt.OtherFocusReason)
        }
    }
}
