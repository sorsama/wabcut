pragma ComponentBehavior: Bound
import QtQuick

// P1 round (#4): the History panel's menu (its "···" button), Premiere's panel menu: Delete (the current step and every
// step after it; Backspace in the panel) and Clear History. They run once the menu has closed, through the keymap
// commands (history.delete / history.clear: Editing asks EditingHost to confirm). Greyed out with the reason when there
// is nothing to delete. Its own QtQuick-only file: a file importing QtQuick.Controls would get Qt's ContextMenu.
ContextMenu {
    id: menu
    objectName: "historyPanelMenu"

    // The command chosen, run once the menu has closed.
    property string pending: ""
    // The rows' state, read each time the menu opens.
    property var deleteState: ({})
    property var clearState: ({})

    onAboutToShow: {
        menu.deleteState = Editing.commandAvailability("history.delete")
        menu.clearState = Editing.commandAvailability("history.clear")
    }
    onClosed: {
        const command = menu.pending
        menu.pending = ""
        if (command !== "")
            Qt.callLater(() => Editing.runCommand(command))
    }

    ContextMenuItem {
        objectName: "historyMenuDelete"
        text: qsTr("Delete")
        iconName: "trash-2"
        command: "history.delete"
        enabled: menu.deleteState.enabled === true
        disabledReason: menu.deleteState.reason ?? ""
        onTriggered: menu.pending = "history.delete"
    }
    ContextMenuItem {
        objectName: "historyMenuClear"
        text: qsTr("Clear History")
        iconName: "x"
        command: "history.clear"
        enabled: menu.clearState.enabled === true
        disabledReason: menu.clearState.reason ?? ""
        onTriggered: menu.pending = "history.clear"
    }
}
