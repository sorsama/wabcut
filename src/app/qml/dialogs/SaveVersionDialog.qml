pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Save Version… (0.7, card 006; File > Save Version…): asks for a name and keeps the project as it is now as a named
// version of its history (History.saveVersion: never an undo step; Review > Versions lists it, File > Version
// History… opens that list). Enter in the field saves; the dialog stays open with the reason when it cannot.
WabcutDialog {
    id: dialog
    objectName: "saveVersionDialog"

    /// A version was saved (its name): Main.qml says so.
    signal saved(string name)

    function openDialog() {
        nameField.text = ""
        errorText.text = ""
        open()
        nameField.forceActiveFocus()
    }
    function accept() {
        const name = nameField.text.trim()
        if (name === "") {
            errorText.text = qsTr("Name the version")
            nameField.forceActiveFocus()
            return
        }
        const error = History.saveVersion(name)
        if (error !== "") {
            errorText.text = error
            return
        }
        dialog.close()
        dialog.saved(name)
    }

    title: qsTr("Save Version")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(130), Theme.space(130))

    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("Keeps the project as it is now under a name you can find again. Restore or compare it from File > Version History.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    FieldInput {
        id: nameField
        objectName: "saveVersionName"
        Layout.fillWidth: true
        placeholderText: qsTr("Name this version, e.g. Client cut v2")
        Accessible.name: qsTr("Version name")
        onAccepted: dialog.accept()
        onTextChanged: errorText.text = ""
    }
    Text {
        id: errorText
        objectName: "saveVersionError"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "saveVersionCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "saveVersionOk"
            variant: "primary"
            iconName: "bookmark"
            text: qsTr("Save Version")
            enabled: nameField.text.trim() !== ""
            onClicked: dialog.accept()
        }
    ]
}
