import QtQuick

// The Markers panel's export menu (M5i): CSV with Premiere's columns, or YouTube chapters for a video description.
// (Its own file: QtQuick.Controls, which the panel imports, has a ContextMenu type of its own.)
// P1 (#24): Copy YouTube chapters puts them on the clipboard at once (Markers.copyToClipboard); "From In to Out" keeps
// the markers between the sequence's In and Out marks and times them from the In mark (an export with pre-roll); the
// YouTube rows use chapter markers only, so to-dos and comments never become chapters. The choice is `fromInToOut`, which
// the panel passes to Markers.exportToFile with the YouTube file.
ContextMenu {
    id: root

    /// "From In to Out" is checked (and the sequence has both marks).
    property bool fromInToOut: false
    readonly property bool hasInOut: ProjectController.markInText !== "" && ProjectController.markOutText !== ""
    /// The range the rows export: "inOut" or "" (the whole sequence).
    readonly property string range: fromInToOut && hasInOut ? "inOut" : ""

    signal exportRequested(string format)

    ContextMenuItem {
        objectName: "exportMarkersCsv"
        text: qsTr("Export as CSV…")
        onTriggered: root.exportRequested("csv")
    }
    ContextMenuItem {
        objectName: "exportMarkersYoutube"
        text: qsTr("Export as YouTube chapters…")
        onTriggered: root.exportRequested("youtube")
    }
    ContextMenuItem {
        objectName: "exportMarkersCopyYoutube"
        text: qsTr("Copy YouTube chapters")
        iconName: "copy"
        onTriggered: Markers.copyToClipboard("youtube", root.range, true)
    }
    // P1 (#26, 2C): each marker with a duration as its own file (the Export Ranges dialog, once the menu has closed).
    ContextMenuItem {
        objectName: "exportMarkersEachRange"
        text: qsTr("Export each marker range…")
        iconName: "scissors"
        enabled: ProjectController.markers.length >= 0 && ExportController.markerRanges("", -1).length > 0
        disabledReason: qsTr("No marker has a duration: drag a marker's end to give it one")
        onTriggered: root.rangesPending = true
    }
    property bool rangesPending: false
    onClosed: {
        if (root.rangesPending) {
            root.rangesPending = false
            ExportController.rangesExportRequested("markers")
        }
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        id: inOutItem
        objectName: "exportMarkersInOut"
        text: qsTr("From In to Out")
        checkable: true
        checked: root.fromInToOut && root.hasInOut
        enabled: root.hasInOut
        disabledReason: qsTr("Set In and Out marks on the sequence first")
        onTriggered: {
            root.fromInToOut = !root.fromInToOut
            inOutItem.checked = Qt.binding(() => root.fromInToOut && root.hasInOut) // the toggle broke it
        }
    }
}
