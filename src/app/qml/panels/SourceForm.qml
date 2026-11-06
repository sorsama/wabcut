pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The form of one source (Sources panel, 0.7 card 177): the claim it backs and the reference - address, title, author,
// publisher, date, archive link, screenshot, a note. For a new claim it also asks where the claim is (the playhead, the
// In-Out range or the selected clips). `submitted` carries { claim, range, values } for Sources.add / Sources.update.
ColumnLayout {
    id: root

    /// A new claim (asks where it is) or an edit of one.
    property bool adding: true
    property alias claimInput: claimField

    signal submitted(var fields)
    signal cancelled

    spacing: Theme.space(1.5)

    function focusClaim() {
        claimField.forceActiveFocus(Qt.TabFocusReason)
    }
    /// Fills the fields from a row of Sources.items ({} for a new claim).
    function load(row: var) {
        claimField.text = row.claim ?? ""
        urlField.text = row.url ?? ""
        titleField.text = row.title ?? ""
        authorField.text = row.author ?? ""
        publisherField.text = row.publisher ?? ""
        dateField.text = row.date ?? ""
        archiveField.text = row.archive ?? ""
        screenshotField.text = row.screenshot ?? ""
        noteField.text = row.note ?? ""
        neededSwitch.checked = row.needed ?? false
        rangeTabs.currentIndex = row.range === "transcript" ? 3 : 0
    }
    function submit() {
        const values = {}
        const optional = { url: urlField.text, title: titleField.text, author: authorField.text,
                           publisher: publisherField.text, date: dateField.text, archive: archiveField.text,
                           screenshot: screenshotField.text, note: noteField.text }
        for (const key in optional) {
            const text = optional[key].trim()
            // A new claim sends what is filled in; an edit sends every field, so an emptied one is cleared.
            if (!root.adding || text !== "")
                values[key] = text
        }
        if (!root.adding)
            values.needed = neededSwitch.checked
        root.submitted({ claim: claimField.text.trim(), values: values,
                         range: ["playhead", "marks", "selection", "transcript"][rangeTabs.currentIndex] })
    }

    component FieldLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }

    FieldLabel {
        text: qsTr("Claim")
    }
    FieldInput {
        id: claimField
        objectName: "sourceClaim"
        Layout.fillWidth: true
        placeholderText: qsTr("What the video says, e.g. Unemployment fell 4% in 2023")
        Accessible.name: qsTr("The claim the source backs")
        onAccepted: root.submit()
    }
    FieldLabel {
        visible: root.adding
        text: qsTr("Where the claim is")
    }
    PillTabs {
        id: rangeTabs
        objectName: "sourceRange"
        Layout.fillWidth: true
        visible: root.adding
        compact: true
        model: [qsTr("Playhead"), qsTr("In to Out"), qsTr("Selected clips"), qsTr("Transcript line")]
        Accessible.name: qsTr("Where the claim is")
    }
    FieldLabel {
        text: qsTr("Address")
    }
    FieldInput {
        id: urlField
        objectName: "sourceUrl"
        Layout.fillWidth: true
        placeholderText: qsTr("https://…")
        Accessible.name: qsTr("The source's address")
        onAccepted: root.submit()
    }
    FieldLabel {
        text: qsTr("Title")
    }
    FieldInput {
        id: titleField
        objectName: "sourceTitle"
        Layout.fillWidth: true
        placeholderText: qsTr("The article, video, paper or book")
        Accessible.name: qsTr("The source's title")
        onAccepted: root.submit()
    }
    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        ColumnLayout {
            width: Math.max(Theme.space(40), (parent.width - Theme.space(2)) / 2)
            spacing: Theme.space(1)
            FieldLabel {
                text: qsTr("Author")
            }
            FieldInput {
                id: authorField
                objectName: "sourceAuthor"
                Layout.fillWidth: true
                Accessible.name: qsTr("The author")
                onAccepted: root.submit()
            }
        }
        ColumnLayout {
            width: Math.max(Theme.space(40), (parent.width - Theme.space(2)) / 2)
            spacing: Theme.space(1)
            FieldLabel {
                text: qsTr("Publisher")
            }
            FieldInput {
                id: publisherField
                objectName: "sourcePublisher"
                Layout.fillWidth: true
                Accessible.name: qsTr("The publisher or outlet")
                onAccepted: root.submit()
            }
        }
        ColumnLayout {
            width: Math.max(Theme.space(40), (parent.width - Theme.space(2)) / 2)
            spacing: Theme.space(1)
            FieldLabel {
                text: qsTr("Date")
            }
            FieldInput {
                id: dateField
                objectName: "sourceDate"
                Layout.fillWidth: true
                placeholderText: qsTr("2024-02-01")
                Accessible.name: qsTr("The publication date")
                onAccepted: root.submit()
            }
        }
    }
    FieldLabel {
        text: qsTr("Archive link")
    }
    FieldInput {
        id: archiveField
        objectName: "sourceArchive"
        Layout.fillWidth: true
        placeholderText: qsTr("https://web.archive.org/…")
        Accessible.name: qsTr("An archive link of the source")
        onAccepted: root.submit()
    }
    FieldLabel {
        text: qsTr("Screenshot")
    }
    FieldInput {
        id: screenshotField
        objectName: "sourceScreenshot"
        Layout.fillWidth: true
        placeholderText: qsTr("A file name or address")
        Accessible.name: qsTr("A screenshot of the source")
        onAccepted: root.submit()
    }
    FieldLabel {
        text: qsTr("Note")
    }
    FieldInput {
        id: noteField
        objectName: "sourceNote"
        Layout.fillWidth: true
        Accessible.name: qsTr("A note for yourself")
        onAccepted: root.submit()
    }
    Toggle {
        id: neededSwitch
        objectName: "sourceNeeded"
        visible: !root.adding
        text: qsTr("Still needs a source")
        Accessible.name: qsTr("This claim still needs a source")
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        Item {
            Layout.fillWidth: true
        }
        PillButton {
            objectName: "sourceCancel"
            variant: "ghost"
            compact: true
            text: qsTr("Cancel")
            onClicked: root.cancelled()
        }
        PillButton {
            objectName: "sourceSave"
            variant: "primary"
            compact: true
            text: root.adding ? qsTr("Add") : qsTr("Save")
            enabled: claimField.text.trim() !== ""
            onClicked: root.submit()
        }
    }
}
