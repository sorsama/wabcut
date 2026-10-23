pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as T

// A menu of the title bar's menu bar (UX rework, AppMenuBar): Wabcut's ContextMenu whose rows (BarMenuItem) run through
// the bar - the menus close, the focus they took goes back to where it was before the menu opened, then the row's
// action runs - and whose Left / Right keys move to the neighbouring menu of the bar. `nested` for a submenu inside one
// (Left closes it, as in any menu). QtQuick.Controls is imported under a name: the unqualified ContextMenu must stay
// Wabcut's (Qt has a type of that name).
ContextMenu {
    id: root

    /// The menu bar that shows this menu (an AppMenuBar): its rows run through it. Null: they run at once.
    property Item bar: null
    readonly property AppMenuBar menuBar: root.bar as AppMenuBar
    /// The menu's key (the bar's title is "menuBar_<key>").
    property string key: ""
    /// The letter of the Alt+letter key that opens it ("F" for File); "" for none.
    property string mnemonic: ""
    /// A submenu (declared inside another menu, or one of the collapsed bar's menus).
    property bool nested: false

    // A press on the menu's own title (its parent while it is open) leaves it to the title, which closes it; a press
    // anywhere else closes it.
    closePolicy: root.nested ? T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutside
                             : T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
    // Submenu rows (a menu declared inside this one) are bar rows too, for Left / Right.
    delegate: BarMenuItem {}

    /// Runs `action` once the menus have closed and the focus is back where it was (AppMenuBar.run).
    function run(action: var) {
        if (root.menuBar)
            root.menuBar.run(action, true)
        else
            Qt.callLater(action)
    }
    /// Like run(), for actions that move the focus themselves (a panel from the Window menu): the focus is not put back
    /// first.
    function runMovingFocus(action: var) {
        if (root.menuBar)
            root.menuBar.run(action, false)
        else
            Qt.callLater(action)
    }
    /// A keymap command, as its keys would run it (KeymapDispatcher.trigger: Main.runCommand; Edit's text rows on a
    /// focused text field: AppMenuBar.runCommand).
    function runCommand(command: string) {
        if (root.menuBar)
            root.menuBar.runCommand(command)
        else
            Qt.callLater(() => KeymapDispatcher.trigger(command))
    }
    /// An action only the window can do (AppMenuBar.actionRequested: Main.menuAction), once the menus have closed.
    function request(action: string, argument: var) {
        root.run(() => root.menuBar?.actionRequested(action, argument))
    }
    /// Left (-1) or Right (+1) from one of its rows: the neighbouring menu of the bar. False: the menu handles the key
    /// (Left closes a submenu).
    function step(delta: int): bool {
        if (!root.menuBar || (root.nested && delta < 0))
            return false
        return root.menuBar.step(delta)
    }

    onClosed: root.menuBar?.menuClosed(root)
}
