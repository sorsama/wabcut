pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Offline media and LUT files: search folders (file name + fingerprint, ProjectFile.relinkAll) or locate each file by
// hand (ProjectFile.relink; a different file is refused unless the user confirms it).
// P1 round (0.6):
//   * #43: openFor(keys) lists only those items (Link Media… on Library items and timeline clips, a double-click on an
//     offline Library item); "Show all offline media" lists every one again.
//   * #42: Locate… relinks the rest too (ProjectFile.relinkLocated: the others beside it or under the same moved
//     folder, "Relinked 1 file and 37 others."), and recordings remuxed into another container (an OBS .mkv saved again
//     as .mp4) found by a search or beside a located file are offered as "different container (.mkv → .mp4)" with one
//     Relink these (one undo step; their media info is read again).
WabcutDialog {
    id: dialog
    objectName: "relinkDialog"

    property string status: ""
    property string pendingMedia: ""
    property url pendingFile
    /// The offline items listed ([]: every one).
    property var only: []
    /// Recordings in another container: [{ id, name, path, url, from, to }] (ProjectFile.sameRecordings).
    property var sameRecordings: []
    readonly property var shown: {
        const all = ProjectFile.offlineMedia
        const keys = dialog.only
        return keys.length > 0 ? all.filter(m => keys.indexOf(m.id) >= 0) : all
    }

    /// Opens the dialog for the offline items `keys` only.
    function openFor(keys) {
        dialog.only = keys
        dialog.open()
    }
    /// Offers what `folders` hold of the offline items in another container.
    function findSameRecordings(folders) {
        dialog.sameRecordings = ProjectFile.offlineCount > 0 ? ProjectFile.sameRecordings(folders) : []
    }
    // Fix round 1 (ARGON-23): Search… and Locate… open where the last relink found media (remembered, Premiere does
    // too), else in the project's folder.
    readonly property url startFolder: remembered.lastFolder !== "" ? Qt.url(remembered.lastFolder) : ProjectFile.folder
    function rememberFolder(folder) {
        const text = folder.toString()
        if (text !== "") {
            remembered.lastFolder = text
            remembered.sync()
        }
    }
    Settings {
        id: remembered
        category: "relink"
        property string lastFolder: ""
    }

    function folderOf(file) {
        const text = file.toString()
        return Qt.url(text.substring(0, text.lastIndexOf("/")))
    }
    function locate(file, force) {
        const others = ProjectFile.relinkLocated(dialog.pendingMedia, file, force)
        if (others < 0) {
            // A different file (fingerprint mismatch) or unreadable: say why; the user may force it.
            dialog.pendingFile = file
            dialog.status = ProjectFile.lastError
            return
        }
        //: After Locate… found one file and, with it, the other offline files beside it
        dialog.status = others === 0 ? qsTr("Relinked.")
                        : others == 1 ? qsTr("Relinked 1 file and 1 other.") //: one more offline file beside it
                                      : qsTr("Relinked 1 file and %1 others.").arg(others)
        dialog.pendingMedia = ""
        dialog.pendingFile = ""
        dialog.findSameRecordings([dialog.folderOf(file)])
    }

    title: qsTr("Relink media")

    Text {
        objectName: "relinkSummary"
        Layout.fillWidth: true
        text: dialog.only.length > 0 && dialog.shown.length > 0
              ? qsTr("Link %1: locate the file, or search a folder to find them by name.")
                    .arg(dialog.shown.length === 1 ? qsTr("1 offline item") : qsTr("%1 offline items").arg(dialog.shown.length))
              : dialog.only.length > 0
              ? qsTr("These items are linked again.")
              : ProjectFile.offlineCount > 0
              ? qsTr("%1 cannot be found. Search a folder to find them by name, or locate them one by one.")
                    .arg(ProjectFile.offlineCount === 1 ? qsTr("1 file") : qsTr("%1 files").arg(ProjectFile.offlineCount))
              : qsTr("All media and LUTs are online.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    ListView {
        objectName: "relinkList"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(70))
        visible: count > 0
        clip: true
        spacing: Theme.space(1)
        model: dialog.shown
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
                objectName: "relinkLocate_" + row.modelData.id
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                text: qsTr("Locate…")
                Accessible.name: qsTr("Locate %1").arg(row.modelData.name)
                onClicked: {
                    dialog.pendingMedia = row.modelData.id
                    locateDialog.open()
                }
            }
        }
    }

    // #43: back to every offline file.
    PillButton {
        objectName: "relinkShowAll"
        visible: dialog.only.length > 0 && ProjectFile.offlineCount > dialog.shown.length
        variant: "ghost"
        compact: true
        text: qsTr("Show all offline media (%1)").arg(ProjectFile.offlineCount)
        onClicked: dialog.only = []
    }

    // #42: the same recordings in another container, relinked together after one confirmation.
    Text {
        Layout.fillWidth: true
        visible: dialog.sameRecordings.length > 0
        text: qsTr("Found in a different container: the same recordings (length, picture and sound match) saved as another file type. Relink them to these files?")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    ListView {
        objectName: "relinkSameRecordings"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(40))
        visible: count > 0
        clip: true
        spacing: Theme.space(1)
        model: dialog.sameRecordings
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        delegate: Rectangle {
            id: offer
            required property var modelData
            width: ListView.view.width
            height: offerColumn.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: Theme.sunken
            Accessible.role: Accessible.ListItem
            Accessible.name: offerName.text

            Column {
                id: offerColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space(3)
                anchors.rightMargin: Theme.space(3)
                spacing: Theme.space(0.5)
                Text {
                    id: offerName
                    width: parent.width
                    //: A recording found as another file type, e.g. "Match 3: different container (.mkv → .mp4)"
                    text: qsTr("%1: different container (%2 → %3)").arg(offer.modelData.name)
                          .arg(offer.modelData.from).arg(offer.modelData.to)
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: offer.modelData.path
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                    elide: Text.ElideMiddle
                }
            }
        }
    }

    Text {
        objectName: "relinkStatus"
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
            objectName: "relinkUseAnyway"
            visible: dialog.pendingMedia !== "" && dialog.pendingFile.toString() !== ""
            text: qsTr("Use this file anyway")
            onClicked: dialog.locate(dialog.pendingFile, true)
        },
        PillButton {
            objectName: "relinkSameRecordingsButton"
            visible: dialog.sameRecordings.length > 0
            text: qsTr("Relink these (%1)").arg(dialog.sameRecordings.length)
            onClicked: {
                const relinked = ProjectFile.relinkSameRecordings(dialog.sameRecordings)
                dialog.status = relinked == 1 ? qsTr("Relinked 1 recording from another container.") //: files with another extension (OBS .mkv to .mp4)
                               : relinked > 1 ? qsTr("Relinked %1 recordings from another container.").arg(relinked)
                                              : ProjectFile.lastError
                if (relinked > 0)
                    dialog.sameRecordings = []
            }
        },
        PillButton {
            objectName: "relinkSearchFolder"
            text: qsTr("Search folder…")
            iconName: "search"
            onClicked: folderDialog.open()
        },
        PillButton {
            objectName: "relinkDone"
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
    onClosed: {
        only = []
        sameRecordings = []
    }

    FolderDialog {
        id: folderDialog
        currentFolder: dialog.startFolder
        onAccepted: {
            dialog.rememberFolder(selectedFolder)
            const found = ProjectFile.relinkAll([selectedFolder])
            dialog.findSameRecordings([selectedFolder])
            dialog.status = (found === 1 ? qsTr("1 file relinked.") : qsTr("%1 files relinked.").arg(found))
                            + (dialog.sameRecordings.length > 0
                               ? " " + qsTr("%n more found in a different container: check them below.", "",
                                            dialog.sameRecordings.length) : "")
        }
    }

    FileDialog {
        id: locateDialog
        fileMode: FileDialog.OpenFile
        currentFolder: dialog.startFolder
        onAccepted: {
            dialog.rememberFolder(dialog.folderOf(selectedFile))
            dialog.locate(selectedFile, false)
        }
        onRejected: dialog.pendingMedia = ""
    }
}
