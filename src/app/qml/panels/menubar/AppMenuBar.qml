pragma ComponentBehavior: Bound
import QtQuick

// The title bar's menu bar (UX rework, Premiere's menus): File, Edit, Clip, Sequence, Markers, Graphics, View, Window and
// Help as text titles after the brand. A click opens a menu under its title (again: closes it); while one is open,
// pointing at another title opens that one and Left / Right go to the neighbouring menu; Alt+F/E/C/S/M/G/V/W/H open them
// from anywhere (the first row highlighted; not on macOS, and not a letter the keymap binds with Alt). When the title
// bar is short of room (`collapsed`: the workspace tabs need it), one "Menu" button holds the same menus as submenus.
// Choosing a row: the menus close, the focus they took goes back to where it was before the first menu opened, then the
// row's action runs (run()) - a keymap command through KeymapDispatcher.trigger, so focus-routed commands (Add Marker,
// the Source Monitor's marks) act on the panel you were in and a dialog it opens keeps the focus. A row's state follows
// that panel too (commandState). Edit's text rows act on a text field that had the focus (textEditorBefore). Actions only
// the window can do come out as actionRequested (Main.menuAction).
Item {
    id: root
    objectName: "appMenuBar"

    /// One "Menu" button instead of the titles (the title bar is short of room).
    property bool collapsed: false
    /// The editor's docking (the Window menu: open panels, the maximized frame).
    property DockManager docking: null
    /// The current workspace's id (the Window menu's Workspaces).
    property int workspace: 0

    /// An action only the window can do ("home", "openRecent" (url), "revert", "openPanel" (key), "workspace" (id),
    /// "help" (what), "about", ...: Main.menuAction), once the menus have closed.
    signal actionRequested(string action, var argument)

    /// The menus, in the bar's order.
    readonly property list<MenuBarMenu> menus: [fileMenu, editMenu, clipMenu, sequenceMenu, markersMenu, graphicsMenu,
                                                viewMenu, windowMenu, helpMenu]
    /// The panels' focus keys by panel key (the Window menu's; the palette's "Open panel" rows show them too).
    readonly property var panelCommands: windowMenu.panelCommands
    /// The titles' width in a row (whether the bar is collapsed or not: the title bar decides `collapsed` from it).
    readonly property real expandedWidth: {
        let width = 0
        for (let i = 0; i < titles.count; ++i)
            width += titles.itemAt(i)?.implicitWidth ?? 0
        return width + Math.max(0, titles.count - 1) * titleRow.spacing
    }
    /// The bar menu open now (or the collapsed bar's menu), else null.
    property MenuBarMenu openMenu: null
    // The focus before the first menu opened (it comes back when they close), whether the keyboard had put it there, and
    // its keymap scope ("sourceMonitor", "timeline", ...: the panel the rows act on).
    property Item focusBefore: null
    property bool focusBeforeVisual: false
    property string focusScope: ""
    /// Alt+letter opens the menus on Windows and Linux. macOS has no menu mnemonics: Option+letter types characters
    /// there (é, ç, ß, µ...), and its key set binds some of these chords (⌥M: Clear Selected Marker).
    readonly property bool mnemonics: KeymapDispatcher.platform !== "macos"
    // A chosen row's action, waiting for the menus to close (run()), and whether the focus comes back first.
    property var pending: null
    property bool pendingRestoresFocus: true

    implicitWidth: root.collapsed ? menuButton.implicitWidth : root.expandedWidth
    implicitHeight: menuButton.implicitHeight

    /// Whether any of the bar's menus (or the collapsed bar's) shows.
    function anyOpen(): bool {
        for (let i = 0; i < root.menus.length; ++i) {
            if (root.menus[i].visible)
                return true
        }
        const collapsedMenu = collapsedLoader.item as MenuBarMenu
        return collapsedMenu !== null && collapsedMenu.visible
    }
    function indexOf(menu: MenuBarMenu): int {
        for (let i = 0; i < root.menus.length; ++i) {
            if (root.menus[i] === menu)
                return i
        }
        return -1
    }
    // Remembers where the focus is before the first menu opens.
    function capture() {
        if (root.anyOpen())
            return
        const window = root.Window.window
        const focus = window ? window.activeFocusItem : null
        root.focusBefore = focus
        root.focusBeforeVisual = (focus as MenuBarButton)?.visualFocus ?? false
        root.focusScope = focus ? KeymapDispatcher.scopeAt(focus) : ""
    }
    /// Whether Alt+`letter` may open a menu: the keymap does not bind it (a command the user put on it wins; the Keyboard
    /// Shortcuts editor shows that command on the key).
    function mnemonicFree(letter: string): bool {
        return KeymapDispatcher.revision >= 0 && Shortcuts.commandsForKey(letter.charCodeAt(0), Qt.AltModifier).length === 0
    }

    // ---- A row's state and its command, for the panel the menus act on ----
    /// The text field or text area that had the focus before the menus opened, else null.
    function textEditorBefore(): Item {
        const input = root.focusBefore as TextInput
        if (input !== null)
            return input
        return root.focusBefore as TextEdit
    }
    /// Edit's rows whose keys a focused text editor keeps (KeymapDispatcher leaves Ctrl+Z / Y / X / C / V / A and Delete
    /// to it): from the menu they act on its text too, not on the timeline.
    function isTextCommand(command: string): bool {
        switch (command) {
        case "edit.undo":
        case "edit.redo":
        case "edit.cut":
        case "edit.copy":
        case "edit.paste":
        case "edit.clear":
        case "edit.selectAll":
            return true
        }
        return false
    }
    /// The state of a row's keymap command while its menu shows ({enabled, reason}, like CommandPalette.commandState),
    /// where it will act: a focused text editor's text for Edit's text rows; the Source Monitor's clip and marks for the
    /// commands it runs itself while it has the focus (Main.runCommand gives them to SourceMonitor.runCommand first);
    /// else the palette's answer (the Timeline's marks, the selection, the clipboard...).
    function commandState(command: string): var {
        const editor = root.textEditorBefore()
        if (editor !== null && root.isTextCommand(command))
            return root.textState(editor, command)
        if (root.focusScope === "sourceMonitor") {
            const source = root.sourceState(command)
            if (source !== null)
                return source
        }
        return CommandPalette.commandState(command)
    }
    function textState(editor: Item, command: string): var {
        const input = editor as TextInput
        const area = editor as TextEdit
        const readOnly = input !== null ? input.readOnly : (area?.readOnly ?? true)
        const selected = (input !== null ? input.selectedText : (area?.selectedText ?? "")) !== ""
        let enabled = false
        let reason = ""
        switch (command) {
        case "edit.undo":
            enabled = input !== null ? input.canUndo : (area?.canUndo ?? false)
            reason = qsTr("Nothing to undo")
            break
        case "edit.redo":
            enabled = input !== null ? input.canRedo : (area?.canRedo ?? false)
            reason = qsTr("Nothing to redo")
            break
        case "edit.copy":
            enabled = selected
            reason = qsTr("Select some text first")
            break
        case "edit.cut":
        case "edit.clear":
            enabled = selected && !readOnly
            reason = readOnly ? qsTr("This text cannot be changed") : qsTr("Select some text first")
            break
        case "edit.paste":
            enabled = !readOnly && (input !== null ? input.canPaste : (area?.canPaste ?? false))
            reason = readOnly ? qsTr("This text cannot be changed") : qsTr("There is no text to paste")
            break
        case "edit.selectAll":
            enabled = (input !== null ? input.length : (area?.length ?? 0)) > 0
            reason = qsTr("There is no text")
            break
        }
        return { enabled: enabled, reason: enabled ? "" : reason }
    }
    function sourceState(command: string): var {
        const ready = { enabled: true, reason: "" }
        switch (command) {
        case "marker.markIn":
        case "marker.markOut":
        case "marker.markClip":
        case "marker.add":
        case "sequence.reverseMatchFrame":
            return SourceMonitor.loaded ? ready : { enabled: false, reason: qsTr("No clip is open in the Source Monitor") }
        case "marker.goToIn":
        case "marker.clearIn":
            return SourceMonitor.hasIn ? ready : { enabled: false, reason: qsTr("No In mark is set") }
        case "marker.goToOut":
        case "marker.clearOut":
            return SourceMonitor.hasOut ? ready : { enabled: false, reason: qsTr("No Out mark is set") }
        case "marker.clearInAndOut":
            return SourceMonitor.hasIn || SourceMonitor.hasOut ? ready
                                                               : { enabled: false, reason: qsTr("No In or Out mark is set") }
        case "marker.markSelection":
        case "sequence.matchFrame":
            // The Source Monitor keeps these (they never reach the sequence behind it).
            return { enabled: false, reason: qsTr("Only in the Timeline") }
        }
        return null
    }
    /// A row's keymap command, once the menus have closed and the focus is back where it was: Edit's text rows on the
    /// text editor that had the focus, every other one through KeymapDispatcher.trigger (Main.runCommand).
    function runCommand(command: string) {
        const editor = root.isTextCommand(command) ? root.textEditorBefore() : null
        root.run(() => {
            if (editor !== null)
                root.editText(editor, command)
            else
                KeymapDispatcher.trigger(command)
        }, true)
    }
    function editText(editor: Item, command: string) {
        const input = editor as TextInput
        const area = editor as TextEdit
        switch (command) {
        case "edit.undo":
            if (input !== null) input.undo(); else area?.undo()
            break
        case "edit.redo":
            if (input !== null) input.redo(); else area?.redo()
            break
        case "edit.cut":
            if (input !== null) input.cut(); else area?.cut()
            break
        case "edit.copy":
            if (input !== null) input.copy(); else area?.copy()
            break
        case "edit.paste":
            if (input !== null) input.paste(); else area?.paste()
            break
        case "edit.clear":
            if (input !== null)
                input.remove(input.selectionStart, input.selectionEnd)
            else if (area !== null)
                area.remove(area.selectionStart, area.selectionEnd)
            break
        case "edit.selectAll":
            if (input !== null) input.selectAll(); else area?.selectAll()
            break
        }
    }
    // The first row the keyboard can choose gets the highlight (and the focus).
    function highlightFirst(menu: MenuBarMenu) {
        for (let i = 0; i < menu.count; ++i) {
            const item = menu.itemAt(i) as BarMenuItem
            if (item && item.enabled && item.visible && item.height > 0) {
                menu.currentIndex = i
                return
            }
        }
    }

    /// Opens the menu `index` (0 File ... 8 Help) under its title; `keyboard`: its first row highlighted. Collapsed: the
    /// Menu button's menu, with that submenu open.
    function open(index: int, keyboard: bool) {
        if (index < 0 || index >= root.menus.length)
            return
        if (root.collapsed) {
            root.openCollapsed(index)
            return
        }
        const menu = root.menus[index]
        const title = titles.itemAt(index) as MenuBarButton
        if (!title)
            return
        root.capture()
        const previous = root.openMenu
        root.openMenu = menu
        if (previous && previous !== menu && previous.visible)
            previous.close()
        menu.popup(title, 0, title.height + Theme.space(1))
        if (keyboard)
            root.highlightFirst(menu)
    }
    /// The menu `key` ("file", "edit", ...): open().
    function openMenuByKey(key: string, keyboard: bool) {
        for (let i = 0; i < root.menus.length; ++i) {
            if (root.menus[i].key === key)
                root.open(i, keyboard)
        }
    }
    // A title's click: its menu, or closed when it is the one open.
    function toggle(index: int, keyboard: bool) {
        const menu = root.menus[index]
        if (root.openMenu === menu && menu.visible)
            menu.close()
        else
            root.open(index, keyboard)
    }
    /// The Menu button's menu (collapsed), with the submenu `index` open (-1: none).
    function openCollapsed(index: int) {
        const menu = collapsedLoader.item as MenuBarMenu
        if (!menu)
            return
        root.capture()
        root.openMenu = menu
        if (!menu.visible)
            menu.popup(menuButton, 0, menuButton.height + Theme.space(1))
        if (index < 0) {
            root.highlightFirst(menu)
            return
        }
        menu.currentIndex = index
        // A submenu row opens its submenu (first row highlighted) when it is triggered.
        const row = menu.itemAt(index) as BarMenuItem
        row?.triggered()
    }
    /// Left (-1) / Right (+1) inside an open menu: the neighbouring one, its first row highlighted. False when there is
    /// none to go to (collapsed: the menus are submenus there).
    function step(delta: int): bool {
        if (root.collapsed)
            return false
        const index = root.indexOf(root.openMenu)
        if (index < 0)
            return false
        root.open((index + delta + root.menus.length) % root.menus.length, true)
        return true
    }

    /// Runs `action` once every menu of the bar has closed, after giving the focus back to where it was before the menu
    /// opened (`restoreFocus`; off for actions that focus something themselves). With no menu open (a test, a row
    /// triggered from code): at once.
    function run(action: var, restoreFocus: bool) {
        if (!root.anyOpen()) {
            Qt.callLater(action)
            return
        }
        root.pending = action
        root.pendingRestoresFocus = restoreFocus
    }
    /// A menu of the bar has closed (MenuBarMenu.onClosed): once they all have, the waiting action runs.
    function menuClosed(menu: MenuBarMenu) {
        if (menu === root.openMenu)
            root.openMenu = null
        Qt.callLater(root.settle)
    }
    function settle() {
        if (root.anyOpen())
            return
        const action = root.pending
        const restore = root.pendingRestoresFocus
        const before = root.focusBefore
        const visual = root.focusBeforeVisual
        root.pending = null
        root.pendingRestoresFocus = true
        root.focusBefore = null
        root.focusBeforeVisual = false
        root.focusScope = ""
        // A closed menu leaves the focus on the window itself: it goes back (always before an action, so that the
        // command acts where you were; without one only when nothing else took it, e.g. a click into a panel).
        const window = root.Window.window
        const current = window ? window.activeFocusItem : null
        const lost = current === null || (window !== null && current === window.contentItem) || !current.visible
        if (before && before.visible && before.enabled && (action ? restore : lost))
            before.forceActiveFocus(visual ? Qt.TabFocusReason : Qt.OtherFocusReason)
        if (action)
            action()
    }

    // The titles.
    Row {
        id: titleRow
        objectName: "menuBarTitles"
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.collapsed
        spacing: 0

        Repeater {
            id: titles
            model: root.menus
            delegate: MenuBarButton {
                id: title
                required property MenuBarMenu modelData
                required property int index
                objectName: "menuBar_" + title.modelData.key
                text: title.modelData.title
                open: root.openMenu === title.modelData && title.modelData.visible
                onClicked: root.toggle(title.index, title.visualFocus)
                onKeyboardOpenRequested: root.open(title.index, true)
                onStepRequested: delta => {
                    const next = titles.itemAt((title.index + delta + titles.count) % titles.count) as MenuBarButton
                    next?.forceActiveFocus(Qt.TabFocusReason)
                }
                // A menu is open: pointing at another title opens that one (a native menu bar's way).
                onHoveredChanged: {
                    if (title.hovered && root.openMenu !== null && root.openMenu !== title.modelData
                            && root.openMenu.visible && !root.collapsed)
                        root.open(title.index, false)
                }
            }
        }
    }

    // Collapsed: one button, the menus as its submenus.
    MenuBarButton {
        id: menuButton
        objectName: "menuBar_menu"
        anchors.verticalCenter: parent.verticalCenter
        visible: root.collapsed
        text: qsTr("Menu")
        iconName: "chevron-down"
        open: root.collapsed && (collapsedLoader.item as MenuBarMenu)?.visible === true
        onClicked: {
            const menu = collapsedLoader.item as MenuBarMenu
            if (menu && menu.visible)
                menu.close()
            else
                root.openCollapsed(-1)
        }
        onKeyboardOpenRequested: root.openCollapsed(-1)
    }

    // The collapsed bar's menu: the same menus again, as submenus (made only while collapsed).
    Loader {
        id: collapsedLoader
        objectName: "menuBarCollapsedLoader"
        active: root.collapsed
        sourceComponent: MenuBarMenu {
            objectName: "menuBarCollapsedMenu"
            title: qsTr("Menu")
            bar: root

            FileMenu {
                objectName: "collapsedFileMenu"
                nested: true
                bar: root
            }
            EditMenu {
                objectName: "collapsedEditMenu"
                nested: true
                bar: root
            }
            ClipMenu {
                objectName: "collapsedClipMenu"
                nested: true
                bar: root
            }
            SequenceBarMenu {
                objectName: "collapsedSequenceMenu"
                nested: true
                bar: root
            }
            MarkersMenu {
                objectName: "collapsedMarkersMenu"
                nested: true
                bar: root
            }
            GraphicsMenu {
                objectName: "collapsedGraphicsMenu"
                nested: true
                bar: root
            }
            ViewMenu {
                objectName: "collapsedViewMenu"
                nested: true
                bar: root
            }
            WindowMenu {
                objectName: "collapsedWindowMenu"
                nested: true
                bar: root
                docking: root.docking
                workspace: root.workspace
            }
            HelpMenu {
                objectName: "collapsedHelpMenu"
                nested: true
                bar: root
            }
        }
    }

    // The bar's menus (popups: they show under their titles).
    FileMenu {
        id: fileMenu
        bar: root
    }
    EditMenu {
        id: editMenu
        bar: root
    }
    ClipMenu {
        id: clipMenu
        bar: root
    }
    SequenceBarMenu {
        id: sequenceMenu
        bar: root
    }
    MarkersMenu {
        id: markersMenu
        bar: root
    }
    GraphicsMenu {
        id: graphicsMenu
        bar: root
    }
    ViewMenu {
        id: viewMenu
        bar: root
    }
    WindowMenu {
        id: windowMenu
        bar: root
        docking: root.docking
        workspace: root.workspace
    }
    HelpMenu {
        id: helpMenu
        bar: root
    }

    // Alt+letter: the menu, its first row highlighted (not while the editor is covered: Home, a disabled shell; not on
    // macOS; not when the keymap binds the chord: mnemonicFree).
    Instantiator {
        model: root.menus
        delegate: Shortcut {
            required property MenuBarMenu modelData
            required property int index
            objectName: "menuBarKey_" + modelData.key
            sequence: "Alt+" + modelData.mnemonic
            enabled: root.enabled && root.visible && root.mnemonics && root.mnemonicFree(modelData.mnemonic)
            onActivated: root.open(index, true)
        }
    }
}
