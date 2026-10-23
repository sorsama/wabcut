pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The Effects panel asks before it deletes (UX rework, Premiere's habit): a custom bin, one of the user's presets bins
// with its presets, a preset, or an item out of a custom bin. Library changes are not undo steps, so nothing goes
// without this. `ask(question, confirmText, action)`; `action()` runs on the confirm button (Enter), Esc cancels.
WabcutDialog {
    id: dialog
    objectName: "effectsConfirmDialog"

    property string question: ""
    property string confirmText: qsTr("Delete")
    property var action: null

    function ask(text: string, confirm: string, onConfirm: var) {
        dialog.question = text
        dialog.confirmText = confirm !== "" ? confirm : qsTr("Delete")
        dialog.action = onConfirm
        dialog.open()
        okButton.forceActiveFocus(Qt.OtherFocusReason)
    }
    function confirm() {
        const run = dialog.action
        dialog.action = null
        dialog.close()
        if (run)
            run()
    }

    title: dialog.confirmText === qsTr("Remove") ? qsTr("Remove from the custom bin?") : qsTr("Delete?")

    Text {
        objectName: "effectsConfirmText"
        Layout.fillWidth: true
        text: dialog.question
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    footerData: [
        PillButton {
            objectName: "effectsConfirmCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            id: okButton
            objectName: "effectsConfirmOk"
            variant: "danger"
            text: dialog.confirmText
            onClicked: dialog.confirm()
            Keys.onReturnPressed: dialog.confirm()
            Keys.onEnterPressed: dialog.confirm()
        }
    ]
}
