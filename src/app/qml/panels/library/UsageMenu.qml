pragma ComponentBehavior: Bound
import QtQuick

// P1 round (#40), Find in Timeline: where a Library item (media, a subclip, a sequence nested in others) is used, one
// row per placement - "Sequence 01 · 00:00:12:03 · V1, A1" (a linked video / audio pair is one row). A row opens that
// sequence with the playhead at the clip's start and the clip selected, scrolled into view (Library.findInTimeline, run
// once the menu has closed so the timeline keeps the focus). The card's usage badge opens it. In its own file, importing
// only QtQuick, so the Wabcut ContextMenu is not shadowed by QtQuick.Controls'.
ContextMenu {
    id: menu
    objectName: "libraryUsageMenu"

    /// The item the rows are for, and its uses (Library.usages, read when the menu opens).
    property string key: ""
    property var usages: []
    /// At most this many rows; "… n more" says how many are left out.
    readonly property int limit: 40
    property var pendingAction: null

    /// Shows the menu for `itemKey` at (`x`, `y`) in `target`.
    function openFor(itemKey: string, target: Item, x: real, y: real) {
        menu.key = itemKey
        menu.usages = Library.usages(itemKey)
        menu.pendingAction = null
        menu.popup(target, x, y)
    }

    onClosed: {
        const action = menu.pendingAction
        menu.pendingAction = null
        if (action !== null)
            action()
    }

    ContextMenuItem {
        objectName: "libraryUsageNone"
        visible: menu.usages.length === 0
        height: visible ? implicitHeight : 0
        enabled: false
        text: qsTr("Not used in any sequence")
        iconName: "layers"
    }
    Instantiator {
        model: menu.usages.slice(0, menu.limit)
        delegate: ContextMenuItem {
            id: useRow
            required property var modelData
            required property int index
            objectName: "libraryUsage_" + useRow.index
            //: A place a Library item is used: sequence, timecode, tracks, e.g. "Sequence 01 · 00:00:12:03 · V1, A1"
            text: qsTr("%1 · %2 · %3").arg(useRow.modelData.sequenceName).arg(useRow.modelData.timecode)
                                       .arg(useRow.modelData.tracks)
            iconName: "film"
            Accessible.description: useRow.modelData.clipName
            onTriggered: {
                const sequence = useRow.modelData.sequenceKey
                const clip = useRow.modelData.clipId
                menu.pendingAction = () => Library.findInTimeline(sequence, clip)
            }
        }
        onObjectAdded: (index, object) => menu.insertItem(index + 1, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
    ContextMenuItem {
        objectName: "libraryUsageMore"
        visible: menu.usages.length > menu.limit
        height: visible ? implicitHeight : 0
        enabled: false
        //: More places than the menu lists, e.g. "12 more"
        text: qsTr("%n more", "", Math.max(0, menu.usages.length - menu.limit))
    }
}
