pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Dialogs

// Save for an Older Version… (0.7, card 059; File > Save for an Older Version…): writes a copy of the project the way an
// older Wabcut reads it. Pick the version; the list shows what that version cannot hold (it is left out of the copy and
// everything else stays), then Save a Copy… asks for the file. The project open here is not changed. A newer Wabcut can
// open older projects, so this is only for handing a project to someone who has not updated.
WabcutDialog {
    id: dialog
    objectName: "saveForOlderVersionDialog"

    /// A copy was written: the version's name and what was left out ([{ what, effect, count }]); Main.qml says so.
    signal saved(string name, int lostCount)

    /// [{ id, name, versions }] (ProjectFile.olderReleases()).
    property var releases: []
    /// What the chosen version cannot hold: [{ id, what, effect, count }].
    property var losses: []
    readonly property var chosen: releases[tabs.currentIndex] ?? null
    property string error: ""

    function openDialog() {
        releases = ProjectFile.olderReleases()
        tabs.currentIndex = 0
        error = ""
        refresh()
        open()
    }
    function refresh() {
        losses = chosen ? ProjectFile.olderReleaseLosses(chosen.id) : []
    }
    function chooseFile() {
        if (!chosen)
            return
        const name = ProjectFile.name !== "" ? ProjectFile.name : qsTr("Untitled project")
        fileDialog.selectedFile = ProjectFile.folder.toString() + "/"
                                  + encodeURIComponent(qsTr("%1 for Wabcut %2").arg(name).arg(chosen.id)) + ".wabcut"
        fileDialog.open()
    }

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }

    title: qsTr("Save for an Older Version")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("Writes a copy that an older Wabcut opens. This project stays as it is. What the older version has no place for is left out of the copy, listed below.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    GroupLabel {
        text: qsTr("For Wabcut")
    }
    PillTabs {
        id: tabs
        objectName: "olderVersionTabs"
        Layout.fillWidth: true
        Accessible.name: qsTr("The Wabcut version the copy is for")
        model: dialog.releases.map(r => r.name)
        onCurrentIndexChanged: dialog.refresh()
    }
    GroupLabel {
        text: dialog.losses.length > 0 ? qsTr("Left out of the copy") : qsTr("Left out")
    }
    Text {
        objectName: "olderVersionNothingLost"
        Layout.fillWidth: true
        visible: dialog.losses.length === 0
        wrapMode: Text.Wrap
        text: qsTr("Nothing: %1 opens this project as it is.").arg(dialog.chosen ? dialog.chosen.name : "")
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    ColumnLayout {
        objectName: "olderVersionLosses"
        Layout.fillWidth: true
        visible: dialog.losses.length > 0
        spacing: Theme.space(1.5)
        Repeater {
            model: dialog.losses
            delegate: Text {
                id: row
                required property var modelData
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                // "Auditions (2): each keeps its active take; the other takes are left out"
                text: qsTr("%1 (%2): %3").arg(row.modelData.what).arg(row.modelData.count).arg(row.modelData.effect)
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                Accessible.name: text
            }
        }
    }
    Text {
        id: errorText
        objectName: "olderVersionError"
        Layout.fillWidth: true
        visible: dialog.error !== ""
        wrapMode: Text.Wrap
        text: dialog.error
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    FileDialog {
        id: fileDialog
        objectName: "olderVersionFileDialog"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "wabcut"
        nameFilters: [qsTr("Wabcut projects (*.wabcut)")]
        currentFolder: ProjectFile.folder
        onAccepted: {
            const result = ProjectFile.saveForOlderVersion(selectedFile, dialog.chosen.id)
            if (result.saved) {
                dialog.close()
                dialog.saved(result.name, result.lost.length)
            } else {
                dialog.error = ProjectFile.lastError
            }
        }
    }

    footerData: [
        PillButton {
            objectName: "olderVersionCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "olderVersionSave"
            variant: "primary"
            iconName: "save"
            text: qsTr("Save a Copy…")
            enabled: dialog.chosen !== null
            onClicked: dialog.chooseFile()
        }
    ]
}
