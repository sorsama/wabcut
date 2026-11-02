pragma ComponentBehavior: Bound
import QtQuick

// 0.6 (P1 round, #36): a menu of the project's speakers (Captions.speakers: [{ key, name, color }]), each with its colour,
// the `current` one checked and the `excluded` one left out, then "New speaker…" (when `offerNew`). Used on its own (the
// Transcript tab's Speaker button for selected words) and as a submenu (a paragraph's This paragraph is / Merge into).
// `chosen(key)` when a speaker is picked; `newRequested()` for New speaker… (the host opens its name field once the menu
// has closed). Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu

    property var speakers: Captions.speakers
    property string current: ""
    property string excluded: ""
    property bool offerNew: true
    /// The rows offered (the speakers but `excluded`).
    readonly property var offered: (menu.speakers ?? []).filter(s => s.key !== menu.excluded)

    signal chosen(string key)
    signal newRequested()

    Instantiator {
        model: menu.offered
        delegate: ContextMenuItem {
            required property var modelData
            objectName: "speakerChoice_" + modelData.key
            text: modelData.name !== "" ? modelData.name : qsTr("Unnamed speaker")
            iconName: "circle"
            iconColor: modelData.color
            checkable: true
            checked: modelData.key === menu.current
            onTriggered: menu.chosen(modelData.key)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object as ContextMenuItem)
        onObjectRemoved: (index, object) => menu.removeItem(object as ContextMenuItem)
    }
    ContextMenuSeparator {
        visible: menu.offerNew && menu.offered.length > 0
        height: visible ? implicitHeight : 0
    }
    ContextMenuItem {
        objectName: "speakerChoiceNew"
        visible: menu.offerNew
        height: visible ? implicitHeight : 0
        text: qsTr("New speaker…")
        iconName: "plus"
        onTriggered: menu.newRequested()
    }
}
