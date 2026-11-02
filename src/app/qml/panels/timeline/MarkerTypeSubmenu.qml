pragma ComponentBehavior: Bound
import QtQuick

// 0.6 (P1 round, stream 1A): a marker's Type submenu (the marker menus): Comment, Chapter, Segmentation and Web Link,
// the marker's own checked; choosing one is one undo step (Markers.update in Markers' current scope). A web link's URL is
// set in the marker editor. Its own file, importing only QtQuick (see MarkerColorSubmenu).
ContextMenu {
    id: menu
    objectName: "markerTypeSubmenu"

    property int markerIndex: -1
    /// Its type ("comment", "chapter", "segmentation", "webLink"), read by the host when it opens.
    property string current: ""

    readonly property var types: [{ value: "comment", label: qsTr("Comment") }, { value: "chapter", label: qsTr("Chapter") },
                                  { value: "segmentation", label: qsTr("Segmentation") },
                                  { value: "webLink", label: qsTr("Web Link") }]

    title: qsTr("Marker Type")

    Instantiator {
        model: menu.types
        delegate: ContextMenuItem {
            required property var modelData
            objectName: "markerType_" + modelData.value
            text: modelData.label
            checkable: true
            checked: menu.current === modelData.value
            onTriggered: {
                if (menu.markerIndex >= 0)
                    Markers.update(menu.markerIndex, { type: modelData.value })
            }
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
}
