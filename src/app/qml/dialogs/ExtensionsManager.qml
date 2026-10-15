pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Settings > Extensions (C.12, M5n; bridge `Extensions`): the extensions installed for this user and the ones that came
// with the open project, each an ExtensionRow (status, permissions, commands, crash badge, actions, log). "Install from
// folder…" reviews the folder first (the permission dialog), then copies its manifest and scripts into the user's
// extensions folder and turns it on with what was allowed. Every extension runs in its own sandboxed helper process
// with no file, network or process access: what it can do in the project is exactly what it was allowed.
WabcutDialog {
    id: dialog
    objectName: "extensionsManager"

    title: qsTr("Extensions")
    /// The permission dialog (Review…, Enable…, Trust…, Install from folder…).
    readonly property alias permissionDialog: permissions
    /// The rows whose log is open (id -> true), and the one whose Uninstall is being confirmed: kept here because the
    /// rows are rebuilt when the list changes.
    property var openLogs: ({})
    property string confirmingUninstall: ""
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(170), Theme.space(170))

    /// Opens the permission dialog for an extension (Review…, Enable…, Trust…).
    function reviewExtension(id) {
        permissions.openFor(id)
    }
    /// "Install from folder…" with a chosen folder (the folder dialog's, or a test's).
    function installFrom(folder) {
        permissions.openForFolder(folder)
    }

    onOpened: Extensions.rescan()

    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        text: qsTr("Extensions add commands written in JavaScript. Each runs in its own sandboxed helper, with no file, network or process access, and can do only what you allow. Their edits are undo steps named after them.")
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        Text {
            objectName: "extensionsCount"
            Layout.fillWidth: true
            text: Extensions.count > 0 ? qsTr("%n extension(s)", "", Extensions.count) : ""
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.Medium
        }
        PillButton {
            objectName: "rescanExtensions"
            compact: true
            variant: "ghost"
            iconName: "rotate-cw"
            text: qsTr("Rescan")
            tooltip: qsTr("Read the extension folders again")
            onClicked: Extensions.rescan()
        }
        PillButton {
            objectName: "installExtension"
            compact: true
            iconName: "folder-plus"
            text: qsTr("Install from folder…")
            tooltip: qsTr("A folder with wabcut-extension.json and its scripts")
            onClicked: folderDialog.open()
        }
    }

    EmptyState {
        objectName: "extensionsEmpty"
        Layout.fillWidth: true
        visible: Extensions.count === 0
        illustration: "effects"
        text: qsTr("No extensions yet. Install one from a folder, or open a project that comes with its own.")
    }

    ScrollView {
        id: scroller
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(rows.implicitHeight, Theme.space(150))
        visible: Extensions.count > 0
        contentWidth: availableWidth
        clip: true
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: rows
            width: scroller.availableWidth
            spacing: Theme.space(2)
            Repeater {
                model: Extensions.extensions
                delegate: ExtensionRow {
                    Layout.fillWidth: true
                    showLog: dialog.openLogs[modelData.id] === true
                    confirmingUninstall: dialog.confirmingUninstall === modelData.id
                    onReviewRequested: extensionId => dialog.reviewExtension(extensionId)
                    onLogToggled: (extensionId, open) => {
                        const logs = Object.assign({}, dialog.openLogs)
                        logs[extensionId] = open
                        dialog.openLogs = logs
                    }
                    onUninstallAsked: (extensionId, asking) => dialog.confirmingUninstall = asking ? extensionId : ""
                }
            }
        }
    }

    Text {
        objectName: "extensionsMessage"
        Layout.fillWidth: true
        visible: text !== ""
        text: Extensions.message
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    Text {
        Layout.fillWidth: true
        text: qsTr("Installed in %1").arg(Extensions.userFolder)
        elide: Text.ElideMiddle
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Install an extension from a folder")
        onAccepted: dialog.installFrom(selectedFolder)
    }
    ExtensionPermissionDialog {
        id: permissions
    }

    footerData: [
        PillButton {
            objectName: "closeExtensionsManager"
            variant: "primary"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
