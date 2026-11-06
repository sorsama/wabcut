pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Settings > General > Backup and reset (0.7, card 028): the confirmation before a settings file replaces the current
// settings (`mode` "import": what the file holds, from AppSettings.previewImport) or before everything is reset to the
// defaults (`mode` "reset": the backup that is written first is named, recent projects and opt-ins can be kept). Nothing
// changes until the button is pressed; then the change is staged (a backup bundle is written first) and Wabcut restarts
// to apply it (`confirmed` tells the page, which asks AppShell for the restart).
WabcutDialog {
    id: dialog
    objectName: "settingsBackupDialog"

    /// "import" or "reset".
    property string mode: "reset"
    /// import: AppSettings.previewImport's map, and the file it was read from.
    property var preview: ({})
    property url file
    /// reset: AppSettings.planReset's map ({ backupName, backupFolder }).
    property var plan: ({})
    property bool keepRecents: true
    /// Set when staging failed: the reason is shown and the buttons stay.
    property string problem: ""

    /// The change was staged; the page restarts Wabcut.
    signal confirmed()

    function openImport(previewMap, fileUrl) {
        preview = previewMap
        file = fileUrl
        mode = "import"
        problem = ""
        open()
    }
    function openReset() {
        mode = "reset"
        plan = AppSettings.planReset()
        keepRecents = true
        problem = ""
        open()
    }
    function confirm() {
        const result = mode === "import" ? AppSettings.importSettings(dialog.file)
                                         : AppSettings.resetAllSettings(dialog.keepRecents, dialog.plan.backupName)
        if (result.ok) {
            dialog.close()
            dialog.confirmed()
        } else {
            dialog.problem = result.error
        }
    }

    readonly property bool unusable: mode === "import" && preview.ok !== true

    title: mode === "import" ? (unusable ? qsTr("This file cannot be imported") : qsTr("Import settings?"))
                             : qsTr("Reset all settings?")

    Text {
        objectName: "settingsBackupText"
        Layout.fillWidth: true
        text: dialog.mode === "reset"
              ? qsTr("Wabcut goes back to its default settings: the theme, window layout, Preferences, shortcuts and AI choices. Your projects, media, presets, templates and API keys are not touched.")
              : dialog.unusable ? (dialog.preview.error ?? "")
              : qsTr("These replace your current settings. Keyboard shortcuts, export presets, effect presets, graphic templates and the dictionary in the file replace yours too.")
        color: dialog.unusable ? Theme.danger : Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    // import: what the file holds.
    ColumnLayout {
        objectName: "settingsBackupContents"
        Layout.fillWidth: true
        visible: dialog.mode === "import" && !dialog.unusable
        spacing: Theme.space(1)

        Text {
            Layout.fillWidth: true
            visible: dialog.preview.createdText !== undefined && dialog.preview.createdText !== ""
            //: Settings import: %1 = when the file was made (a date and time), %2 = the Wabcut version that made it
            text: qsTr("Made %1 by Wabcut %2").arg(dialog.preview.createdText).arg(dialog.preview.app || "?")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            wrapMode: Text.Wrap
        }
        Repeater {
            model: dialog.preview.lines || []
            Text {
                required property string modelData
                Layout.fillWidth: true
                text: qsTr("• %1").arg(modelData)
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                wrapMode: Text.Wrap
            }
        }
    }

    // reset: keep the recents and opt-ins?
    RowLayout {
        objectName: "settingsBackupKeepRow"
        Layout.fillWidth: true
        visible: dialog.mode === "reset"
        spacing: Theme.space(3)

        Toggle {
            id: keepToggle
            objectName: "settingsBackupKeepToggle"
            tooltip: qsTr("Keep recent projects and opt-ins")
            checked: dialog.keepRecents
            onToggled: dialog.keepRecents = checked
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Keep recent and starred projects, and my choices for AI features and update checks")
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            wrapMode: Text.Wrap
        }
    }

    Text {
        objectName: "settingsBackupBackupText"
        Layout.fillWidth: true
        visible: !dialog.unusable
        //: %1 = the file name of the backup (settings-2026-11-05-101500.json), %2 = the folder it goes in
        text: qsTr("Your current settings are saved first as %1 in %2, so you can import them back.")
              .arg(dialog.mode === "reset" ? (dialog.plan.backupName || "") : qsTr("a file named settings-<date>-<time>.json"))
              .arg(dialog.mode === "reset" ? (dialog.plan.backupFolder || "") : qsTr("Wabcut's backups folder"))
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    Text {
        objectName: "settingsBackupRestartText"
        Layout.fillWidth: true
        visible: !dialog.unusable
        text: qsTr("Wabcut restarts to apply this. It asks about unsaved changes first.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    Text {
        objectName: "settingsBackupProblem"
        Layout.fillWidth: true
        visible: dialog.problem !== ""
        text: dialog.problem
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            objectName: "settingsBackupCancel"
            text: dialog.unusable ? qsTr("Close") : qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "settingsBackupConfirm"
            visible: !dialog.unusable
            variant: "primary"
            text: dialog.mode === "import" ? qsTr("Replace settings and restart") : qsTr("Reset and restart")
            onClicked: dialog.confirm()
        }
    ]
}
