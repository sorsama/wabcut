import QtQuick

// The Markers panel's export menu (M5i): CSV with Premiere's columns, or YouTube chapters for a video description.
// (Its own file: QtQuick.Controls, which the panel imports, has a ContextMenu type of its own.)
ContextMenu {
    id: root

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
}
