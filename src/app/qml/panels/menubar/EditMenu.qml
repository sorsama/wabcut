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

    BarMenuItem {
        text: qsTr("Undo")
        command: "edit.undo"
    }
    BarMenuItem {
        text: qsTr("Redo")
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
    BarMenuItem {
        text: qsTr("Deselect All")
        command: "edit.deselectAll"
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Find…")
        command: "edit.find"
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
