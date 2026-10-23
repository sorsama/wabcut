import QtQuick
import QtQuick.Layouts

// "Save changes before …?" before New/Open/Quit replace a project with unsaved changes. The caller continues in
// saveChosen / discardChosen; Cancel (or Escape) leaves everything as it is.
// UX rework: `demo` - leaving the edited sample project (it has no file and never counts as unsaved): "Discard your
// changes to the sample project?" with Save As… (saveChosen: Main saves the demo as a new project), Discard, Cancel.
WabcutDialog {
    id: dialog
    objectName: "unsavedChangesDialog"

    /// The sample project with edits (Main.whenSaved sets it before opening).
    property bool demo: false

    signal saveChosen()
    signal discardChosen()

    //: Unsaved-changes prompt; %1 is the project name
    title: dialog.demo ? qsTr("Discard your changes to the sample project?") : qsTr("Save changes to “%1”?").arg(ProjectFile.name)

    Text {
        Layout.fillWidth: true
        text: dialog.demo ? qsTr("The sample project is not saved anywhere. Save it as a project of your own to keep your edits.")
                          : qsTr("Your changes will be lost if you don't save them.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            objectName: "unsavedDiscardButton"
            text: dialog.demo ? qsTr("Discard") : qsTr("Don't Save")
            onClicked: {
                dialog.close()
                dialog.discardChosen()
            }
        },
        PillButton {
            objectName: "unsavedCancelButton"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "unsavedSaveButton"
            text: dialog.demo ? qsTr("Save As…") : qsTr("Save")
            variant: "primary"
            onClicked: {
                dialog.close()
                dialog.saveChosen()
            }
        }
    ]
}
