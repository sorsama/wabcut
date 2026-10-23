pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// "Are you sure?" for a menu bar action that cannot be undone (File > Revert, Window > Workspaces > Delete Workspace…):
// a title, one line of what happens, Cancel and the action's own button. ask() opens it; the action runs once it has
// closed (so a dialog it opens keeps the focus). Escape and Cancel drop it.
WabcutDialog {
    id: dialog
    objectName: "menuConfirmDialog"

    /// What happens (one or two sentences).
    property string text: ""
    /// The confirming button's label ("Revert", "Delete").
    property string confirmText: ""
    // The action waiting for the answer, and whether it was chosen.
    property var action: null
    property bool confirmed: false

    /// Asks `title` / `text`; `confirmText` runs `action`.
    function ask(title: string, text: string, confirmText: string, action: var) {
        dialog.title = title
        dialog.text = text
        dialog.confirmText = confirmText
        dialog.action = action
        dialog.confirmed = false
        dialog.open()
    }

    onClosed: {
        const run = dialog.confirmed ? dialog.action : null
        dialog.action = null
        dialog.confirmed = false
        if (run)
            run()
    }

    Text {
        objectName: "menuConfirmText"
        Layout.fillWidth: true
        text: dialog.text
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            objectName: "menuConfirmCancel"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "menuConfirmButton"
            text: dialog.confirmText
            variant: "primary"
            onClicked: {
                dialog.confirmed = true
                dialog.close()
            }
        }
    ]
}
