pragma ComponentBehavior: Bound
import QtQuick

// Audio Channels (A.9, M5n): the clip menu's submenu for clips with sound, on the selected clips (Premiere's clip menu >
// Audio Channels and Audio Options): Modify > Audio Channels… (the dialog), Fill Left with Right / Fill Right with
// Left (both sides play one channel: audio.fillChannel), Swap Channels, and Split to Mono (one linked mono clip per
// source channel on the free audio tracks below). Each is one undo step. Its own file, importing only QtQuick, so
// QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "audioChannelsSubmenu"

    /// The clips it acts on ("c12").
    property var clipKeys: []
    /// Why the last action was refused ("" after a good one).
    property string error: ""

    function act(failure) {
        menu.error = failure
    }

    title: qsTr("Audio Channels")

    ContextMenuItem {
        objectName: "audioChannelsOpenItem"
        text: qsTr("Audio Channels…")
        iconName: "sliders-horizontal"
        onTriggered: AudioChannels.openDialog(menu.clipKeys)
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "fillLeftItem"
        text: qsTr("Fill Left with Right")
        onTriggered: menu.act(AudioChannels.fill(menu.clipKeys, "right"))
    }
    ContextMenuItem {
        objectName: "fillRightItem"
        text: qsTr("Fill Right with Left")
        onTriggered: menu.act(AudioChannels.fill(menu.clipKeys, "left"))
    }
    ContextMenuItem {
        objectName: "swapChannelsItem"
        text: qsTr("Swap Channels")
        iconName: "arrow-left-right"
        onTriggered: menu.act(AudioChannels.swap(menu.clipKeys))
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "splitToMonoItem"
        text: qsTr("Split to Mono")
        iconName: "fold-vertical"
        enabled: menu.clipKeys.length === 1
        onTriggered: menu.act(AudioChannels.splitToMono(menu.clipKeys[0]))
    }
}
