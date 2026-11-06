pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// The Storage page of the Cloud dialog (0.7): the places the user keeps cloud projects and review copies. A location is a folder a
// sync service mirrors (OneDrive, Dropbox, Google Drive, a share), a WebDAV collection, or an S3-compatible bucket (AWS, Cloudflare R2,
// Backblaze B2, MinIO, Wasabi ...). It is for cloud projects OR for review links, never both: a review folder is shared with strangers.
ColumnLayout {
    id: page

    signal locationSaved(string id)

    /// The id being edited ("" = a new one), and whether the form is open.
    property string editingId: ""
    property bool editing: false
    property string error: ""
    readonly property var kinds: ["folder", "webdav", "s3"]
    readonly property var purposes: ["projects", "reviews"]

    spacing: Theme.space(3)

    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }
    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        textFormat: Text.PlainText
    }

    function openForm(row) {
        page.error = ""
        page.editingId = row ? row.id : ""
        nameField.text = row ? row.name : ""
        kindTabs.currentIndex = row ? Math.max(0, page.kinds.indexOf(row.kind)) : 0
        purposeTabs.currentIndex = row ? Math.max(0, page.purposes.indexOf(row.purpose)) : 0
        folderField.text = row ? row.folder : ""
        urlField.text = row ? row.url : ""
        userField.text = row ? row.user : ""
        endpointField.text = row ? row.endpoint : ""
        regionField.text = row ? row.region : "us-east-1"
        bucketField.text = row ? row.bucket : ""
        prefixField.text = row ? row.prefix : ""
        secretA.text = ""
        secretB.text = ""
        pathStyleToggle.checked = row ? row.pathStyle : true
        insecureToggle.checked = row ? row.allowInsecure : false
        page.editing = true
    }
    function save() {
        const kind = page.kinds[kindTabs.currentIndex]
        const spec = {
            id: page.editingId,
            name: nameField.text,
            kind: kind,
            purpose: page.purposes[purposeTabs.currentIndex],
            folder: folderField.text,
            url: urlField.text,
            user: userField.text,
            endpoint: endpointField.text,
            region: regionField.text,
            bucket: bucketField.text,
            prefix: prefixField.text,
            pathStyle: pathStyleToggle.checked,
            allowInsecure: insecureToggle.checked,
            secretA: secretA.text,
            secretB: secretB.text
        }
        const result = Cloud.saveLocation(spec)
        if (!result.ok) {
            page.error = result.error
            return
        }
        page.editing = false
        secretA.text = ""
        secretB.text = ""
        page.locationSaved(result.id)
    }

    Note {
        text: qsTr("Cloud projects and review links live on storage you own: a folder your sync service keeps (OneDrive, Dropbox, Google Drive, a network share), a WebDAV server such as Nextcloud, or an S3-compatible bucket. Wabcut has no server of its own and holds no copy. Passwords and keys go to your system's key store.")
    }

    Repeater {
        model: Cloud.locations
        delegate: Rectangle {
            id: card
            required property var modelData
            objectName: "cloudLocation_" + card.modelData.name
            Layout.fillWidth: true
            implicitHeight: cardColumn.implicitHeight + Theme.space(4)
            radius: Theme.radiusField
            color: Theme.raised
            border.width: Theme.hairline
            border.color: Theme.border
            Accessible.role: Accessible.ListItem
            Accessible.name: card.modelData.name

            ColumnLayout {
                id: cardColumn
                anchors.fill: parent
                anchors.margins: Theme.space(3)
                spacing: Theme.space(2)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Text {
                        Layout.fillWidth: true
                        text: card.modelData.name
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.DemiBold
                    }
                    StatusPill {
                        label: card.modelData.purposeName
                        dotColor: card.modelData.purpose === "reviews" ? Theme.warning : Theme.success
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: card.modelData.kindName + " · " + card.modelData.summary
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Text {
                    objectName: "cloudLocationCheck"
                    Layout.fillWidth: true
                    visible: card.modelData.checkText !== ""
                    text: card.modelData.checkText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: card.modelData.check === "problem" ? Theme.warning : Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Text {
                    Layout.fillWidth: true
                    visible: card.modelData.purpose === "projects" && card.modelData.mediaFolders.length > 0
                    text: qsTr("Media folders on this computer: %1").arg(card.modelData.mediaFolders.join("; "))
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    PillButton {
                        objectName: "cloudLocationTest"
                        compact: true
                        text: qsTr("Test")
                        iconName: "check"
                        enabled: !Cloud.busy
                        onClicked: Cloud.testLocation(card.modelData.id)
                    }
                    PillButton {
                        objectName: "cloudLocationMedia"
                        compact: true
                        visible: card.modelData.purpose === "projects"
                        text: qsTr("Media folder…")
                        iconName: "folder-open"
                        onClicked: {
                            mediaDialog.locationId = card.modelData.id
                            mediaDialog.known = card.modelData.mediaFolders
                            mediaDialog.open()
                        }
                    }
                    PillButton {
                        objectName: "cloudLocationEdit"
                        compact: true
                        text: qsTr("Edit")
                        onClicked: page.openForm(card.modelData)
                    }
                    PillButton {
                        objectName: "cloudLocationRemove"
                        compact: true
                        variant: "ghost"
                        text: qsTr("Remove")
                        iconName: "trash-2"
                        onClicked: Cloud.removeLocation(card.modelData.id)
                    }
                }
            }
        }
    }

    PillButton {
        objectName: "cloudLocationAdd"
        visible: !page.editing
        text: qsTr("Add a location")
        iconName: "folder-plus"
        variant: Cloud.locations.length === 0 ? "primary" : "secondary"
        onClicked: page.openForm(null)
    }

    // The form.
    Rectangle {
        objectName: "cloudLocationForm"
        Layout.fillWidth: true
        visible: page.editing
        implicitHeight: form.implicitHeight + Theme.space(6)
        radius: Theme.radiusField
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.borderStrong

        GridLayout {
            id: form
            anchors.fill: parent
            anchors.margins: Theme.space(3)
            columns: 2
            columnSpacing: Theme.space(4)
            rowSpacing: Theme.space(2)

            Label { text: qsTr("Name") }
            FieldInput {
                id: nameField
                objectName: "cloudNameField"
                Layout.fillWidth: true
                maximumLength: 80
                placeholderText: qsTr("For example: Dropbox projects")
                Accessible.name: qsTr("Name of the location")
            }

            Label { text: qsTr("Kind") }
            PillTabs {
                id: kindTabs
                objectName: "cloudKindTabs"
                Layout.fillWidth: true
                compact: true
                Accessible.name: qsTr("Kind of storage")
                model: [qsTr("Synced folder"), qsTr("WebDAV"), qsTr("S3-compatible")]
            }

            Label { text: qsTr("For") }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                PillTabs {
                    id: purposeTabs
                    objectName: "cloudPurposeTabs"
                    Layout.fillWidth: true
                    compact: true
                    Accessible.name: qsTr("What the location is for")
                    model: [qsTr("Cloud projects"), qsTr("Review links")]
                }
                Note {
                    text: purposeTabs.currentIndex === 0
                          ? qsTr("Your project files and their revisions. Keep this private.")
                          : qsTr("Review copies you share with other people. Use a folder or bucket of its own: anyone you share it with can see everything in it.")
                }
            }

            // folder
            Label { text: qsTr("Folder"); visible: kindTabs.currentIndex === 0 }
            RowLayout {
                visible: kindTabs.currentIndex === 0
                Layout.fillWidth: true
                spacing: Theme.space(2)
                FieldInput {
                    id: folderField
                    objectName: "cloudFolderField"
                    Layout.fillWidth: true
                    placeholderText: qsTr("A folder inside your OneDrive, Dropbox or Google Drive")
                    Accessible.name: qsTr("Folder")
                }
                PillButton {
                    compact: true
                    text: qsTr("Choose…")
                    onClicked: folderDialog.open()
                }
            }

            // WebDAV
            Label { text: qsTr("Address"); visible: kindTabs.currentIndex === 1 }
            FieldInput {
                id: urlField
                objectName: "cloudUrlField"
                visible: kindTabs.currentIndex === 1
                Layout.fillWidth: true
                placeholderText: qsTr("https://cloud.example.com/remote.php/dav/files/you/Wabcut")
                Accessible.name: qsTr("WebDAV address")
            }
            Label { text: qsTr("User"); visible: kindTabs.currentIndex === 1 }
            FieldInput {
                id: userField
                objectName: "cloudUserField"
                visible: kindTabs.currentIndex === 1
                Layout.fillWidth: true
                Accessible.name: qsTr("User name")
            }

            // S3
            Label { text: qsTr("Endpoint"); visible: kindTabs.currentIndex === 2 }
            FieldInput {
                id: endpointField
                objectName: "cloudEndpointField"
                visible: kindTabs.currentIndex === 2
                Layout.fillWidth: true
                placeholderText: qsTr("https://s3.eu-west-1.amazonaws.com")
                Accessible.name: qsTr("S3 endpoint")
            }
            Label { text: qsTr("Region"); visible: kindTabs.currentIndex === 2 }
            FieldInput {
                id: regionField
                objectName: "cloudRegionField"
                visible: kindTabs.currentIndex === 2
                Layout.fillWidth: true
                Accessible.name: qsTr("Region")
            }
            Label { text: qsTr("Bucket"); visible: kindTabs.currentIndex === 2 }
            FieldInput {
                id: bucketField
                objectName: "cloudBucketField"
                visible: kindTabs.currentIndex === 2
                Layout.fillWidth: true
                Accessible.name: qsTr("Bucket")
            }
            Label { text: qsTr("Folder in bucket"); visible: kindTabs.currentIndex === 2 }
            FieldInput {
                id: prefixField
                objectName: "cloudPrefixField"
                visible: kindTabs.currentIndex === 2
                Layout.fillWidth: true
                placeholderText: qsTr("Optional, for example: wabcut")
                Accessible.name: qsTr("Folder in the bucket")
            }

            // secrets
            Label { text: kindTabs.currentIndex === 1 ? qsTr("Password") : qsTr("Access key"); visible: kindTabs.currentIndex !== 0 }
            FieldInput {
                id: secretA
                objectName: "cloudSecretAField"
                visible: kindTabs.currentIndex !== 0
                Layout.fillWidth: true
                echoMode: kindTabs.currentIndex === 1 ? TextInput.Password : TextInput.Normal
                placeholderText: page.editingId !== "" ? qsTr("Leave empty to keep the saved one") : ""
                Accessible.name: kindTabs.currentIndex === 1 ? qsTr("Password") : qsTr("Access key")
                Accessible.description: kindTabs.currentIndex === 1 ? qsTr("Password") : qsTr("Access key")
            }
            Label { text: qsTr("Secret key"); visible: kindTabs.currentIndex === 2 }
            FieldInput {
                id: secretB
                objectName: "cloudSecretBField"
                visible: kindTabs.currentIndex === 2
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: page.editingId !== "" ? qsTr("Leave empty to keep the saved one") : ""
                Accessible.name: qsTr("Secret key")
                Accessible.description: qsTr("Secret key")
            }

            Item { visible: kindTabs.currentIndex === 2; implicitWidth: 1; implicitHeight: 1 }
            Toggle {
                id: pathStyleToggle
                objectName: "cloudPathStyleToggle"
                visible: kindTabs.currentIndex === 2
                checked: true
                text: qsTr("Address the bucket by path (turn off for bucket.endpoint addresses)")
            }
            Item { visible: kindTabs.currentIndex !== 0; implicitWidth: 1; implicitHeight: 1 }
            Toggle {
                id: insecureToggle
                objectName: "cloudInsecureToggle"
                visible: kindTabs.currentIndex !== 0
                text: qsTr("Allow an unencrypted connection (http). Only on a network you trust.")
            }

            Item { implicitWidth: 1; implicitHeight: 1 }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Note {
                    objectName: "cloudFormError"
                    visible: page.error !== ""
                    text: page.error
                    color: Theme.warning
                }
                RowLayout {
                    spacing: Theme.space(2)
                    PillButton {
                        objectName: "cloudLocationSave"
                        variant: "primary"
                        text: qsTr("Save")
                        onClicked: page.save()
                    }
                    PillButton {
                        objectName: "cloudLocationCancel"
                        variant: "ghost"
                        text: qsTr("Cancel")
                        onClicked: {
                            page.editing = false
                            secretA.text = ""
                            secretB.text = ""
                        }
                    }
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose the folder")
        onAccepted: folderField.text = selectedFolder.toString().replace(/^file:\/\/\//, Qt.platform.os === "windows" ? "" : "/")
    }
    FolderDialog {
        id: mediaDialog
        property string locationId: ""
        property var known: []
        title: qsTr("Folder where this computer keeps the media")
        onAccepted: {
            const path = selectedFolder.toString().replace(/^file:\/\/\//, Qt.platform.os === "windows" ? "" : "/")
            const next = known.slice()
            if (!next.includes(path))
                next.push(path)
            Cloud.setMediaFolders(locationId, next)
        }
    }
}
