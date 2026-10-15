import QtQuick
import QtQuick.Layouts

// "Save changes before …?" before New/Open/Quit replace a project with unsaved changes. The caller continues in
// saveChosen / discardChosen; Cancel (or Escape) leaves everything as it is.
WabcutDialog {
    id: dialog
    objectName: "unsavedChangesDialog"

    signal saveChosen()
    signal discardChosen()

    //: Unsaved-changes prompt; %1 is the project name
    title: qsTr("Save changes to “%1”?").arg(ProjectFile.name)

    Text {
        Layout.fillWidth: true
        text: qsTr("Your changes will be lost if you don't save them.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            text: qsTr("Don't Save")
            onClicked: {
                dialog.close()
                dialog.discardChosen()
            }
        },
        PillButton {
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            text: qsTr("Save")
            variant: "primary"
            onClicked: {
                dialog.close()
                dialog.saveChosen()
            }
        }
    ]
}
