pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// P1 round (#49): Home's Rename… and Duplicate… of a project - its new name (it stays in its folder), checked as it is
// typed (Home.validateRename / validateProject) with the reason under the field. `nameChosen` reports the choice once
// the dialog has closed; HomeScreen does the work (Home.renameProject / duplicateProject).
WabcutDialog {
    id: dialog
    objectName: "homeProjectNameDialog"

    /// "rename" or "duplicate".
    property string mode: "rename"
    /// The project (a Home.recent entry) and its folder.
    property var project: null
    property url folder: ""
    /// Why the name typed cannot be used ("" when it can).
    readonly property string problem: dialog.project === null ? ""
                                      : dialog.mode === "rename" ? Home.validateRename(dialog.project.url, nameField.text)
                                      : Home.validateProject(nameField.text, dialog.folder)

    signal nameChosen(string mode, var project, string name)

    function openRename(project: var) {
        dialog.start("rename", project, project.name)
    }
    function openDuplicate(project: var) {
        dialog.start("duplicate", project, Home.duplicateName(project.url))
    }
    function start(mode: string, project: var, name: string) {
        dialog.mode = mode
        dialog.project = project
        dialog.folder = Home.existingFolder(project.url)
        nameField.text = name
        dialog.open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }
    function accept() {
        if (dialog.problem !== "" || dialog.project === null)
            return
        const chosen = dialog.project
        const mode = dialog.mode
        const name = nameField.text.trim()
        dialog.close()
        dialog.nameChosen(mode, chosen, name)
    }

    title: dialog.project === null ? "" : dialog.mode === "rename" ? qsTr("Rename %1").arg(dialog.project.name)
                                                                    : qsTr("Duplicate %1").arg(dialog.project.name)

    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: dialog.mode === "rename"
              ? qsTr("The project file and its autosaves, backups and render previews take the new name. Its media stay where they are.")
              : qsTr("A copy of the project as saved, in the same folder. Unsaved changes of a project open in Wabcut are not in it.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    FieldInput {
        id: nameField
        objectName: "homeProjectNameField"
        Layout.fillWidth: true
        Accessible.name: qsTr("Project name")
        placeholderText: qsTr("Project name")
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
                                                              || event.key === Qt.Key_Delete
        onAccepted: dialog.accept()
    }
    Text {
        objectName: "homeProjectNameProblem"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        text: nameField.text.trim() === "" ? "" : dialog.problem
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "homeProjectNameCancel"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "homeProjectNameAccept"
            variant: "primary"
            text: dialog.mode === "rename" ? qsTr("Rename") : qsTr("Duplicate")
            enabled: dialog.problem === ""
            disabledReason: dialog.problem
            onClicked: dialog.accept()
        }
    ]
}
