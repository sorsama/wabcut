pragma ComponentBehavior: Bound
import QtQuick

// Markers (the menu bar, Premiere's order): In and Out, then the markers. Every row is a keymap command (its shortcut
// shows; greyed out while it cannot run). They act on the panel that had the focus before the menu opened (the bar puts
// it back first): Mark In in the Source Monitor marks the source, Add Marker in the Timeline marks the clip under the
// playhead. Their state follows that panel as well (AppMenuBar.commandState): with the Source Monitor focused, Go to /
// Clear In and Out go by the source's marks, not the sequence's.
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
}
