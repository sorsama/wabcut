pragma ComponentBehavior: Bound
import QtQuick

// Markers (the menu bar, Premiere's order): In and Out, then the markers. Every row is a keymap command (its shortcut
// shows; greyed out while it cannot run). They act on the panel that had the focus before the menu opened (the bar puts
// it back first): Mark In in the Source Monitor marks the source, Add Marker in the Timeline marks the clip under the
// playhead. Their state follows that panel as well (AppMenuBar.commandState): with the Source Monitor focused, Go to /
// Clear In and Out go by the source's marks, not the sequence's.
// 0.6: Add Chapter Marker… and Edit Marker… (the marker at the playhead) open the marker editor.
// 0.4: Ripple Sequence Markers (checked while on, the default): the sequence's markers move with insert, extract, ripple
// delete and ripple trims, in the edit's undo step. Toggling it is one undo step; the choice is saved per sequence.
MenuBarMenu {
    id: root
    objectName: "markersMenu"
    title: qsTr("Markers")
    key: "markers"
    mnemonic: "M"

    BarMenuItem {
        text: qsTr("Mark In")
        command: "marker.markIn"
    }
    BarMenuItem {
        text: qsTr("Mark Out")
        command: "marker.markOut"
    }
    BarMenuItem {
        text: qsTr("Mark Clip")
        command: "marker.markClip"
    }
    BarMenuItem {
        text: qsTr("Mark Selection")
        command: "marker.markSelection"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Go to In")
        command: "marker.goToIn"
    }
    BarMenuItem {
        text: qsTr("Go to Out")
        command: "marker.goToOut"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Clear In")
        command: "marker.clearIn"
    }
    BarMenuItem {
        text: qsTr("Clear Out")
        command: "marker.clearOut"
    }
    BarMenuItem {
        text: qsTr("Clear In and Out")
        command: "marker.clearInAndOut"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Add Marker")
        command: "marker.add"
    }
    // 0.7 (card 105): a range marker as long as the In to Out range.
    BarMenuItem {
        objectName: "markersMenuAddFromInOut"
        text: qsTr("Add Marker from In/Out")
        command: "marker.addFromInOut"
    }
    // 0.6 (stream 1A): a chapter marker at the playhead (its editor opens), and the marker at the playhead's editor.
    BarMenuItem {
        objectName: "markersMenuAddChapter"
        text: qsTr("Add Chapter Marker…")
        command: "marker.addChapter"
    }
    BarMenuItem {
        objectName: "markersMenuEdit"
        text: qsTr("Edit Marker…")
        command: "marker.edit"
    }
    BarMenuItem {
        text: qsTr("Go to Next Marker")
        command: "marker.goToNext"
    }
    BarMenuItem {
        text: qsTr("Go to Previous Marker")
        command: "marker.goToPrevious"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Clear Selected Marker")
        command: "marker.clearSelected"
    }
    BarMenuItem {
        text: qsTr("Clear All Markers")
        command: "marker.clearAll"
    }
    // P1 (#23, stream 1C): a still at each marker (thumbnail candidates; the Export Frame dialog in its markers mode).
    BarMenuItem {
        objectName: "markersMenuExportFrames"
        text: qsTr("Export Frames at Markers…")
        command: "marker.exportFrames"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        id: rippleMarkers
        text: qsTr("Ripple Sequence Markers")
        command: "marker.toggleRipple"
        checkable: true
        checked: ProjectController.rippleSequenceMarkers
        onTriggered: rippleMarkers.checked = Qt.binding(() => ProjectController.rippleSequenceMarkers)
    }
}
