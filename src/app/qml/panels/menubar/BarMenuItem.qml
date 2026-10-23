pragma ComponentBehavior: Bound
import QtQuick

// A row of a menu bar menu (MenuBarMenu). With a keymap `command` it shows the command's shortcut, greys out while the
// command cannot run now where it will act (AppMenuBar.commandState - the panel or text field that had the focus -, else
// CommandPalette.commandState; asked each time its menu opens; `available` adds conditions of its own) and runs it
// through the bar (AppMenuBar.runCommand, after the menus closed and the focus came back).
// Rows that do something else set `runsCommand: false` (their `command` then only shows the shortcut) and run their
// action with their menu's run() / request(). Left and Right move to the neighbouring menu of the bar (Right on a
// submenu's row opens the submenu).
// Look: like ContextMenuItem, but without icons and with a check gutter on every row, so the labels of a menu line up
// (a checked row shows the accent dot in it).
ContextMenuItem {
    id: control

    /// Choosing the row runs `command`.
    property bool runsCommand: command !== ""
    /// Conditions of the row's own, on top of the command's state.
    property bool available: true
    /// The command's state while the menu shows ({enabled, reason}; null without a command or while it is closed).
    readonly property var commandInfo: control.command !== "" && (control.menu?.visible ?? false)
                                       ? control.stateOf(control.command) : null

    function stateOf(command: string): var {
        const bar = (control.menu as MenuBarMenu)?.menuBar ?? null
        return bar !== null ? bar.commandState(command) : CommandPalette.commandState(command)
    }

    objectName: control.command !== "" ? "menuCommand_" + control.command : ""
    enabled: control.available && (control.commandInfo === null || control.commandInfo.enabled === true)
    Accessible.description: control.commandInfo !== null && control.commandInfo.enabled !== true
                            ? (control.commandInfo.reason ?? "") : ""

    onTriggered: {
        if (control.runsCommand)
            (control.menu as MenuBarMenu)?.runCommand(control.command)
    }

    Keys.onLeftPressed: event => {
        event.accepted = (control.menu as MenuBarMenu)?.step(-1) ?? false
    }
    Keys.onRightPressed: event => {
        event.accepted = control.subMenu === null && ((control.menu as MenuBarMenu)?.step(1) ?? false)
    }

    contentItem: Item {
        implicitWidth: label.x + label.implicitWidth + Theme.space(6)
                       + (control.subMenu !== null ? Theme.iconSm : keys.implicitWidth)
        implicitHeight: label.implicitHeight

        // The check gutter.
        Rectangle {
            objectName: "barMenuItemCheck"
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.dotSize
            height: Theme.dotSize
            radius: Theme.dotSize / 2
            color: Theme.accentStroke
            visible: control.checkable && control.checked
        }

        Text {
            id: label
            x: Theme.dotSize + Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font.family: control.font.family
            font.pixelSize: control.font.pixelSize
            font.weight: Font.DemiBold
            color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        }

        Icon {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: control.subMenu !== null
            name: "chevron-right"
            size: Theme.iconSm
            color: control.enabled ? Theme.textSecondary : Theme.textDisabled
        }

        Text {
            id: keys
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: control.subMenu === null
            text: control.shortcut
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
    }
}
