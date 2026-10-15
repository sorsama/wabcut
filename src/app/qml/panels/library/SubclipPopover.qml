pragma ComponentBehavior: Bound
import QtQuick

// Make Subclip (Ctrl/⌘+U, from the Source Monitor's marks) and Edit Subclip (In, Out and the restriction) (M5k).
// Premiere's "Restrict Trims To Subclip Boundaries" is on by default: edits of the subclip's clips stay inside it.
Popover {
    id: root
    objectName: "subclipPopover"

    /// "" = Make Subclip; a subclip's key = Edit Subclip.
    property string editing: ""
    /// Make Subclip of a library item from its stored marks ("" = the Source Monitor's clip).
    property string source: ""
    readonly property bool making: editing === ""

    function openMake(target) {
        source = ""
        editing = ""
        nameField.text = ""
        restrictToggle.checked = true
        parent = target
        open()
        nameField.forceActiveFocus()
    }
    function openMakeOf(key, target) {
        openMake(target)
        source = key
    }
    function openEdit(key, target) {
        const d = Library.details(key)
        if (d.kind !== "subclip")
            return
        editing = key
        nameField.text = d.name
        inField.text = d.inText
        outField.text = d.outText
        restrictToggle.checked = d.restricted
        parent = target
        open()
        inField.forceActiveFocus()
    }
    function accept() {
        let ok = false
        if (root.making) {
            ok = (root.source !== "" ? Library.makeSubclipOf(root.source, nameField.text, restrictToggle.checked)
                                      : Library.makeSubclip(nameField.text, restrictToggle.checked)) !== ""
        } else {
            ok = Library.editSubclip(root.editing, inField.text, outField.text, restrictToggle.checked)
            if (ok && nameField.text.trim() !== Library.details(root.editing).name)
                Library.rename(root.editing, nameField.text)
        }
        if (ok)
            root.close()
    }

    title: making ? qsTr("Make subclip") : qsTr("Edit subclip")

    Column {
        width: Theme.space(72)
        spacing: Theme.space(2)

        FieldInput {
            id: nameField
            objectName: "subclipName"
            width: parent.width
            Accessible.name: qsTr("Subclip name")
            placeholderText: root.making ? qsTr("Name (the clip's name + .Subclip)") : qsTr("Name")
            onAccepted: root.accept()
            Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
        }
        Row {
            visible: !root.making
            spacing: Theme.space(2)
            FieldInput {
                id: inField
                objectName: "subclipIn"
                width: Theme.space(34)
                Accessible.name: qsTr("Subclip In")
                placeholderText: qsTr("In")
                font.family: Theme.fontMono
                onAccepted: root.accept()
                Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            }
            FieldInput {
                id: outField
                objectName: "subclipOut"
                width: Theme.space(34)
                Accessible.name: qsTr("Subclip Out")
                placeholderText: qsTr("Out")
                font.family: Theme.fontMono
                onAccepted: root.accept()
                Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            }
        }
        Text {
            visible: root.making
            width: parent.width
            wrapMode: Text.WordWrap
            text: root.source !== "" ? qsTr("Uses the clip's In and Out marks.") : qsTr("Uses the Source Monitor's In and Out marks.")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Toggle {
            id: restrictToggle
            objectName: "subclipRestrict"
            text: qsTr("Restrict trims to subclip boundaries")
            checked: true
        }
        Row {
            spacing: Theme.space(2)
            PillButton {
                objectName: "subclipAccept"
                variant: "primary"
                text: root.making ? qsTr("Make subclip") : qsTr("Save")
                onClicked: root.accept()
            }
            PillButton {
                variant: "ghost"
                text: qsTr("Cancel")
                onClicked: root.close()
            }
        }
    }
}
