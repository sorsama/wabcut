pragma ComponentBehavior: Bound
import QtQuick

// Edit (the menu bar, Premiere's order): history, the clipboard, deleting, selecting, Find, the shortcuts editor and
// the preferences. Every row is a keymap command (its shortcut shows; greyed out while it cannot run). With a text field
// focused before the menu opened, Undo, Redo, Cut, Copy, Paste, Clear and Select All act on its text, as their keys do
// there (AppMenuBar.isTextCommand), and their state is the field's (a selection, something to paste).
MenuBarMenu {
    id: root
    objectName: "editMenu"
    title: qsTr("Edit")
    key: "edit"
    mnemonic: "E"

    // The focus was in a text field when the menu opened: Undo and Redo act on its text, so they keep their plain names
    // (the bar's focusBefore, set as the menu opens, makes the rows follow). Otherwise they name the edit they undo or
    // redo - "Undo Razor", "Redo Ripple Delete" -, as the toolbar's tooltips do. The palette's titles stay static (they
    // come from the keymap registry).
    readonly property bool actsOnText: (root.menuBar?.textEditorBefore() ?? null) !== null

    BarMenuItem {
        text: root.actsOnText ? qsTr("Undo") : ProjectController.undoText
        command: "edit.undo"
    }
    BarMenuItem {
        text: root.actsOnText ? qsTr("Redo") : ProjectController.redoText
        command: "edit.redo"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Cut")
        command: "edit.cut"
    }
    BarMenuItem {
        text: qsTr("Copy")
        command: "edit.copy"
    }
    BarMenuItem {
        text: qsTr("Paste")
        command: "edit.paste"
    }
    BarMenuItem {
        text: qsTr("Paste Insert")
        command: "edit.pasteInsert"
    }
    BarMenuItem {
        text: qsTr("Paste Attributes…")
        command: "edit.pasteAttributes"
    }
    BarMenuItem {
        text: qsTr("Remove Attributes…")
        command: "edit.removeAttributes"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Clear")
        command: "edit.clear"
    }
    BarMenuItem {
        text: qsTr("Ripple Delete")
        command: "edit.rippleDelete"
    }
    BarMenuItem {
        text: qsTr("Duplicate")
        command: "edit.duplicate"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Select All")
        command: "edit.selectAll"
    }
    // 0.7 (card 122): every disabled clip of the sequence, for Delete, Ripple Delete or Enable.
    BarMenuItem {
        text: qsTr("Select All Disabled Clips")
        command: "edit.selectDisabledClips"
    }
    BarMenuItem {
        text: qsTr("Deselect All")
        command: "edit.deselectAll"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Find…")
        command: "edit.find"
    }
    // 0.7 (card 025): Premiere's Remove Unused and Consolidate Duplicates; the Library asks before it changes anything.
    BarMenuItem {
        objectName: "editMenuRemoveUnused"
        text: qsTr("Remove Unused…")
        command: "edit.removeUnused"
    }
    BarMenuItem {
        objectName: "editMenuConsolidateDuplicates"
        text: qsTr("Consolidate Duplicates…")
        command: "edit.consolidateDuplicates"
    }
    // 0.7 (card 010): Premiere's Edit > Label. The 16 named colours (Library.labelNames) set the selected clips' label, the
    // colour they share is checked; Select Label Group selects every clip of the sequence with the selection's colour.
    // Greyed out while no clip is selected.
    MenuBarMenu {
        id: labelMenu
        objectName: "editMenuLabel"
        title: qsTr("Label")
        nested: true
        bar: root.bar
        /// The label colour the selected clips share (-1: they differ or none is selected), read as the menu opens.
        property int current: -1
        onAboutToShow: labelMenu.current = ProjectController.selectionLabel()

        Instantiator {
            model: Library.labelNames
            delegate: BarMenuItem {
                required property string modelData
                required property int index
                objectName: "editMenuLabel_" + index
                text: modelData
                chipColor: Theme.clipColor(index)
                checkable: true
                checked: index === labelMenu.current
                available: ProjectController.selectedClips.length > 0
                onTriggered: {
                    checked = Qt.binding(() => index === labelMenu.current)
                    labelMenu.run(() => ProjectController.setSelectionColor(index))
                }
            }
            onObjectAdded: (index, object) => labelMenu.insertItem(index, object as BarMenuItem)
            onObjectRemoved: (index, object) => labelMenu.removeItem(object as BarMenuItem)
        }
        ContextMenuSeparator {}
        BarMenuItem {
            objectName: "editMenuSelectLabelGroup"
            text: qsTr("Select Label Group")
            command: "edit.selectLabelGroup"
        }
    }
    BarMenuItem {
        text: qsTr("Edit Original")
        command: "edit.editOriginal"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        objectName: "editMenuShortcuts"
        text: qsTr("Keyboard Shortcuts…")
        command: "edit.keyboardShortcuts"
    }
    // P.2 (E5): Premiere's Preferences > Media and Timeline (the editing defaults dialog).
    BarMenuItem {
        objectName: "editingPreferencesMenuItem"
        text: qsTr("Media and Timeline Preferences…")
        command: "edit.preferencesMedia"
    }
    // The Settings window (Ctrl+, / ⌘,: the keymap's edit.preferences).
    BarMenuItem {
        objectName: "editMenuSettings"
        text: qsTr("Settings…")
        command: "edit.preferences"
    }
}
