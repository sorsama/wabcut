pragma ComponentBehavior: Bound
import QtQuick

// 0.6 (P1 round, stream 1A): the right-click menu of one marker - a marker on the Program Monitor's time bar or a row of
// the Markers panel (the timeline ruler's flags get the same rows in their ruler menu): Edit Marker… (the host opens
// the marker editor once the menu has closed: editRequested), Marker Colour and Marker Type submenus, Go to Marker and
// Delete Marker. It acts in Markers' current scope (the host sets it before openAt: "sequence" for the ruler and the time
// bar, the panel's own scope for its rows); every change is one undo step. Its own file, importing only QtQuick, so
// QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "markerContextMenu"

    /// The marker (an index in Markers' current scope).
    property int markerIndex: -1
    /// Edit Marker… was chosen: the editor opens after the menu has closed (it keeps the focus).
    property int pendingEdit: -1
    /// Markers.marker(markerIndex) as the menu opened.
    property var marker: ({})

    /// Edit Marker… on `index`.
    signal editRequested(int index)
    /// Go to Marker moved the playhead (the host reveals it).
    signal wentTo(int index)

    function openAt(target: Item, point: point, index: int) {
        menu.markerIndex = index
        menu.pendingEdit = -1
        menu.marker = Markers.marker(index)
        colorMenu.markerIndex = index
        colorMenu.current = menu.marker.colorIndex ?? -1
        typeMenu.markerIndex = index
        typeMenu.current = menu.marker.type ?? ""
        menu.popup(target, point.x, point.y)
    }

    onClosed: {
        const index = menu.pendingEdit
        menu.pendingEdit = -1
        if (index >= 0)
            menu.editRequested(index)
    }

    ContextMenuItem {
        objectName: "markerMenuEdit"
        text: qsTr("Edit Marker…")
        iconName: "pencil"
        command: "marker.edit"
        onTriggered: menu.pendingEdit = menu.markerIndex
    }
    MarkerColorSubmenu {
        id: colorMenu
    }
    MarkerTypeSubmenu {
        id: typeMenu
    }
    ContextMenuItem {
        objectName: "markerMenuGoTo"
        text: qsTr("Go to Marker")
        iconName: "crosshair"
        onTriggered: {
            if (Markers.seek(menu.markerIndex))
                menu.wentTo(menu.markerIndex)
        }
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "markerMenuDelete"
        text: qsTr("Delete Marker")
        iconName: "trash-2"
        onTriggered: Markers.remove(menu.markerIndex)
    }
}
