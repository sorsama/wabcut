pragma ComponentBehavior: Bound
import QtQuick

// The Freeform view's canvas menu (0.2, P.3: Premiere's Freeform right-click menu): Reset to Grid (by name, label,
// type, duration or date), Align to Grid, the clip size of the selection, Save as New Layout / Restore Layout / Delete
// Layout, and Freeform View Options (the two metadata lines under each card). Its own file: QtQuick.Controls'
// ContextMenu would hide Wabcut's in a file that imports the controls.
ContextMenu {
    id: menu
    objectName: "libraryFreeformMenu"

    /// The canvas width in view units (Reset to Grid fills it).
    property real canvasWidth: 0

    signal saveLayoutRequested()

    ContextMenu {
        objectName: "libraryFreeformReset"
        title: qsTr("Reset to Grid")
        ContextMenuItem {
            objectName: "libraryFreeformResetBy_name"
            text: qsTr("Name")
            onTriggered: Library.freeformResetToGrid("name", 0, menu.canvasWidth)
        }
        ContextMenuItem {
            objectName: "libraryFreeformResetBy_label"
            text: qsTr("Label")
            onTriggered: Library.freeformResetToGrid("label", 0, menu.canvasWidth)
        }
        ContextMenuItem {
            objectName: "libraryFreeformResetBy_type"
            text: qsTr("Media Type")
            onTriggered: Library.freeformResetToGrid("type", 0, menu.canvasWidth)
        }
        ContextMenuItem {
            objectName: "libraryFreeformResetBy_duration"
            text: qsTr("Media Duration")
            onTriggered: Library.freeformResetToGrid("duration", 0, menu.canvasWidth)
        }
        ContextMenuItem {
            objectName: "libraryFreeformResetBy_date"
            text: qsTr("Date modified")
            onTriggered: Library.freeformResetToGrid("date", 0, menu.canvasWidth)
        }
    }
    ContextMenuItem {
        objectName: "libraryFreeformAlign"
        text: Library.selectionCount > 0 ? qsTr("Align selection to grid") : qsTr("Align to Grid")
        iconName: "layout-grid"
        onTriggered: Library.freeformAlignToGrid(Library.selectionCount > 0)
    }
    ContextMenu {
        objectName: "libraryFreeformClipSize"
        title: qsTr("Clip Size")
        enabled: Library.selectionCount > 0
        ContextMenuItem {
            text: qsTr("Small")
            onTriggered: Library.setFreeformSize(Library.selection, 0.5)
        }
        ContextMenuItem {
            text: qsTr("Medium")
            onTriggered: Library.setFreeformSize(Library.selection, 1.0)
        }
        ContextMenuItem {
            text: qsTr("Large")
            onTriggered: Library.setFreeformSize(Library.selection, 1.5)
        }
        ContextMenuItem {
            text: qsTr("Extra large")
            onTriggered: Library.setFreeformSize(Library.selection, 2.0)
        }
    }
    ContextMenuSeparator {}
    ContextMenuItem {
        objectName: "libraryFreeformSaveLayout"
        text: qsTr("Save as New Layout…")
        iconName: "save"
        onTriggered: menu.saveLayoutRequested()
    }
    ContextMenu {
        id: restoreMenu
        objectName: "libraryFreeformRestore"
        title: qsTr("Restore Layout")
        enabled: Library.freeformLayouts.length > 0
        Instantiator {
            model: Library.freeformLayouts
            delegate: ContextMenuItem {
                required property string modelData
                text: modelData
                onTriggered: Library.restoreFreeformLayout(modelData)
            }
            onObjectAdded: (index, object) => restoreMenu.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => restoreMenu.removeItem(object as ContextMenuItem)
        }
    }
    ContextMenu {
        id: deleteMenu
        objectName: "libraryFreeformDeleteLayout"
        title: qsTr("Delete Layout")
        enabled: Library.freeformLayouts.length > 0
        Instantiator {
            model: Library.freeformLayouts
            delegate: ContextMenuItem {
                required property string modelData
                text: modelData
                onTriggered: Library.removeFreeformLayout(modelData)
            }
            onObjectAdded: (index, object) => deleteMenu.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => deleteMenu.removeItem(object as ContextMenuItem)
        }
    }
    ContextMenuSeparator {}
    // Freeform View Options: the two lines under each card.
    ContextMenu {
        id: firstLine
        objectName: "libraryFreeformLine1"
        title: qsTr("First metadata line")
        Instantiator {
            model: [{ id: "", title: qsTr("None") }].concat(Library.allColumns.filter(c => c.id !== "name"))
            delegate: ContextMenuItem {
                required property var modelData
                text: modelData.title
                checkable: true
                checked: (Library.freeformLines[0] ?? "") === modelData.id
                onTriggered: Library.freeformLines = [modelData.id, Library.freeformLines[1] ?? ""]
            }
            onObjectAdded: (index, object) => firstLine.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => firstLine.removeItem(object as ContextMenuItem)
        }
    }
    ContextMenu {
        id: secondLine
        objectName: "libraryFreeformLine2"
        title: qsTr("Second metadata line")
        Instantiator {
            model: [{ id: "", title: qsTr("None") }].concat(Library.allColumns.filter(c => c.id !== "name"))
            delegate: ContextMenuItem {
                required property var modelData
                text: modelData.title
                checkable: true
                checked: (Library.freeformLines[1] ?? "") === modelData.id
                onTriggered: Library.freeformLines = [Library.freeformLines[0] ?? "", modelData.id]
            }
            onObjectAdded: (index, object) => secondLine.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => secondLine.removeItem(object as ContextMenuItem)
        }
    }
}
