pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The Export panel's gaming presets (M5o) as one grid of tiles in `columns`, in the order of their groups - YouTube,
// Shorts / TikTok / Reels, Twitch, Discord size targets, GIF and WebM - each tile in its destination's hue (the panel's
// filter chips carry the same hues). `filter` shows one group ("all" = every one). Each tile carries its size and
// export-time estimate for the range the panel exports (GamingExport.estimateFor; re-evaluated on edits, range changes
// and encoder probes). `chosen(id)` asks the panel to use one. Keyboard: the grid is one Tab stop (the selected preset,
// else the first one shown); the arrow keys, Home and End move between the presets shown, Space/Enter choose one.
GridLayout {
    id: root
    objectName: "gamingPresets"

    /// The selected gaming preset id ("" = none).
    property string selected: ""
    property string sequenceId: ""
    /// "whole" | "inOut" | "workArea".
    property string range: "whole"
    /// The group shown ("all" = every group).
    property string filter: "all"

    signal chosen(string id)

    /// A destination's colour: the groups' hues; the built-in presets' for any other id.
    function hueOf(groupId) {
        switch (groupId) {
        case "youtube": return Theme.huePink
        case "vertical": return Theme.hueMint
        case "twitch": return Theme.hueViolet
        case "discord": return Theme.hueBlue
        case "memes": return Theme.hueLime
        }
        return Theme.hueOrange
    }
    function shows(groupId) {
        return root.filter === "all" || root.filter === groupId
    }

    /// Every gaming preset in the order of its group.
    readonly property var presets: GamingExport.groups.reduce(
        (list, g) => list.concat(GamingExport.presets.filter(p => p.group === g.id)), [])
    /// The available presets shown, in order.
    readonly property var order: presets.filter(p => p.available && root.shows(p.group)).map(p => p.id)
    /// How many presets the filter shows.
    readonly property int shownCount: presets.filter(p => root.shows(p.group)).length
    /// The preset last focused (the grid's Tab stop follows it, so the focused card never leaves the Tab chain).
    property string focusedId: ""
    /// The preset in the Tab chain.
    readonly property string tabStop: order.indexOf(focusedId) >= 0 ? focusedId
                                      : order.indexOf(selected) >= 0 ? selected : (order[0] ?? "")
    property var cards: ({})

    function moveFocus(fromId, key) {
        const i = order.indexOf(fromId)
        let next = -1
        if (key === Qt.Key_Right || key === Qt.Key_Down)
            next = Math.min(i + 1, order.length - 1)
        else if (key === Qt.Key_Left || key === Qt.Key_Up)
            next = Math.max(i - 1, 0)
        else if (key === Qt.Key_Home)
            next = 0
        else if (key === Qt.Key_End)
            next = order.length - 1
        if (next < 0)
            return false
        root.cards[order[next]]?.forceActiveFocus(Qt.TabFocusReason)
        return true
    }

    columns: 2
    columnSpacing: Theme.space(2)
    rowSpacing: Theme.space(2)

    Repeater {
        model: root.presets

        delegate: GamingPresetCard {
            id: card
            required property var modelData
            // Re-read on edits (length), encoder probes (time) and the range.
            readonly property var estimateData: ProjectController.revision >= 0 && ExportController.autoEncoders
                                                && modelData.available
                                                ? GamingExport.estimateFor(modelData.id, root.sequenceId, root.range)
                                                : ({})
            objectName: "gamingPreset_" + modelData.id
            visible: root.shows(modelData.group)
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1 // equal columns
            text: modelData.name
            description: modelData.description
            iconName: modelData.icon
            hue: root.hueOf(modelData.group)
            available: modelData.available
            reason: modelData.reason
            selected: root.selected === modelData.id
            focusPolicy: root.tabStop === modelData.id ? Qt.StrongFocus : Qt.ClickFocus
            // The arrows, Home and End move here rather than the playhead.
            Keys.onShortcutOverride: event => event.accepted = event.modifiers === Qt.NoModifier
                                                           && [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down,
                                                               Qt.Key_Home, Qt.Key_End].indexOf(event.key) >= 0
            Keys.onPressed: event => {
                if (event.modifiers === Qt.NoModifier && root.moveFocus(card.modelData.id, event.key))
                    event.accepted = true
            }
            onActiveFocusChanged: {
                if (activeFocus)
                    root.focusedId = modelData.id
            }
            Component.onCompleted: root.cards[modelData.id] = card
            Component.onDestruction: {
                if (root.cards[modelData.id] === card)
                    delete root.cards[modelData.id]
            }
            estimateIsError: card.estimateData.ok === false
            sizeText: card.estimateData.ok === true ? card.estimateData.sizeText : ""
            encodeSeconds: card.estimateData.ok === true ? card.estimateData.encodeSeconds : -1
            estimate: card.estimateData.ok === true
                      ? qsTr("≈ %1 · %2").arg(card.estimateData.sizeText).arg(card.estimateData.encodeText)
                      : (card.estimateData.error ?? "")
            onClicked: root.chosen(modelData.id)
        }
    }
}
