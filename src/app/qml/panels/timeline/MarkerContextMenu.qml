pragma ComponentBehavior: Bound
import QtQuick

// 0.6 (P1 round, stream 1A): the right-click menu of one marker - a marker on the Program Monitor's time bar or a row of
// the Markers panel (the timeline ruler's flags get the same rows in their ruler menu): Edit Marker… (the host opens
// the marker editor once the menu has closed: editRequested), Marker Colour and Marker Type submenus, Go to Marker and
// Delete Marker. It acts in Markers' current scope (the host sets it before openAt: "sequence" for the ruler and the time
// bar, the panel's own scope for its rows); every change is one undo step. 0.7 (card 119): the marker joins the selection
// when it is not in it, and the Copy / Cut / Delete rows act on the whole selection when it holds several markers; Paste
// puts the copied markers at the playhead. Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not
// shadow ours.
ContextMenu {
    id: menu
    objectName: "markerContextMenu"

    /// The marker (an index in Markers' current scope).
    property int markerIndex: -1
    /// Edit Marker… was chosen: the editor opens after the menu has closed (it keeps the focus).
    property int pendingEdit: -1
    /// Markers.marker(markerIndex) as the menu opened.
    property var marker: ({})
    /// How many markers the group rows act on: the whole selection when this marker is part of a larger one, else 1.
    readonly property int groupCount: Markers.selectionCount > 1 && Markers.selection.indexOf(menu.markerIndex) >= 0
                                      ? Markers.selectionCount : 1

    /// Edit Marker… on `index`.
    signal editRequested(int index)
    /// Go to Marker moved the playhead (the host reveals it).
    signal wentTo(int index)

    function openAt(target: Item, point: point, index: int) {
        if (Markers.selection.indexOf(index) < 0)
            Markers.select(index) // a right click on a marker outside the selection selects just that one
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
        objectName: "markerMenuCopy"
        text: menu.groupCount > 1 ? qsTr("Copy %n marker(s)", "", menu.groupCount) : qsTr("Copy Marker")
        iconName: "copy"
        onTriggered: Markers.copySelected()
    }
    ContextMenuItem {
        objectName: "markerMenuCut"
        text: menu.groupCount > 1 ? qsTr("Cut %n marker(s)", "", menu.groupCount) : qsTr("Cut Marker")
        iconName: "scissors"
        onTriggered: Markers.cutSelected()
    }
    ContextMenuItem {
        objectName: "markerMenuPaste"
        text: qsTr("Paste Markers at Playhead")
        iconName: "clipboard-paste"
        enabled: Markers.clipboardCount > 0
        onTriggered: Markers.paste()
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "markerMenuDelete"
        text: menu.groupCount > 1 ? qsTr("Delete %n marker(s)", "", menu.groupCount) : qsTr("Delete Marker")
        iconName: "trash-2"
        onTriggered: {
            if (menu.groupCount > 1)
                Markers.removeSelected()
            else
                Markers.remove(menu.markerIndex)
        }
    }
}
