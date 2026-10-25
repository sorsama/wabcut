import QtQuick

// No `pragma ComponentBehavior: Bound` here: the monitors instantiate the inline Entry, which a bound inline component
// does not allow from another file.
// The monitors' transport overflow (0.4): an "ellipsis" button at the end of a transport row that is too narrow for all
// its buttons. Its menu lists exactly the buttons the row hid (TransportOverflowButton.Entry rows, one per button, in
// the row's order): the same icon, text, shortcut and action, and the check of a checkable one (Loop, Redact,
// Multi-Camera View). A row's action runs once the menu has closed, as the button's click would (a popover it opens
// keeps the focus). Imports only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
IconButton {
    id: root

    /// The rows (Entry, ContextMenuItem, a ContextMenu as a submenu, ContextMenuSeparator).
    default property alias items: menu.contentData
    readonly property alias menu: menu

    /// One hidden button of the row: shown in the menu only while the button itself is hidden.
    component Entry: ContextMenuItem {
        id: entry

        required property IconButton button
        // Chosen: the button's click runs after the menu closed.
        property bool pending: false

        objectName: "overflow_" + entry.button.objectName
        visible: !entry.button.visible
        height: visible ? implicitHeight : 0
        text: entry.button.tooltip
        iconName: entry.button.iconName
        command: entry.button.command
        enabled: entry.button.enabled
        checkable: entry.button.checkable
        checked: entry.button.checked
        Accessible.name: entry.text

        onTriggered: {
            entry.pending = true
            entry.checked = Qt.binding(() => entry.button.checked)
        }
        Connections {
            target: entry.menu
            function onClosed() {
                if (!entry.pending)
                    return
                entry.pending = false
                if (entry.button.checkable)
                    entry.button.toggle()
                entry.button.clicked()
            }
        }
    }

    compact: true
    iconName: "ellipsis"
    tooltip: qsTr("More controls")
    Accessible.name: qsTr("More controls")
    onClicked: menu.open()

    // Above the button; `y` follows the menu's height (its rows are laid out only once it opens, so a popup() offset
    // computed at the click would be stale the first time).
    ContextMenu {
        id: menu
        objectName: root.objectName + "Menu"
        x: root.width - width // right-aligned: the button ends the row
        y: -height - Theme.space(1)
    }
}
