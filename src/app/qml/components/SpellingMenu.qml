pragma ComponentBehavior: Bound
import QtQuick

// The right-click menu of a text box with spell check (0.2 graphics, Spelling): on a misspelled word its suggestions,
// Delete Repeated Word, Add to Dictionary and Ignore All, then the usual Cut, Copy, Paste and Select All. `target` is
// the TextEdit / TextArea (a TextArea sets ContextMenu.menu: null so Qt's own menu does not open too); openAt() opens
// the menu at a point of it. Choosing a suggestion emits corrected(text) with the target's whole new text: the owner
// commits it as one edit. In its own file, importing only QtQuick, so the Wabcut ContextMenu is not shadowed by
// QtQuick.Controls'.
ContextMenu {
    id: menu
    objectName: "spellingMenu"

    property TextEdit target: null
    property string language: ""
    /// The misspelling the menu is open for ({ start, end, word, action, suggestions }), or {} on a right word.
    property var issue: ({})
    readonly property bool misspelled: issue.word !== undefined
    readonly property bool hasSelection: target !== null && (target.selectedText ?? "") !== ""

    signal corrected(string text)

    function openAt(x, y) {
        if (!target)
            return false
        const position = target.positionAt(x, y)
        issue = Spelling.issueAt(target.text, position, language)
        popup(target, x, y)
        return true
    }
    function choose(replacement) {
        corrected(Spelling.corrected(target.text, issue.start, issue.end, replacement))
        close()
    }

    Instantiator {
        model: (menu.issue.suggestions ?? []).slice(0, 6)
        delegate: ContextMenuItem {
            required property var modelData
            required property int index
            objectName: "spellingSuggestion_" + index
            text: modelData
            font.bold: index === 0
            onTriggered: menu.choose(modelData)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object as ContextMenuItem)
        onObjectRemoved: (index, object) => menu.removeItem(object as ContextMenuItem)
    }
    ContextMenuItem {
        objectName: "spellingDelete"
        visible: menu.misspelled && menu.issue.action === "delete"
        height: visible ? implicitHeight : 0
        text: qsTr("Delete Repeated Word")
        onTriggered: menu.choose("")
    }
    ContextMenuItem {
        objectName: "spellingNoSuggestions"
        visible: menu.misspelled && (menu.issue.suggestions ?? []).length === 0 && menu.issue.action !== "delete"
        height: visible ? implicitHeight : 0
        enabled: false
        text: qsTr("No suggestions")
    }
    ContextMenuItem {
        objectName: "spellingAddWord"
        visible: menu.misspelled
        height: visible ? implicitHeight : 0
        text: qsTr("Add to Dictionary")
        iconName: "book-open"
        onTriggered: Spelling.addWord(menu.issue.word)
    }
    ContextMenuItem {
        objectName: "spellingIgnore"
        visible: menu.misspelled
        height: visible ? implicitHeight : 0
        text: qsTr("Ignore All")
        iconName: "eye-off"
        onTriggered: Spelling.ignoreWord(menu.issue.word)
    }
    ContextMenuSeparator {
        visible: menu.misspelled
        height: visible ? implicitHeight : 0
    }
    ContextMenuItem {
        objectName: "textCut"
        text: qsTr("Cut")
        enabled: menu.hasSelection && !(menu.target?.readOnly ?? false)
        onTriggered: menu.target.cut()
    }
    ContextMenuItem {
        objectName: "textCopy"
        text: qsTr("Copy")
        iconName: "copy"
        enabled: menu.hasSelection
        onTriggered: menu.target.copy()
    }
    ContextMenuItem {
        objectName: "textPaste"
        text: qsTr("Paste")
        iconName: "clipboard-paste"
        enabled: (menu.target?.canPaste ?? false) && !(menu.target?.readOnly ?? false)
        onTriggered: menu.target.paste()
    }
    ContextMenuItem {
        objectName: "textSelectAll"
        text: qsTr("Select All")
        onTriggered: menu.target.selectAll()
    }
}
