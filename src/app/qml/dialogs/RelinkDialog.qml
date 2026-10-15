pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Offline media and LUT files: search folders (file name + fingerprint, ProjectFile.relinkAll) or locate each file by
// hand (ProjectFile.relink; a different file is refused unless the user confirms it).
WabcutDialog {
    id: dialog
    objectName: "relinkDialog"

    property string status: ""
    property string pendingMedia: ""
    property url pendingFile

    title: qsTr("Relink media")

    Text {
        Layout.fillWidth: true
        text: ProjectFile.offlineCount > 0
              ? qsTr("%n file(s) cannot be found. Search a folder to find them by name, or locate them one by one.",
                     "", ProjectFile.offlineCount)
              : qsTr("All media and LUTs are online.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    ListView {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(70))
        visible: count > 0
        clip: true
        spacing: Theme.space(1)
        model: ProjectFile.offlineMedia
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        delegate: Rectangle {
            id: row
            required property var modelData
            width: ListView.view.width
            height: Theme.controlHeight + Theme.space(4)
            radius: Theme.radiusField
            color: Theme.sunken

            Column {
                anchors.left: parent.left
                anchors.right: locate.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space(3)
                anchors.rightMargin: Theme.space(2)
                spacing: Theme.space(0.5)

                Text {
                    width: parent.width
                    text: row.modelData.kind === "lut" ? qsTr("%1 (LUT)").arg(row.modelData.name) : row.modelData.name
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: row.modelData.path
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                    elide: Text.ElideMiddle
                }
            }
            PillButton {
                id: locate
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                text: qsTr("Locate…")
                onClicked: {
                    dialog.pendingMedia = row.modelData.id
                    locateDialog.open()
                }
            }
        }
    }

    Text {
        Layout.fillWidth: true
        visible: dialog.status !== ""
        text: dialog.status
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            visible: dialog.pendingMedia !== "" && dialog.pendingFile.toString() !== ""
            text: qsTr("Use this file anyway")
            onClicked: {
                if (ProjectFile.relink(dialog.pendingMedia, dialog.pendingFile, true))
                    dialog.status = qsTr("Relinked.")
                dialog.pendingMedia = ""
                dialog.pendingFile = ""
            }
        },
        PillButton {
            text: qsTr("Search folder…")
            iconName: "search"
            onClicked: folderDialog.open()
        },
        PillButton {
            text: qsTr("Done")
            variant: "primary"
            onClicked: dialog.close()
        }
    ]

    onOpened: {
        status = ""
        pendingMedia = ""
        pendingFile = ""
    }

    FolderDialog {
        id: folderDialog
        currentFolder: ProjectFile.folder
        onAccepted: {
            const found = ProjectFile.relinkAll([selectedFolder])
            dialog.status = qsTr("%n file(s) relinked.", "", found)
        }
    }

    FileDialog {
        id: locateDialog
        fileMode: FileDialog.OpenFile
        currentFolder: ProjectFile.folder
        onAccepted: {
            if (ProjectFile.relink(dialog.pendingMedia, selectedFile, false)) {
                dialog.status = qsTr("Relinked.")
                dialog.pendingMedia = ""
            } else {
                // A different file (fingerprint mismatch) or unreadable: say why; the user may force it.
                dialog.pendingFile = selectedFile
                dialog.status = ProjectFile.lastError
            }
        }
        onRejected: dialog.pendingMedia = ""
    }
}
