pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// A name for an Effects panel item (0.2): New Presets Bin, renaming a custom bin or a presets bin. `submit(text)` returns
// "" (the dialog closes) or why not (shown).
WabcutDialog {
    id: dialog
    objectName: "effectNameDialog"

    property var submit: function(text) { return "" }
    property string okText: qsTr("OK")

    function ask(heading, initial, ok, onSubmit) {
        title = heading
        field.text = initial
        okText = ok
        submit = onSubmit
        errorText.text = ""
        open()
        field.forceActiveFocus()
        field.selectAll()
    }
    function accept() {
        const error = dialog.submit(field.text.trim())
        if (error === "")
            dialog.close()
        else
            errorText.text = error
    }

    FieldInput {
        id: field
        objectName: "effectNameField"
        Layout.fillWidth: true
        Accessible.name: dialog.title
        onAccepted: dialog.accept()
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
    }
    Text {
        id: errorText
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
            objectName: "effectNameOk"
            variant: "primary"
            text: dialog.okText
            enabled: field.text.trim() !== ""
            onClicked: dialog.accept()
        }
    ]
}
