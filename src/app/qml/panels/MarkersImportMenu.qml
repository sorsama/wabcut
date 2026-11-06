import QtQuick

// The Markers panel's import menu (0.7): read a marker list from a CSV (Premiere's export, a spreadsheet), a text file
// (YouTube chapters, Audacity labels) or a JSON file into the sequence's markers, or the selected clip's media in the
// Clip tab. The file dialog opens once the menu has closed (MarkersPanel). (Its own file: QtQuick.Controls, which the
// panel imports, has a ContextMenu type of its own.)
ContextMenu {
    id: root
    objectName: "markersImportMenu"

    /// The file's times count from the playhead / the markers there are replaced by the file's.
    signal importRequested(bool atPlayhead, bool replace)

    property bool atPlayhead: false
    property bool replace: false
    property bool pending: false

    ContextMenuItem {
        objectName: "importMarkersFile"
        text: qsTr("Import markers…")
        iconName: "import"
        onTriggered: {
            root.atPlayhead = false
            root.replace = false
            root.pending = true
        }
    }
    ContextMenuItem {
        objectName: "importMarkersAtPlayhead"
        text: qsTr("Import markers at the playhead…")
        iconName: "import"
        enabled: Markers.scope === "sequence"
        disabledReason: qsTr("In the Clip tab the file's times are the media's own")
        onTriggered: {
            root.atPlayhead = true
            root.replace = false
            root.pending = true
        }
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "importMarkersReplace"
        text: qsTr("Replace the markers with a file…")
        iconName: "repeat"
        onTriggered: {
            root.atPlayhead = false
            root.replace = true
            root.pending = true
        }
    }
    onClosed: {
        if (root.pending) {
            root.pending = false
            root.importRequested(root.atPlayhead, root.replace)
        }
    }
}
