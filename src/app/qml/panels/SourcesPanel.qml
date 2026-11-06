pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// ● SOURCES (0.7, card 177): sources and citations for video essays. The panel lists the sequence's claims by time with the
// source behind each; a claim that still needs one is flagged (red dot, "Needs a source") and can be shown alone. "Add source"
// cites a claim at the playhead, over the In-Out range or over the selected clips; a row opens to edit its reference
// (address, title, author, publisher, date, archive link, screenshot, note). Click a row to move the playhead to the
// claim. The export button copies or saves the list as a YouTube description block with timestamps, a numbered list,
// Markdown, an APA bibliography, or a PDF; the lower-third button shows each cited source on screen over its claim.
// Every change is one undo step (the sources.* commands); a source is a marker of type Source, so it ripples with
// the edit and shows on the ruler.
Panel {
    id: root

    objectName: "sourcesPanel"
    title: qsTr("Sources")

    /// The row being edited (its marker index), -1 = none; `adding` opens the form for a new claim.
    property int editing: -1
    property bool adding: false
    property string exportFormat: "youtube"
    property bool exportWithUncited: false

    function focusContent() {
        if (root.adding || root.editing >= 0)
            form.focusClaim()
        else
            list.forceActiveFocus(Qt.TabFocusReason)
    }
    function openAdd() {
        root.editing = -1
        root.adding = true
        // Words selected in the Transcript tab are the claim, and the claim's place.
        const span = Transcript.hasSelection ? Transcript.selectionTimeSpan() : ({})
        form.load(span.text !== undefined ? { claim: span.text, range: "transcript" } : {})
        form.focusClaim()
    }
    function openEdit(row: var) {
        root.adding = false
        root.editing = row.index
        form.load(row)
    }
    function closeForm() {
        root.adding = false
        root.editing = -1
    }
    function copyText(format: string) {
        const exported = Sources.exportText(format, false)
        if (exported.text === "") {
            messageText.text = qsTr("Nothing to copy: no claim has a source yet")
            return
        }
        clipboardHelper.text = exported.text
        clipboardHelper.selectAll()
        clipboardHelper.copy()
        messageText.text = exported.uncited > 0 ? qsTr("Copied. %1").arg((exported.warnings ?? [])[0] ?? "") : qsTr("Copied")
    }

    TextEdit {
        id: clipboardHelper
        visible: false
    }

    leading: [
        IconButton {
            objectName: "sourcesAddButton"
            compact: true
            iconName: "plus"
            tooltip: qsTr("Cite a source for a claim")
            onClicked: root.openAdd()
        },
        IconButton {
            id: exportBtn
            objectName: "sourcesExportButton"
            compact: true
            iconName: "file-down"
            tooltip: qsTr("Copy or save the list of sources")
            enabled: Sources.count > 0
            onClicked: exportMenu.popup(exportBtn, 0, exportBtn.height + Theme.space(1))
        },
        IconButton {
            objectName: "sourcesLowerThirdsButton"
            compact: true
            iconName: "layers"
            tooltip: qsTr("Show each cited source on screen as a lower third")
            enabled: Sources.count - Sources.neededCount > 0
            onClicked: {
                const error = Sources.addLowerThirds(4.0)
                messageText.text = error !== "" ? error : qsTr("Lower thirds added on new Sources tracks")
            }
        }
    ]

    OptionsMenu {
        id: exportMenu
        objectName: "sourcesExportMenu"
        options: [
            { id: "copy:youtube", name: qsTr("Copy for a YouTube description") },
            { id: "copy:apa", name: qsTr("Copy the bibliography (APA)") },
            { id: "copy:plain", name: qsTr("Copy as a numbered list") },
            { id: "save:markdown", name: qsTr("Save as Markdown…") },
            { id: "save:apa", name: qsTr("Save the bibliography…") },
            { id: "save:pdf", name: qsTr("Save as PDF…") }
        ]
        onChosen: id => {
            const parts = id.split(":")
            if (parts[0] === "copy") {
                root.copyText(parts[1])
            } else {
                root.exportFormat = parts[1] === "pdf" ? "markdown" : parts[1]
                saveDialog.pdf = parts[1] === "pdf"
                saveDialog.nameFilters = parts[1] === "pdf" ? [qsTr("PDF files (*.pdf)")] : [qsTr("Text files (*.txt *.md)")]
                saveDialog.defaultSuffix = parts[1] === "pdf" ? "pdf" : (parts[1] === "markdown" ? "md" : "txt")
                saveDialog.open()
            }
        }
    }
    FileDialog {
        id: saveDialog
        objectName: "sourcesSaveDialog"
        property bool pdf: false
        title: qsTr("Save the list of sources")
        fileMode: FileDialog.SaveFile
        onAccepted: {
            const error = Sources.exportFile(selectedFile, root.exportFormat, root.exportWithUncited)
            messageText.text = error !== "" ? error : qsTr("Saved")
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        // The count and the filter.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                objectName: "sourcesSummary"
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: Sources.count === 0 ? qsTr("No sources yet")
                      : Sources.neededCount === 0 ? qsTr("%n claim(s), all with a source", "", Sources.count)
                      : qsTr("%n claim(s), %1 without a source", "", Sources.count).arg(Sources.neededCount)
                color: Sources.neededCount > 0 ? Theme.warningText : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Toggle {
                objectName: "sourcesOnlyNeeded"
                text: qsTr("Needs a source")
                checked: Sources.onlyNeeded
                onToggled: Sources.onlyNeeded = checked
                Accessible.name: qsTr("Show only the claims that still need a source")
            }
        }

        // The form for a new claim or an edited one: it takes the panel's room (the list is hidden meanwhile) and scrolls.
        ScrollView {
            id: formCard
            objectName: "sourcesForm"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.adding || root.editing >= 0
            clip: true
            contentWidth: availableWidth
            ScrollBar.vertical: ThinScrollBar {}

            SourceForm {
                id: form
                width: formCard.availableWidth - Theme.space(1)
                adding: root.adding
                onSubmitted: fields => {
                    const error = root.adding ? Sources.add(fields.claim, fields.values, fields.range)
                                              : Sources.update(root.editing, Object.assign({ claim: fields.claim }, fields.values))
                    if (error === "") {
                        messageText.text = ""
                        root.closeForm()
                    } else {
                        messageText.text = error
                    }
                }
                onCancelled: root.closeForm()
            }
        }

        Text {
            id: messageText
            objectName: "sourcesMessage"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        ListView {
            id: list
            objectName: "sourcesList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !formCard.visible
            clip: true
            model: Sources.items
            spacing: Theme.space(1)
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Sources by time")

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                objectName: "sourceRow_" + modelData.index
                width: ListView.view.width
                implicitHeight: rowColumn.implicitHeight + Theme.space(3)
                height: implicitHeight
                radius: Theme.radiusField
                color: modelData.atPlayhead ? Theme.hover : (rowMouse.containsMouse ? Theme.raised : "transparent")
                border.width: modelData.needed ? Theme.hairline : 0
                border.color: Theme.warning
                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1 %2%3").arg(modelData.clock).arg(modelData.claim).arg(modelData.needed ? qsTr(", needs a source") : "")

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Sources.seekTo(row.modelData.index)
                    onDoubleClicked: root.openEdit(row.modelData)
                }
                ColumnLayout {
                    id: rowColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.space(1.5)
                    spacing: Theme.space(0.5)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        Rectangle {
                            Layout.preferredWidth: Theme.space(2)
                            Layout.preferredHeight: Theme.space(2)
                            radius: Theme.space(1)
                            color: row.modelData.needed ? Theme.danger : Theme.accentFill
                        }
                        Text {
                            text: row.modelData.clock
                            color: Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: row.modelData.claim
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                        }
                        IconButton {
                            objectName: "sourceEdit_" + row.modelData.index
                            compact: true
                            iconName: "pencil"
                            tooltip: qsTr("Edit this source")
                            onClicked: root.openEdit(row.modelData)
                        }
                        IconButton {
                            objectName: "sourceDelete_" + row.modelData.index
                            compact: true
                            iconName: "trash-2"
                            tooltip: qsTr("Remove this source")
                            onClicked: Sources.remove(row.modelData.index)
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.space(4)
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        color: row.modelData.needed ? Theme.warningText : Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        text: {
                            if (!row.modelData.cited)
                                return qsTr("Needs a source")
                            const parts = [row.modelData.title, row.modelData.author, row.modelData.publisher, row.modelData.date].filter(p => p !== "")
                            const reference = parts.join(" · ")
                            const address = row.modelData.url
                            return reference !== "" && address !== "" ? reference + "\n" + address : (reference !== "" ? reference : address)
                        }
                    }
                }
            }

            Text {
                objectName: "sourcesEmptyState"
                anchors.centerIn: parent
                width: parent.width - Theme.space(8)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: list.count === 0
                text: Sources.count === 0 ? qsTr("Cite the sources behind your claims. Park the playhead on a claim and choose Add.")
                                          : qsTr("Every claim has a source.")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
        }
    }
}
