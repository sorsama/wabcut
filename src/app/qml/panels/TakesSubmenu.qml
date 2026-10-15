pragma ComponentBehavior: Bound
import QtQuick

// Takes (A.8, M5m): the clip menu's submenu for a voice-over clip. It lists the takes recorded into the clip's slot
// (VoiceOver.takesOf: take n, its length, when it was recorded and the microphone), newest first, with the one the clip
// plays checked; choosing one swaps it in (VoiceOver.useTake: record.useTake, one undo step). A clip that was not
// recorded has no takes: the submenu is disabled. The host calls refresh() when it opens (a new take may have been
// recorded since). Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "takesSubmenu"

    /// The clip's key ("c7").
    property string clipKey: ""
    /// VoiceOver.takesOf(clipKey): [{ take, media, name, startText, seconds, device, createdAt, active }].
    property var takes: []
    /// Why the last choice was refused ("" after a good one).
    property string error: ""

    function refresh() {
        menu.takes = menu.clipKey !== "" ? VoiceOver.takesOf(menu.clipKey) : []
    }
    function choose(take) {
        menu.error = VoiceOver.useTake(menu.clipKey, take)
        menu.refresh()
    }
    /// "Take 3 · 12.4 s · 12/10/2026 09:41 · USB microphone"
    function describe(t) {
        const parts = [qsTr("Take %1").arg(t.take), qsTr("%1 s").arg(Number(t.seconds ?? 0).toFixed(1))]
        if ((t.createdAt ?? "") !== "")
            parts.push(new Date(t.createdAt).toLocaleString(Qt.locale(), Locale.ShortFormat))
        if ((t.device ?? "") !== "")
            parts.push(t.device)
        return parts.join(" · ")
    }

    title: takes.length > 0 ? qsTr("Takes (%1)").arg(takes.length) : qsTr("Takes")
    enabled: takes.length > 0
    onClipKeyChanged: refresh()
    Component.onCompleted: refresh()

    Instantiator {
        model: menu.takes
        delegate: ContextMenuItem {
            required property var modelData
            objectName: "takeItem_" + modelData.take
            text: menu.describe(modelData)
            checkable: true
            checked: modelData.active === true
            onTriggered: menu.choose(modelData.take)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object as ContextMenuItem)
        onObjectRemoved: (index, object) => menu.removeItem(object as ContextMenuItem)
    }
}
