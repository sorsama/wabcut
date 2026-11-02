pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// The Creator autopilot's publishing kit page (AutopilotSheet): Prepare kit makes title ideas, a description (lead,
// chapters, hashtags), chapters, tags and thumbnail candidates for the sequence on this computer; the title,
// description and tags can be edited here. Write files puts them in a "<sequence> - publishing kit" folder (text files,
// thumbnail JPEGs at 1280 x 720 under 2 MB, a Markdown page and JSON). Nothing is uploaded. "Write with my AI" lets the
// user's chat model write the copy from the transcript text: the cost shows first and consent is asked.
ColumnLayout {
    id: page
    objectName: "autopilotKitPage"

    signal said(string text)

    readonly property var kit: Autopilot.kit
    readonly property bool ready: kit.title !== undefined
    readonly property var chapterModes: ["auto", "markers", "topics", "none"]
    readonly property var thumbCounts: [3, 4, 6]
    property var quote: ({})
    /// Where Write files puts the kit's folder (remembered by the bridge).
    property url folder: Autopilot.kitFolder

    spacing: Theme.space(2)

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Field: TextField {
        Layout.fillWidth: true
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        selectByMouse: true
        background: Rectangle {
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: parent.activeFocus ? Theme.focus : Theme.border
        }
    }

    /// Prepare kit; with "Write with my AI" the consent prompt comes first when the chat model needs it.
    function prepare() {
        if (cloudToggle.checked) {
            const q = Autopilot.kitQuote()
            if (q.available !== true) {
                page.said(q.reason ?? qsTr("Your AI is not available"))
                return
            }
            if (q.needed === true) {
                consent.ask(q.prompt)
                return
            }
        }
        page.run(false)
    }
    function run(allowOnce: bool) {
        const error = Autopilot.makeKit({
            thumbnails: page.thumbCounts[thumbs.currentIndex],
            chapters: page.chapterModes[chapters.currentIndex],
            cloud: cloudToggle.checked,
            allowOnce: allowOnce
        })
        if (error !== "")
            page.said(error)
    }
    function write() {
        const error = Autopilot.writeKit(page.folder, {
            title: titleField.text,
            description: descriptionArea.text,
            tags: tagsField.text
        })
        if (error !== "")
            page.said(error)
    }

    // A new kit fills the fields; the user's edits stay until the next one.
    onKitChanged: {
        if (page.kit.title === undefined)
            return
        titleField.text = page.kit.title ?? ""
        descriptionArea.text = page.kit.description ?? ""
        tagsField.text = (page.kit.tags ?? []).join(", ")
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Write the publishing kit into")
        currentFolder: page.folder
        onAccepted: page.folder = selectedFolder
    }

    AiConsentDialog {
        id: consent
        onAllowed: always => {
            if (always)
                AiController.grantConsent(consent.prompt.provider, "chat")
            page.run(!always)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        Label { text: qsTr("Thumbnails") }
        PillTabs {
            id: thumbs
            objectName: "autopilotKitThumbnails"
            Accessible.name: qsTr("Thumbnail candidates")
            Layout.preferredWidth: Theme.space(30)
            compact: true
            currentIndex: 1
            model: ["3", "4", "6"]
        }
        Label { text: qsTr("Chapters") }
        PillTabs {
            id: chapters
            objectName: "autopilotKitChapters"
            Accessible.name: qsTr("Chapters from")
            Layout.preferredWidth: Theme.space(64)
            compact: true
            model: [qsTr("Auto"), qsTr("Markers"), qsTr("Topics"), qsTr("None")]
        }
        Item { Layout.fillWidth: true }
        Toggle {
            id: cloudToggle
            objectName: "autopilotKitCloud"
            text: qsTr("Write with my AI")
            enabled: Autopilot.cloudAvailable
            tooltip: Autopilot.cloudAvailable ? qsTr("Sends the transcript text (never the video) to your chat model; shows the cost and asks first")
                                              : qsTr("No cloud chat model, AI is offline, or this project is no-cloud")
            onCheckedChanged: page.quote = checked ? Autopilot.kitQuote() : ({})
        }
        PillButton {
            objectName: "autopilotPrepareKit"
            variant: "primary"
            iconName: "sparkles"
            text: qsTr("Prepare kit")
            enabled: !Autopilot.busy
            onClicked: page.prepare()
        }
    }
    Text {
        objectName: "autopilotKitCost"
        Layout.fillWidth: true
        visible: cloudToggle.checked && (page.quote.cost ?? "") !== ""
        text: qsTr("Your AI (%1): %2 for %3 characters of transcript").arg(page.quote.provider ?? "").arg(page.quote.cost ?? "")
              .arg(page.quote.characters ?? 0)
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: page.ready
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: Theme.space(1.5)

            Label { text: qsTr("Title") }
            Field {
                id: titleField
                objectName: "autopilotKitTitle"
                Accessible.name: qsTr("Title")
                maximumLength: 100
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Repeater {
                    model: page.kit.titles ?? []
                    delegate: PillButton {
                        required property string modelData
                        compact: true
                        variant: "ghost"
                        text: modelData
                        visible: modelData !== titleField.text
                        tooltip: qsTr("Use this title")
                        onClicked: titleField.text = modelData
                    }
                }
            }
            Label { text: qsTr("Description") }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(26)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: descriptionArea.activeFocus ? Theme.focus : Theme.border
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    TextArea {
                        id: descriptionArea
                        objectName: "autopilotKitDescription"
                        wrapMode: TextEdit.Wrap
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        selectByMouse: true
                        background: null
                        Accessible.name: qsTr("Description")
                    }
                }
            }
            Label { text: qsTr("Tags (comma-separated)") }
            Field {
                id: tagsField
                objectName: "autopilotKitTags"
                Accessible.name: qsTr("Tags")
            }
            Label {
                text: (page.kit.chaptersFrom ?? "none") === "markers" ? qsTr("Chapters from your chapter markers")
                      : (page.kit.chaptersFrom ?? "none") === "topics" ? qsTr("Chapters from the topics in the transcript")
                                                                       : qsTr("No chapters")
            }
            Text {
                objectName: "autopilotKitChaptersText"
                Layout.fillWidth: true
                visible: text !== ""
                text: page.kit.chaptersText ?? ""
                color: Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
                wrapMode: Text.Wrap
            }
            Label { text: qsTr("Thumbnail candidates") }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Repeater {
                    model: page.kit.thumbnails ?? []
                    delegate: ColumnLayout {
                        id: thumb
                        required property var modelData
                        required property int index
                        objectName: "autopilotKitThumb_" + (index + 1)
                        spacing: Theme.space(0.5)
                        Rectangle {
                            Layout.preferredWidth: Theme.space(40)
                            Layout.preferredHeight: Theme.space(22.5)
                            radius: Theme.radiusField
                            color: Theme.sunken
                            border.width: thumb.index === 0 ? 1.5 : Theme.hairline
                            border.color: thumb.index === 0 ? Theme.accentStroke : Theme.border
                            Image {
                                anchors.fill: parent
                                anchors.margins: 1
                                source: thumb.modelData.thumb ?? ""
                                sourceSize: Qt.size(320, 180)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                Accessible.name: qsTr("Thumbnail at %1").arg(thumb.modelData.time)
                            }
                        }
                        Text {
                            Layout.preferredWidth: Theme.space(40)
                            text: thumb.modelData.time + " · " + qsTr("score %1").arg(Math.round((thumb.modelData.score ?? 0) * 100))
                                  + ((thumb.modelData.reasons ?? []).length > 1 ? " · " + thumb.modelData.reasons.slice(1).join(", ") : "")
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }
    EmptyState {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: !page.ready && !Autopilot.busy
        illustration: "transcript"
        text: qsTr("Prepare kit writes a title, a description with chapters, tags and thumbnail candidates for this sequence, as files you upload yourself. Nothing is uploaded.")
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        Text {
            objectName: "autopilotKitFolder"
            Layout.fillWidth: true
            text: (page.kit.written === true ? qsTr("Written to %1") : qsTr("Into %1"))
                  .arg(page.kit.written === true ? page.kit.folder : decodeURIComponent(page.folder.toString().replace(/^file:\/{2,3}/, "")))
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            elide: Text.ElideMiddle
        }
        PillButton {
            compact: true
            iconName: "folder-open"
            text: qsTr("Choose…")
            onClicked: folderDialog.open()
        }
        PillButton {
            objectName: "autopilotRevealKit"
            compact: true
            visible: page.kit.written === true
            text: qsTr("Show folder")
            onClicked: Autopilot.revealKit()
        }
        PillButton {
            objectName: "autopilotWriteKit"
            variant: "primary"
            text: qsTr("Write files")
            enabled: page.ready && !Autopilot.busy
            onClicked: page.write()
        }
    }
}
