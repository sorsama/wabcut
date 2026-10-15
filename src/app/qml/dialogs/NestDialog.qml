pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Nest… (M5l, bridge `Nesting`; Premiere's right-click > Nest…): the selected clips (and their linked partners) move
// into a new sequence, and a nested clip takes their place. Asks for the sequence's name ("Nested Sequence 01", …).
WabcutDialog {
    id: dialog
    objectName: "nestDialog"

    function openForSelection() {
        nameField.text = Nesting.defaultName()
        errorText.text = ""
        open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }
    function accept() {
        const error = Nesting.nestSelection(nameField.text)
        if (error === "")
            dialog.close()
        else
            errorText.text = error
    }

    title: qsTr("Nested Sequence Name")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(110), Theme.space(110))

    FieldInput {
        id: nameField
        objectName: "nestName"
        Layout.fillWidth: true
        Accessible.name: qsTr("Nested sequence name")
        onAccepted: dialog.accept()
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
    }
    Text {
        id: errorText
        objectName: "nestError"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "nestOk"
            variant: "primary"
            text: qsTr("Nest")
            enabled: nameField.text.trim() !== ""
            onClicked: dialog.accept()
        }
    ]
}
