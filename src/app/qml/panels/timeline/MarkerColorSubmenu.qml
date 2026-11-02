pragma ComponentBehavior: Bound
import QtQuick

// 0.6 (P1 round, stream 1A): a marker's Colour submenu (the marker menus: MarkerContextMenu, the ruler's flag menu):
// Premiere's eight marker colours, the marker's own checked; choosing one is one undo step (Markers.update in Markers'
// current scope - the host sets the scope before it opens). Its own file, importing only QtQuick, so QtQuick.Controls'
// ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "markerColorSubmenu"

    /// The marker (an index in Markers' current scope), -1 for none.
    property int markerIndex: -1
    /// Its colour index (Markers.marker(index).colorIndex), read by the host when it opens.
    property int current: -1

    readonly property var names: [qsTr("Green"), qsTr("Red"), qsTr("Purple"), qsTr("Orange"), qsTr("Yellow"),
                                  qsTr("White"), qsTr("Blue"), qsTr("Cyan")]

    title: qsTr("Marker Colour")

    Instantiator {
        model: 8
        delegate: ContextMenuItem {
            required property int index
            objectName: "markerColor_" + index
            text: menu.names[index]
            iconName: "circle"
            iconColor: Theme.markerColor(index)
            checkable: true
            checked: menu.current === index
            onTriggered: {
                if (menu.markerIndex >= 0)
                    Markers.update(menu.markerIndex, { colorIndex: index })
            }
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
}
