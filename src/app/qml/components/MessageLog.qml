pragma Singleton
import QtQuick

// MessageLog (0.4): the recent messages - every toast and every message of Home's strip - newest first, for the title
// bar's Background tasks popover ("Recent messages"), so a message a later one covered is never lost. Premiere's Events
// panel, folded into that popover.
//   add(text, tone, actionText, action)   logs a message (tone "info" | "success" | "warning" | "danger") with its
//                                         action, if it had one; returns its id. A repeat of the newest message (same
//                                         text and tone) only refreshes its time.
//   addKeyed(text, tone, actionText, action, key)
//                                         the same for a running readout (the clip volume keys' "Volume +1 dB (now
//                                         3 dB)"): it replaces the earlier message with the same `key`, so holding a
//                                         key leaves one line, not thirty ("" key: add()).
//   missed(id)                            an error that left on its timer, unseen: `unseen` counts it (once until the
//                                         popover is opened)
//   markSeen()                            the popover opened: every error was seen
//   clear()                               forgets every message
//   toneOf(color)                         Toast.show's colour as a tone (Theme.danger -> "danger", ...)
// Entries: { id, time (Date), text, tone, actionText, action, missed, key }. At most `limit` are kept: past it the oldest
// note goes first (info or success), so a flood of notes never pushes an error or a warning out; only when every entry
// is a problem does the oldest problem go (an unseen error that goes stops counting, so the pill and the list agree).
QtObject {
    id: root
    objectName: "messageLog"

    readonly property int limit: 30
    /// Newest first. Always replaced by a new array (never changed in place), so bindings and views update.
    property var entries: []
    readonly property int count: root.entries.length
    /// Errors that left unseen (missed) since the popover was last opened (`unseenIds.length`).
    property int unseen: 0
    /// The ids `unseen` counts (copied on change, never changed in place).
    property var unseenIds: []
    property int lastId: 0

    function add(text: string, tone: string, actionText: string, action: var): int {
        return root.addKeyed(text, tone, actionText, action, "")
    }
    function addKeyed(text: string, tone: string, actionText: string, action: var, key: string): int {
        const kind = tone === "success" || tone === "warning" || tone === "danger" ? tone : "info"
        const now = new Date()
        const old = root.entries
        if (old.length > 0 && old[0].text === text && old[0].tone === kind) {
            const top = old[0]
            const list = [{ id: top.id, time: now, text: top.text, tone: top.tone,
                            actionText: actionText !== "" ? actionText : top.actionText,
                            action: actionText !== "" ? action : top.action, missed: top.missed, key: top.key }]
            for (let i = 1; i < old.length; ++i)
                list.push(old[i])
            root.entries = list
            return top.id
        }
        root.lastId = root.lastId + 1
        const list = [{ id: root.lastId, time: now, text: text, tone: kind, actionText: actionText,
                        action: typeof action === "function" ? action : null, missed: false, key: key }]
        const gone = []
        for (let i = 0; i < old.length; ++i) {
            // A readout replaces its own earlier line (an unseen one stops counting with it).
            if (key !== "" && old[i].key === key)
                gone.push(old[i].id)
            else
                list.push(old[i])
        }
        // Past the limit the oldest note goes first; every entry a problem: the oldest problem.
        while (list.length > root.limit) {
            let drop = list.length - 1
            for (let i = list.length - 1; i > 0; --i) {
                if (list[i].tone !== "danger" && list[i].tone !== "warning") {
                    drop = i
                    break
                }
            }
            gone.push(list[drop].id)
            list.splice(drop, 1)
        }
        root.entries = list
        root.forget(gone)
        return root.lastId
    }
    // Whether `list` (an array of ids, read back from a property: no Array methods) holds `id`.
    function holds(list: var, id: int): bool {
        for (let i = 0; i < list.length; ++i) {
            if (list[i] === id)
                return true
        }
        return false
    }
    // The entries `ids` left the log: `unseen` no longer counts them.
    function forget(ids: var) {
        const old = root.unseenIds
        if (ids.length === 0 || old.length === 0)
            return
        const kept = []
        for (let i = 0; i < old.length; ++i) {
            if (!root.holds(ids, old[i]))
                kept.push(old[i])
        }
        root.unseenIds = kept
        root.unseen = kept.length
    }
    function missed(id: int) {
        const old = root.entries
        const list = []
        let found = false
        for (let i = 0; i < old.length; ++i) {
            const e = old[i]
            if (e.id === id) {
                found = true
                list.push(e.missed ? e : { id: e.id, time: e.time, text: e.text, tone: e.tone, actionText: e.actionText,
                                           action: e.action, missed: true, key: e.key })
            } else {
                list.push(e)
            }
        }
        const counted = root.unseenIds
        if (!found || root.holds(counted, id))
            return
        root.entries = list
        const ids = []
        for (let i = 0; i < counted.length; ++i)
            ids.push(counted[i])
        ids.push(id)
        root.unseenIds = ids
        root.unseen = ids.length
    }
    function markSeen() {
        root.unseenIds = []
        root.unseen = 0
    }
    function clear() {
        root.entries = []
        root.unseenIds = []
        root.unseen = 0
    }
    function toneOf(color: var): string {
        if (color === undefined || color === null)
            return "info"
        if (Qt.colorEqual(color, Theme.danger))
            return "danger"
        if (Qt.colorEqual(color, Theme.warning))
            return "warning"
        if (Qt.colorEqual(color, Theme.success))
            return "success"
        return "info"
    }
}
