pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// The Review links page of the Cloud dialog (0.7): send a draft to a client and get their comments back as review notes.
//   1. Make the review copy: a small H.264 file of the sequence, rendered on this computer. Its size is known before anything is sent.
//   2. Share it: it goes, with a web page that plays it and collects frame-accurate comments, to one of your "review links" locations.
//      The page says what is uploaded and how big it is, and nothing goes until the Publish button.
//   3. Read the comments back: from the shared folder, or from the file a reviewer sent. They become review notes at the right frames.
// Links can be revoked (the page and the video are deleted from the storage) or removed when they have expired.
ColumnLayout {
    id: page

    signal needStorage()

    property string locationId: ""
    property var estimate: ({})
    property bool confirmed: false
    property bool alsoMarkers: true
    readonly property var reviewLocations: Cloud.locations.filter(l => l.purpose === "reviews")
    readonly property var resolutions: [540, 720, 1080]
    readonly property var ranges: ["whole", "inOut", "workArea"]
    readonly property bool ready: Cloud.reviewCopy.state === "ready"
    readonly property var chosenLocation: page.reviewLocations.find(l => l.id === page.locationId) ?? ({})

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
    component Label_: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        textFormat: Text.PlainText
    }
    component Card: Rectangle {
        Layout.fillWidth: true
        radius: Theme.radiusField
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.border
    }

    function options() {
        return { resolution: page.resolutions[resolutionTabs.currentIndex], range: page.ranges[rangeTabs.currentIndex] }
    }
    function refreshEstimate() {
        page.estimate = Cloud.estimateReviewCopy(page.options())
    }
    function syncLocations() {
        if (page.locationId === "" || !page.reviewLocations.some(l => l.id === page.locationId))
            page.locationId = page.reviewLocations.length > 0 ? page.reviewLocations[0].id : ""
    }
    onReviewLocationsChanged: page.syncLocations()
    Component.onCompleted: {
        page.syncLocations()
        page.refreshEstimate()
    }
    Connections {
        target: Cloud
        function onProjectChanged() { page.refreshEstimate() }
    }

    // ---- 1. the review copy ----
    Heading { text: qsTr("1. Make the review copy") }
    Note {
        text: qsTr("A small video of the sequence for people to watch in a browser. It is made on this computer and kept until you publish or discard it.")
    }
    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(2)
        enabled: !Cloud.busy && !page.ready
        Label_ { text: qsTr("Size") }
        PillTabs {
            id: resolutionTabs
            objectName: "reviewResolutionTabs"
            Layout.fillWidth: true
            compact: true
            Accessible.name: qsTr("Picture size of the review copy")
            model: ["540p", "720p", "1080p"]
            currentIndex: 1
            onActivated: page.refreshEstimate()
        }
        Label_ { text: qsTr("Part") }
        PillTabs {
            id: rangeTabs
            objectName: "reviewRangeTabs"
            Layout.fillWidth: true
            compact: true
            Accessible.name: qsTr("Part of the sequence")
            model: [qsTr("Whole sequence"), qsTr("In to Out"), qsTr("Work area")]
            onActivated: page.refreshEstimate()
        }
    }
    Note {
        objectName: "reviewEstimate"
        visible: !page.ready
        color: page.estimate.ok === true ? Theme.textSecondary : Theme.warning
        text: page.estimate.ok === true
              ? qsTr("\"%1\", %2, %3: %4.").arg(page.estimate.sequenceName).arg(page.estimate.durationText).arg(page.estimate.resolution).arg(page.estimate.sizeText)
              : (page.estimate.error ?? "")
    }
    Card {
        objectName: "reviewCopyReady"
        visible: page.ready
        implicitHeight: readyColumn.implicitHeight + Theme.space(6)
        ColumnLayout {
            id: readyColumn
            anchors.fill: parent
            anchors.margins: Theme.space(3)
            spacing: Theme.space(2)
            Text {
                Layout.fillWidth: true
                text: qsTr("Ready: \"%1\", %2, %3, %4.").arg(Cloud.reviewCopy.sequenceName ?? "").arg(Cloud.reviewCopy.durationText ?? "")
                          .arg(Cloud.reviewCopy.resolution ?? "").arg(Cloud.reviewCopy.sizeText ?? "")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            PillButton {
                objectName: "reviewDiscard"
                compact: true
                variant: "ghost"
                text: qsTr("Discard it")
                iconName: "trash-2"
                enabled: !Cloud.busy
                onClicked: Cloud.discardReviewCopy()
            }
        }
    }
    PillButton {
        objectName: "reviewMake"
        visible: !page.ready
        variant: "primary"
        text: Cloud.reviewCopy.state === "rendering" ? qsTr("Making it…") : qsTr("Make the review copy")
        iconName: "film"
        enabled: page.estimate.ok === true && !Cloud.busy
        onClicked: Cloud.prepareReviewCopy(page.options())
    }

    // ---- 2. share it ----
    Heading { text: qsTr("2. Share it"); Layout.topMargin: Theme.space(2) }
    EmptyState {
        objectName: "reviewNoStorage"
        Layout.fillWidth: true
        visible: page.reviewLocations.length === 0
        illustration: "library"
        text: qsTr("Add a place for review links first. Use a folder or bucket that holds nothing but review copies: everyone you share it with can see all of it.")
        actionText: qsTr("Add storage")
        actionIcon: "folder-plus"
        onActionTriggered: page.needStorage()
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: page.reviewLocations.length > 0
        spacing: Theme.space(2)
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Repeater {
                model: page.reviewLocations
                delegate: PillButton {
                    id: choice
                    required property var modelData
                    objectName: "reviewLocation_" + choice.modelData.name
                    compact: true
                    text: choice.modelData.name
                    variant: page.locationId === choice.modelData.id ? "primary" : "secondary"
                    onClicked: {
                        page.locationId = choice.modelData.id
                        page.confirmed = false
                    }
                }
            }
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.space(4)
            rowSpacing: Theme.space(2)
            Label_ { text: qsTr("Title") }
            FieldInput {
                id: titleField
                objectName: "reviewTitle"
                Layout.fillWidth: true
                text: Cloud.reviewCopy.sequenceName ?? ""
                maximumLength: 120
                Accessible.name: qsTr("Title of the review page")
            }
            Label_ { text: qsTr("Expires") }
            RowLayout {
                spacing: Theme.space(2)
                NumberField {
                    id: expiryField
                    objectName: "reviewExpiry"
                    Accessible.name: qsTr("Days until the link expires")
                    value: 7
                    from: 0
                    to: 365
                    stepSize: 1
                    decimals: 0
                    suffix: qsTr("days")
                }
                Note {
                    text: expiryField.value === 0 ? qsTr("Never. The page keeps working until you revoke it.")
                          : page.chosenLocation.kind === "s3" ? qsTr("The link stops working by itself (S3 links last 7 days at most).")
                          : qsTr("The page refuses to play after that day, and the files can be removed from here.")
                }
            }
            Label_ { text: qsTr("Note") }
            FieldInput {
                id: noteField
                objectName: "reviewNote"
                Layout.fillWidth: true
                maximumLength: 400
                placeholderText: qsTr("A few words for the people who watch (optional)")
                Accessible.name: qsTr("Note to the reviewers")
            }
        }
        Toggle {
            id: removeToggle
            objectName: "reviewRemoveExpired"
            checked: true
            text: qsTr("Offer to remove the files from the storage once the link has expired")
        }
        Card {
            objectName: "reviewPublishBox"
            implicitHeight: publishColumn.implicitHeight + Theme.space(6)
            ColumnLayout {
                id: publishColumn
                anchors.fill: parent
                anchors.margins: Theme.space(3)
                spacing: Theme.space(2)
                Text {
                    objectName: "reviewPublishText"
                    Layout.fillWidth: true
                    text: page.ready
                          ? qsTr("%1 will be uploaded to \"%2\": the video and a web page. Anyone who gets the link can watch it and leave comments; the page sends nothing back by itself. Your storage provider bills for storage and traffic.")
                                .arg(Cloud.reviewCopy.sizeText ?? "").arg(page.chosenLocation.name ?? "")
                          : qsTr("Make the review copy first; its size is shown here before anything is uploaded.")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                Note {
                    visible: page.chosenLocation.kind === "folder"
                    text: qsTr("This is a folder your sync service uploads: share this review folder with your reviewers using the service's own sharing.")
                }
                Note {
                    objectName: "reviewBlocked"
                    visible: Cloud.blockedReason !== ""
                    color: Theme.warning
                    text: Cloud.blockedReason
                }
                Toggle {
                    id: confirmToggle
                    objectName: "reviewConfirm"
                    visible: page.ready
                    checked: page.confirmed
                    text: qsTr("I want to upload this")
                    onToggled: page.confirmed = confirmToggle.checked
                }
            }
        }
        PillButton {
            objectName: "reviewPublish"
            variant: "primary"
            text: qsTr("Upload and publish")
            iconName: "upload"
            enabled: page.ready && page.confirmed && !Cloud.busy && Cloud.blockedReason === "" && page.locationId !== ""
            disabledReason: Cloud.blockedReason !== "" ? Cloud.blockedReason : !page.ready ? qsTr("Make the review copy first.") : !page.confirmed ? qsTr("Tick the box to confirm.") : ""
            onClicked: {
                Cloud.publishReviewLink({
                    locationId: page.locationId,
                    title: titleField.text,
                    expiryDays: expiryField.value,
                    removeWhenExpired: removeToggle.checked,
                    instructions: noteField.text
                })
                page.confirmed = false
            }
        }
    }

    // ---- 3. the links and their comments ----
    Heading { text: qsTr("3. Links of this project"); Layout.topMargin: Theme.space(2) }
    Note {
        visible: Cloud.links.length === 0
        text: qsTr("No review link has been published for this project.")
    }
    Repeater {
        model: Cloud.links
        delegate: Card {
            id: link
            required property var modelData
            objectName: "reviewLink_" + link.modelData.id
            implicitHeight: linkColumn.implicitHeight + Theme.space(6)
            ColumnLayout {
                id: linkColumn
                anchors.fill: parent
                anchors.margins: Theme.space(3)
                spacing: Theme.space(2)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Text {
                        Layout.fillWidth: true
                        text: link.modelData.title
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.DemiBold
                    }
                    StatusPill {
                        label: link.modelData.expired ? qsTr("Expired") : qsTr("Live")
                        dotColor: link.modelData.expired ? Theme.warning : Theme.success
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("%1 · %2 · on %3 · %4").arg(link.modelData.createdText).arg(link.modelData.expiresText)
                              .arg(link.modelData.locationName).arg(link.modelData.sizeText)
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Note {
                    visible: link.modelData.changed
                    color: Theme.warning
                    text: qsTr("The cut has changed since this copy was made. Make a new one before asking for more comments.")
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    PillButton {
                        objectName: "reviewCopyLink"
                        compact: true
                        visible: link.modelData.canCopyLink
                        text: qsTr("Copy link")
                        iconName: "link"
                        onClicked: Cloud.copyLink(link.modelData.id)
                    }
                    PillButton {
                        objectName: "reviewOpenFolder"
                        compact: true
                        visible: Cloud.linkFolder(link.modelData.id) !== ""
                        text: qsTr("Show the folder")
                        iconName: "folder-open"
                        onClicked: Qt.openUrlExternally("file:///" + Cloud.linkFolder(link.modelData.id).split("\\").join("/"))
                    }
                    PillButton {
                        objectName: "reviewGetComments"
                        compact: true
                        variant: "primary"
                        text: qsTr("Get comments")
                        iconName: "download"
                        enabled: !Cloud.busy
                        onClicked: Cloud.collectComments(link.modelData.id, page.alsoMarkers)
                    }
                    PillButton {
                        objectName: "reviewImportFile"
                        compact: true
                        text: qsTr("Open a comments file…")
                        iconName: "folder-open"
                        enabled: !Cloud.busy
                        onClicked: {
                            commentsFile.linkId = link.modelData.id
                            commentsFile.open()
                        }
                    }
                    PillButton {
                        objectName: "reviewRevoke"
                        compact: true
                        variant: "ghost"
                        text: qsTr("Revoke")
                        iconName: "trash-2"
                        enabled: !Cloud.busy
                        onClicked: Cloud.revokeLink(link.modelData.id)
                    }
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        visible: Cloud.links.length > 0
        spacing: Theme.space(3)
        Toggle {
            objectName: "reviewAlsoMarkers"
            checked: page.alsoMarkers
            text: qsTr("Also make sequence markers of new comments")
            onToggled: page.alsoMarkers = checked
        }
        PillButton {
            objectName: "reviewRemoveExpiredLinks"
            compact: true
            variant: "ghost"
            text: qsTr("Remove expired links")
            enabled: !Cloud.busy && Cloud.links.some(l => l.expired)
            onClicked: Cloud.removeExpiredLinks()
        }
    }

    // What the last import did.
    Card {
        objectName: "reviewImportResult"
        visible: (Cloud.lastImport.files ?? -1) >= 0 && Cloud.lastImport.files !== undefined
        implicitHeight: importColumn.implicitHeight + Theme.space(6)
        ColumnLayout {
            id: importColumn
            anchors.fill: parent
            anchors.margins: Theme.space(3)
            spacing: Theme.space(1)
            Text {
                Layout.fillWidth: true
                text: qsTr("%n comment(s) added as review notes", "", Cloud.lastImport.added ?? 0)
                      + (Cloud.lastImport.reviewers && Cloud.lastImport.reviewers.length > 0 ? qsTr(", from %1.").arg(Cloud.lastImport.reviewers.join(", ")) : ".")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            Note {
                visible: (Cloud.lastImport.unchanged ?? 0) > 0
                text: qsTr("%n already in the project.", "", Cloud.lastImport.unchanged ?? 0)
            }
            Repeater {
                model: (Cloud.lastImport.warnings ?? []).concat(Cloud.lastImport.conflicts ?? [])
                delegate: Note {
                    required property string modelData
                    color: Theme.warning
                    text: modelData
                }
            }
        }
    }

    FileDialog {
        id: commentsFile
        property string linkId: ""
        title: qsTr("Choose the comments file")
        nameFilters: [qsTr("Review comments (*.json)"), qsTr("All files (*)")]
        onAccepted: Cloud.importCommentsFile(linkId, selectedFile, page.alsoMarkers)
    }
}
