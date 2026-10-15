import QtQuick

// The Source Monitor's recent-sources menu (M5h): the sources loaded lately, newest first, and Close.
// In its own file so QtQuick.Controls does not shadow the Wabcut ContextMenu.
OptionsMenu {
    id: menu
    objectName: "recentSourcesMenu"

    options: SourceMonitor.recent.map(r => ({ id: r.media, name: r.name }))
    current: SourceMonitor.media
    onChosen: id => SourceMonitor.load(id)

    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "closeSourceItem"
        text: qsTr("Close source")
        iconName: "x"
        enabled: SourceMonitor.loaded
        onTriggered: SourceMonitor.unload()
    }
}
