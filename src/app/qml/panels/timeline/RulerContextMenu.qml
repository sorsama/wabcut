pragma ComponentBehavior: Bound
import QtQuick

// Right-click menu of the timeline ruler (0.4, stream A; Premiere's ruler menu). TimelineRuler opens it at the pointer;
// the playhead does not move. Marks and markers go at the playhead and act on the sequence, whichever panel had the
// focus: Mark In / Out / Clip, Go to In / Out, Clear In / Out / In and Out, Add Marker, Go to Next / Previous Marker,
// Clear All Markers, Ripple Sequence Markers (ProjectController.rippleSequenceMarkers, marker.toggleRipple) and Show
// Audio Time Units (Monitors.audioTimeUnits). Opened on a marker's flag, it starts with Edit Marker… (the marker editor
// opens once the menu has closed: editMarkerRequested) and Clear Marker; 0.6 adds its Marker Colour and Marker Type
// submenus (inserted only for a flag) and Go to Marker, and a flag's menu shows only those marker rows. Rows that cannot run are greyed out (no In, no Out, no
// markers); each row is the existing undo step and shows its keymap shortcut.
// Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "rulerContextMenu"

    required property TimelineView view
    /// The marker whose flag was right-clicked (TimelineView.markerItems' `index`), or -1 (the ruler).
    property int markerIndex: -1
    /// Edit Marker… was chosen: the marker editor opens after the menu has closed (it keeps the focus).
    property int pendingEdit: -1

    readonly property bool hasIn: !isNaN(menu.view.markInX)
    readonly property bool hasOut: !isNaN(menu.view.markOutX)
    readonly property bool hasMarkers: menu.view.markerItems.length > 0
    /// 0.6: opened on a marker's flag: the marker's own rows only (Edit, Colour, Type, Go to, Clear), like the marker
    /// menu of the Program time bar and the Markers panel; the ruler's rows (marks, markers, units) show elsewhere.
    readonly property bool onFlag: menu.markerIndex >= 0
    /// 0.7 (card 119): how many markers the Copy / Cut / Clear rows act on (the selection when the flag is part of a larger
    /// one, else the flag alone).
    readonly property int groupCount: Markers.selectionCount > 1 && Markers.selection.indexOf(menu.markerIndex) >= 0
                                      ? Markers.selectionCount : 1

    /// Edit Marker… on the marker `index` (TimelineRuler opens the marker editor there).
    signal editMarkerRequested(int index)

    function openAt(target: Item, point: point, marker: int) {
        menu.markerIndex = marker
        menu.pendingEdit = -1
        menu.showMarkerSubmenus(marker)
        // Anchored at the click (Premiere and native menus): it opens down from the click, or, where the window has no
        // room below for the whole menu, up from it (its bottom edge at the click). Qt's own fit would push it up over
        // the panel header (the sequence tabs) with the click somewhere in its middle. The flag rows just came or went:
        // lay the rows out before measuring.
        const list = menu.contentItem as ListView
        if (list)
            list.forceLayout()
        const menuHeight = menu.implicitHeight
        const sceneY = target.mapToItem(null, point.x, point.y).y
        const below = target.Window.height - Math.max(0, menu.bottomMargin) - sceneY
        const above = sceneY - Math.max(0, menu.topMargin)
        const up = menuHeight > below && above > below
        menu.transformOrigin = up ? Item.Bottom : Item.Top
        menu.popup(target, point.x, up ? point.y - menuHeight : point.y)
    }
    // 0.6: the flag's Colour and Type submenus, after Edit Marker… (takeMenu keeps them while the ruler's own menu shows).
    readonly property MarkerColorSubmenu colorMenu: MarkerColorSubmenu {}
    readonly property MarkerTypeSubmenu typeMenu: MarkerTypeSubmenu {}
    function submenuIndex(submenu: var): int {
        for (let i = 0; i < menu.count; ++i) {
            const item = menu.itemAt(i) as ContextMenuItem
            if (item && item.subMenu === submenu)
                return i
        }
        return -1
    }
    function showMarkerSubmenus(marker: int) {
        for (const sub of [menu.typeMenu, menu.colorMenu]) {
            const at = menu.submenuIndex(sub)
            if (at >= 0)
                menu.takeMenu(at)
        }
        if (marker < 0)
            return
        Markers.scope = "sequence"
        if (Markers.selection.indexOf(marker) < 0)
            Markers.select(marker) // 0.7: a flag outside the selection becomes the selection
        const m = Markers.marker(marker)
        menu.colorMenu.markerIndex = marker
        menu.colorMenu.current = m.colorIndex ?? -1
        menu.typeMenu.markerIndex = marker
        menu.typeMenu.current = m.type ?? ""
        menu.insertMenu(1, menu.colorMenu)
        menu.insertMenu(2, menu.typeMenu)
    }
    /// A sequence command through ProjectController (the sequence's marks, never the Source Monitor's).
    function run(command: string, reveal: bool) {
        ProjectController.runCommand(command)
        if (reveal)
            menu.view.revealPlayhead()
    }

    onClosed: {
        const index = menu.pendingEdit
        menu.pendingEdit = -1
        if (index >= 0)
            menu.editMarkerRequested(index)
    }

    // ---- The marker under the pointer (a flag's menu only) ----
    // Their heights follow markerIndex, not `visible`: a closed menu's rows all read invisible, and openAt measures the
    // menu before it opens.
    ContextMenuItem {
        objectName: "rulerMenuEditMarker"
        visible: menu.markerIndex >= 0
        height: menu.markerIndex >= 0 ? implicitHeight : 0
        text: qsTr("Edit Marker…")
        iconName: "pencil"
        onTriggered: menu.pendingEdit = menu.markerIndex
    }
    ContextMenuItem {
        objectName: "rulerMenuGoToMarker"
        visible: menu.markerIndex >= 0
        height: menu.markerIndex >= 0 ? implicitHeight : 0
        text: qsTr("Go to Marker")
        iconName: "crosshair"
        onTriggered: {
            Markers.scope = "sequence"
            if (Markers.seek(menu.markerIndex))
                menu.view.revealPlayhead()
        }
    }
    // 0.7 (card 119): the flag joins the selection when it is not in it; Copy / Cut / Clear act on the whole selection when
    // the flag is part of a larger one, and Paste puts the copied markers at the playhead.
    ContextMenuItem {
        objectName: "rulerMenuCopyMarker"
        visible: menu.markerIndex >= 0
        height: menu.markerIndex >= 0 ? implicitHeight : 0
        text: menu.groupCount > 1 ? qsTr("Copy %n marker(s)", "", menu.groupCount) : qsTr("Copy Marker")
        iconName: "copy"
        onTriggered: Markers.copySelected()
    }
    ContextMenuItem {
        objectName: "rulerMenuCutMarker"
        visible: menu.markerIndex >= 0
        height: menu.markerIndex >= 0 ? implicitHeight : 0
        text: menu.groupCount > 1 ? qsTr("Cut %n marker(s)", "", menu.groupCount) : qsTr("Cut Marker")
        iconName: "scissors"
        onTriggered: Markers.cutSelected()
    }
    ContextMenuItem {
        objectName: "rulerMenuPasteMarkers"
        visible: menu.markerIndex >= 0 || Markers.clipboardCount > 0
        height: menu.markerIndex >= 0 || Markers.clipboardCount > 0 ? implicitHeight : 0
        text: qsTr("Paste Markers at Playhead")
        iconName: "clipboard-paste"
        enabled: Markers.clipboardCount > 0
        onTriggered: {
            Markers.scope = "sequence"
            Markers.paste()
        }
    }
    ContextMenuItem {
        objectName: "rulerMenuClearMarker"
        visible: menu.markerIndex >= 0
        height: menu.markerIndex >= 0 ? implicitHeight : 0
        text: menu.groupCount > 1 ? qsTr("Clear %n marker(s)", "", menu.groupCount) : qsTr("Clear Marker")
        iconName: "x"
        onTriggered: {
            Markers.scope = "sequence"
            if (menu.groupCount > 1)
                Markers.removeSelected()
            else
                Markers.remove(menu.markerIndex)
        }
    }
    ContextMenuSeparator {
        visible: false // 0.6: a flag's menu has the marker rows only
        height: 0
    }

    // ---- In and Out ----
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuMarkIn"
        text: qsTr("Mark In")
        iconName: "mark-in"
        command: "marker.markIn"
        onTriggered: menu.run("marker.markIn", false)
    }
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuMarkOut"
        text: qsTr("Mark Out")
        iconName: "mark-out"
        command: "marker.markOut"
        onTriggered: menu.run("marker.markOut", false)
    }
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuMarkClip"
        text: qsTr("Mark Clip")
        iconName: "film"
        command: "marker.markClip"
        onTriggered: menu.run("marker.markClip", false)
    }
    ContextMenuSeparator {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
    }
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuGoToIn"
        text: qsTr("Go to In")
        iconName: "go-to-in"
        command: "marker.goToIn"
        enabled: menu.hasIn
        onTriggered: menu.run("marker.goToIn", true)
    }
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuGoToOut"
        text: qsTr("Go to Out")
        iconName: "go-to-out"
        command: "marker.goToOut"
        enabled: menu.hasOut
        onTriggered: menu.run("marker.goToOut", true)
    }
    ContextMenuSeparator {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
    }
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuClearIn"
        text: qsTr("Clear In")
        iconName: "x"
        command: "marker.clearIn"
        enabled: menu.hasIn
        onTriggered: menu.run("marker.clearIn", false)
    }
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuClearOut"
        text: qsTr("Clear Out")
        iconName: "x"
        command: "marker.clearOut"
        enabled: menu.hasOut
        onTriggered: menu.run("marker.clearOut", false)
    }
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuClearInOut"
        text: qsTr("Clear In and Out")
        iconName: "x"
        command: "marker.clearInAndOut"
        enabled: menu.hasIn || menu.hasOut
        onTriggered: menu.run("marker.clearInAndOut", false)
    }
    ContextMenuSeparator {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
    }

    // ---- Sequence markers ----
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuAddMarker"
        text: qsTr("Add Marker")
        iconName: "bookmark"
        command: "marker.add"
        onTriggered: {
            Markers.scope = "sequence"
            Markers.add(false)
        }
    }
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuNextMarker"
        text: qsTr("Go to Next Marker")
        iconName: "skip-forward"
        command: "marker.goToNext"
        enabled: menu.hasMarkers
        onTriggered: {
            Markers.scope = "sequence"
            if (Markers.goToNext())
                menu.view.revealPlayhead()
        }
    }
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuPreviousMarker"
        text: qsTr("Go to Previous Marker")
        iconName: "skip-back"
        command: "marker.goToPrevious"
        enabled: menu.hasMarkers
        onTriggered: {
            Markers.scope = "sequence"
            if (Markers.goToPrevious())
                menu.view.revealPlayhead()
        }
    }
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        objectName: "rulerMenuClearAllMarkers"
        text: qsTr("Clear All Markers")
        iconName: "trash-2"
        command: "marker.clearAll"
        enabled: menu.hasMarkers
        onTriggered: {
            Markers.scope = "sequence"
            Markers.clearAll()
        }
    }
    // Stream B's per-sequence setting: markers follow insert, extract, ripple delete and ripple trims.
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        id: rippleItem
        objectName: "rulerMenuRippleMarkers"
        text: qsTr("Ripple Sequence Markers")
        iconName: "arrow-left-right"
        command: "marker.toggleRipple"
        checkable: true
        checked: ProjectController.rippleSequenceMarkers
        onTriggered: {
            KeymapDispatcher.trigger("marker.toggleRipple")
            rippleItem.checked = Qt.binding(() => ProjectController.rippleSequenceMarkers)
        }
    }
    ContextMenuSeparator {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
    }
    // Premiere's Show Audio Time Units (one setting with the Program Monitor's): the ruler counts audio samples.
    ContextMenuItem {
        visible: !menu.onFlag
        height: menu.onFlag ? 0 : implicitHeight
        id: audioUnitsItem
        objectName: "rulerMenuAudioTimeUnits"
        text: qsTr("Show Audio Time Units")
        iconName: "audio-lines"
        command: "programMonitor.audioTimeUnits"
        checkable: true
        checked: Monitors.audioTimeUnits
        onTriggered: {
            Monitors.audioTimeUnits = !Monitors.audioTimeUnits
            audioUnitsItem.checked = Qt.binding(() => Monitors.audioTimeUnits)
        }
    }
}
