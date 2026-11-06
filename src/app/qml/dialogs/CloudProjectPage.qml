pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// The Cloud project page of the Cloud dialog (0.7). A project that is not a cloud project can be uploaded to one of the "cloud
// projects" locations (the page says what goes up and how big it is; nothing goes before the Upload button), and a cloud project
// of one of them can be opened on this computer. A cloud project shows its state - the cloud copy's revision, the last sync, who
// is editing - and Sync now brings the two together: it sends what you changed, brings in what others changed, merges both, and asks
// before it keeps one side of a conflict. The revisions of the cloud copy stay listed; any can be opened as a copy.
ColumnLayout {
    id: page

    signal needStorage()

    property string locationId: ""
    property string openLocationId: ""
    property var preview: ({})
    property bool confirmed: false
    readonly property var projectLocations: Cloud.locations.filter(l => l.purpose === "projects")
    readonly property bool linked: Cloud.project.linked === true

    spacing: Theme.space(3)

    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }
    component Heading: Text {
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        font.weight: Font.DemiBold
        textFormat: Text.PlainText
    }
    component Card: Rectangle {
        Layout.fillWidth: true
        radius: Theme.radiusField
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.border
    }

    function refreshPreview() {
        page.confirmed = false
        page.preview = page.locationId !== "" ? Cloud.uploadPreview(page.locationId) : ({})
    }
    function syncLocations() {
        if (page.locationId === "" || !page.projectLocations.some(l => l.id === page.locationId))
            page.locationId = page.projectLocations.length > 0 ? page.projectLocations[0].id : ""
        if (page.openLocationId === "" || !page.projectLocations.some(l => l.id === page.openLocationId))
            page.openLocationId = page.projectLocations.length > 0 ? page.projectLocations[0].id : ""
        page.refreshPreview()
    }
    onProjectLocationsChanged: page.syncLocations()
    Component.onCompleted: page.syncLocations()

    // No storage yet.
    EmptyState {
        objectName: "cloudNoStorage"
        Layout.fillWidth: true
        visible: page.projectLocations.length === 0
        illustration: "library"
        text: qsTr("Add a place to keep cloud projects first: a folder your sync service mirrors, a WebDAV server or an S3 bucket.")
        actionText: qsTr("Add storage")
        actionIcon: "folder-plus"
        onActionTriggered: page.needStorage()
    }

    // ---- not a cloud project yet ----
    ColumnLayout {
        Layout.fillWidth: true
        visible: !page.linked && page.projectLocations.length > 0
        spacing: Theme.space(3)

        Heading { text: qsTr("Keep this project in the cloud") }
        Note {
            text: qsTr("The project file goes to your storage and stays in step with the copy on each computer you open it on. The media files are not uploaded: each computer finds them in its own media folder.")
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Repeater {
                model: page.projectLocations
                delegate: PillButton {
                    id: choice
                    required property var modelData
                    objectName: "cloudUploadLocation_" + choice.modelData.name
                    compact: true
                    text: choice.modelData.name
                    variant: page.locationId === choice.modelData.id ? "primary" : "secondary"
                    onClicked: {
                        page.locationId = choice.modelData.id
                        page.refreshPreview()
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(3)
            Label_ { text: qsTr("Name") }
            FieldInput {
                id: nameField
                objectName: "cloudProjectName"
                Layout.fillWidth: true
                text: Cloud.projectName
                maximumLength: 120
                Accessible.name: qsTr("Name in the cloud")
            }
        }
        // What goes up, and the size, before anything does.
        Card {
            objectName: "cloudUploadPreview"
            implicitHeight: previewColumn.implicitHeight + Theme.space(4)
            ColumnLayout {
                id: previewColumn
                anchors.fill: parent
                anchors.margins: Theme.space(3)
                spacing: Theme.space(2)
                Note {
                    visible: page.preview.error !== undefined && page.preview.error !== ""
                    text: page.preview.error ?? ""
                    color: Theme.warning
                }
                Text {
                    visible: page.preview.ok === true
                    Layout.fillWidth: true
                    text: qsTr("%1 will be uploaded to %2.").arg(page.preview.sizeText ?? "").arg(page.preview.destination ?? "")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                Repeater {
                    model: page.preview.ok === true ? (page.preview.notes ?? []) : []
                    delegate: Note {
                        required property string modelData
                        text: modelData
                    }
                }
                Toggle {
                    id: confirmToggle
                    objectName: "cloudUploadConfirm"
                    visible: page.preview.ok === true
                    checked: page.confirmed
                    text: qsTr("I understand that the project file is copied to this storage")
                    onToggled: page.confirmed = confirmToggle.checked
                }
            }
        }
        PillButton {
            objectName: "cloudUpload"
            variant: "primary"
            text: qsTr("Upload as a cloud project")
            iconName: "upload"
            enabled: page.preview.ok === true && page.confirmed && !Cloud.busy && page.locationId !== ""
            disabledReason: page.preview.ok !== true ? (page.preview.error ?? "") : !page.confirmed ? qsTr("Tick the box to confirm.") : ""
            onClicked: Cloud.createCloudProject(page.locationId, nameField.text)
        }

        // Open one from the storage.
        Heading { text: qsTr("Open a cloud project on this computer"); Layout.topMargin: Theme.space(2) }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Repeater {
                model: page.projectLocations
                delegate: PillButton {
                    id: openChoice
                    required property var modelData
                    objectName: "cloudOpenLocation_" + openChoice.modelData.name
                    compact: true
                    text: openChoice.modelData.name
                    variant: page.openLocationId === openChoice.modelData.id ? "primary" : "secondary"
                    onClicked: page.openLocationId = openChoice.modelData.id
                }
            }
            PillButton {
                objectName: "cloudLook"
                compact: true
                text: qsTr("Look for projects")
                iconName: "search"
                enabled: !Cloud.busy && page.openLocationId !== ""
                onClicked: Cloud.refreshRemoteProjects(page.openLocationId)
            }
        }
        Repeater {
            model: Cloud.remoteProjects
            delegate: Card {
                id: remote
                required property var modelData
                objectName: "cloudRemote_" + remote.modelData.projectId
                implicitHeight: remoteRow.implicitHeight + Theme.space(4)
                RowLayout {
                    id: remoteRow
                    anchors.fill: parent
                    anchors.margins: Theme.space(3)
                    spacing: Theme.space(3)
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(0.5)
                        Text {
                            Layout.fillWidth: true
                            text: remote.modelData.name
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Revision %1 · %2 · %3").arg(remote.modelData.revision).arg(remote.modelData.savedBy).arg(remote.modelData.savedText)
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                    PillButton {
                        objectName: "cloudOpenRemote"
                        compact: true
                        text: qsTr("Open…")
                        iconName: "download"
                        enabled: !Cloud.busy
                        onClicked: {
                            openFolder.projectId = remote.modelData.projectId
                            openFolder.open()
                        }
                    }
                }
            }
        }
    }

    // ---- a cloud project ----
    ColumnLayout {
        Layout.fillWidth: true
        visible: page.linked
        spacing: Theme.space(3)

        Card {
            objectName: "cloudStatus"
            implicitHeight: statusColumn.implicitHeight + Theme.space(6)
            ColumnLayout {
                id: statusColumn
                anchors.fill: parent
                anchors.margins: Theme.space(3)
                spacing: Theme.space(2)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Text {
                        Layout.fillWidth: true
                        text: Cloud.project.name ?? ""
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textLg
                        font.weight: Font.DemiBold
                    }
                    StatusPill {
                        objectName: "cloudLockPill"
                        label: Cloud.project.lock === "mine" ? qsTr("You are editing")
                               : Cloud.project.lock === "other" ? qsTr("Read-only")
                               : Cloud.project.lock === "unknown" ? qsTr("Lock unknown") : qsTr("Not locked")
                        dotColor: Cloud.project.lock === "mine" ? Theme.success : Cloud.project.lock === "other" ? Theme.warning : Theme.textTertiary
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("On %1 · revision %2 · last synced %3").arg(Cloud.project.locationName ?? "").arg(Cloud.project.revision ?? 0).arg(Cloud.project.syncedText ?? "")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Note {
                    objectName: "cloudLockText"
                    visible: (Cloud.project.lockText ?? "") !== ""
                    text: Cloud.project.lock === "other" ? qsTr("Being edited by %1").arg(Cloud.project.lockText ?? "") : (Cloud.project.lockText ?? "")
                }
                Note {
                    objectName: "cloudHint"
                    visible: (Cloud.project.hint ?? "") !== ""
                    text: Cloud.project.hint ?? ""
                    color: Theme.warning
                }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            PillButton {
                objectName: "cloudSync"
                variant: "primary"
                text: qsTr("Sync now")
                iconName: "rotate-cw"
                enabled: Cloud.project.canSync === true && !Cloud.busy
                onClicked: Cloud.syncNow(false)
            }
            PillButton {
                objectName: "cloudSendAnyway"
                visible: Cloud.project.revertGuard === true
                text: qsTr("Send my version anyway")
                enabled: !Cloud.busy
                onClicked: Cloud.syncNow(true)
            }
            PillButton {
                objectName: "cloudFindMedia"
                text: qsTr("Find media")
                iconName: "folder-search"
                enabled: !Cloud.busy
                onClicked: Cloud.relinkMedia()
            }
            PillButton {
                objectName: "cloudTakeLock"
                visible: Cloud.project.lock !== "mine"
                text: qsTr("Try the editing lock again")
                enabled: !Cloud.busy
                onClicked: Cloud.takeLock()
            }
            PillButton {
                objectName: "cloudUnlink"
                variant: "ghost"
                text: qsTr("Stop using the cloud")
                enabled: !Cloud.busy
                onClicked: Cloud.unlinkProject()
            }
        }
        Toggle {
            objectName: "cloudAutoSync"
            checked: Cloud.project.autoSync === true
            text: qsTr("Sync automatically after every Save")
            onToggled: Cloud.setAutoSync(checked)
        }

        // Conflicts waiting for a choice.
        Card {
            objectName: "cloudConflicts"
            visible: Cloud.conflicts.length > 0
            implicitHeight: conflictColumn.implicitHeight + Theme.space(6)
            border.color: Theme.warning
            ColumnLayout {
                id: conflictColumn
                anchors.fill: parent
                anchors.margins: Theme.space(3)
                spacing: Theme.space(2)
                Heading { text: qsTr("Choose which version to keep") }
                Repeater {
                    model: Cloud.conflicts
                    delegate: RowLayout {
                        id: conflictRow
                        required property var modelData
                        property string pick: "mine"
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        Text {
                            Layout.fillWidth: true
                            text: conflictRow.modelData.what
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        PillTabs {
                            objectName: "cloudConflictPick"
                            compact: true
                            Accessible.name: qsTr("Version to keep")
                            model: [qsTr("Mine"), qsTr("Cloud's")]
                            onActivated: index => {
                                conflictRow.pick = index === 0 ? "mine" : "theirs"
                                page.picks[conflictRow.modelData.id] = conflictRow.pick
                            }
                        }
                    }
                }
                Flow {
                    spacing: Theme.space(2)
                    PillButton {
                        objectName: "cloudApplyChoices"
                        variant: "primary"
                        compact: true
                        text: qsTr("Apply these choices and sync")
                        enabled: !Cloud.busy
                        onClicked: Cloud.resolveConflicts("mine", page.picks)
                    }
                    PillButton {
                        objectName: "cloudKeepAllMine"
                        compact: true
                        text: qsTr("Keep all mine")
                        enabled: !Cloud.busy
                        onClicked: Cloud.resolveConflicts("mine")
                    }
                    PillButton {
                        objectName: "cloudTakeAllTheirs"
                        compact: true
                        text: qsTr("Take all the cloud's")
                        enabled: !Cloud.busy
                        onClicked: Cloud.resolveConflicts("theirs")
                    }
                }
            }
        }

        // The cloud copy's history.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Heading { text: qsTr("Revisions in the cloud"); Layout.fillWidth: true }
            PillButton {
                objectName: "cloudHistoryRefresh"
                compact: true
                variant: "ghost"
                text: qsTr("Refresh")
                iconName: "rotate-cw"
                enabled: !Cloud.busy
                onClicked: Cloud.refreshHistory()
            }
        }
        Note {
            visible: Cloud.revisions.length === 0
            text: qsTr("Press Refresh to read the list from the storage.")
        }
        Repeater {
            model: Cloud.revisions
            delegate: RowLayout {
                id: revisionRow
                required property var modelData
                objectName: "cloudRevision_" + revisionRow.modelData.revision
                Layout.fillWidth: true
                spacing: Theme.space(3)
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Revision %1 · %2 · %3 · %4").arg(revisionRow.modelData.revision).arg(revisionRow.modelData.savedBy)
                              .arg(revisionRow.modelData.savedText).arg(revisionRow.modelData.sizeText)
                          + (revisionRow.modelData.current ? qsTr(" · current") : "") + (revisionRow.modelData.mine ? qsTr(" · yours") : "")
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                PillButton {
                    objectName: "cloudRevisionOpen"
                    compact: true
                    variant: "ghost"
                    text: qsTr("Open as a copy…")
                    enabled: !Cloud.busy
                    onClicked: {
                        copyFolder.revision = revisionRow.modelData.revision
                        copyFolder.open()
                    }
                }
            }
        }
        Note {
            objectName: "cloudUnmerged"
            visible: Cloud.unmerged.length > 0
            color: Theme.warning
            text: qsTr("%n revision(s) in the cloud are not part of the current line (saved by someone whose save lost, or kept by your sync service when two computers wrote at once): %1. Open one as a copy to look at it.", "", Cloud.unmerged.length)
                  .arg(Cloud.unmerged.map(u => qsTr("revision %1 by %2").arg(u.revision).arg(u.savedBy)).join("; "))
        }
    }

    /// The choices made for conflicts: { conflictId: "mine" | "theirs" }.
    property var picks: ({})

    component Label_: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        textFormat: Text.PlainText
    }

    FolderDialog {
        id: openFolder
        property string projectId: ""
        title: qsTr("Choose a folder for the project")
        onAccepted: Cloud.openCloudProject(page.openLocationId, projectId, selectedFolder)
    }
    FolderDialog {
        id: copyFolder
        property int revision: 0
        title: qsTr("Choose a folder for the copy")
        onAccepted: Cloud.openRevisionAsCopy(revision, selectedFolder)
    }
}
