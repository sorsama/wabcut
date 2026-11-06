pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Generate Audio Description (0.7, card 175; Sequence > Generate Audio Description…): a spoken layer for blind and
// low-vision viewers, said in the gaps between the dialogue.
//   1. Find the gaps in the dialogue that are long enough (from the transcript);
//   2. Describe: a chat model that can see pictures (yours, after consent, or one on this computer) is shown a frame of each
//      gap and the words around it, and writes one short line that fits - nothing is sent before you agree and see the cost;
//   3. read and edit every line (the count shows how many words fit the gap), drop or write any;
//   4. Put the lines on a caption track "Audio Description" (switched off, editable in the Captions tab), then Voice… them
//      with the Dub dialog (the dialogue and music go down under each line) or Export a script for a narrator.
WabcutDialog {
    id: dialog
    objectName: "audioDescriptionDialog"

    property var quote: ({})
    property bool placed: false

    function openDialog() {
        dialog.placed = false
        AudioDescription.clearLines()
        dialog.quote = ({})
        open()
        if (AudioDescription.lines.length === 0)
            dialog.findGaps()
        else
            dialog.refreshQuote()
    }
    function options(): var {
        return { minGap: [1.5, 2.0, 3.0, 5.0][gapTabs.currentIndex], silent: silentSwitch.checked }
    }
    function describeOptions(allowOnce: bool): var {
        return { allowOnce: allowOnce, frames: [1, 2, 3][frameTabs.currentIndex], language: languageField.text.trim(),
                 style: guidanceField.text.trim() }
    }
    function findGaps() {
        dialog.placed = false
        const error = AudioDescription.analyze(dialog.options())
        if (error === "")
            dialog.refreshQuote()
    }
    function refreshQuote() {
        dialog.quote = AudioDescription.quote(dialog.describeOptions(false))
    }
    function describe() {
        const q = AudioDescription.quote(dialog.describeOptions(false))
        dialog.quote = q
        if (q.available !== true)
            return
        if (q.needed === true) {
            consent.ask(q.prompt)
            return
        }
        AudioDescription.describe(dialog.describeOptions(false))
    }

    title: qsTr("Generate Audio Description")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(190), Theme.space(190))

    component SectionLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }

    AiConsentDialog {
        id: consent
        onAllowed: always => {
            if (always)
                AiController.grantConsent(consent.prompt.provider, "chat")
            AudioDescription.describe(dialog.describeOptions(!always))
        }
    }
    FileDialog {
        id: scriptDialog
        objectName: "audioDescriptionScriptDialog"
        title: qsTr("Export the script for a narrator")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "txt"
        nameFilters: [qsTr("Text files (*.txt)")]
        onAccepted: {
            const error = AudioDescription.exportScript(selectedFile)
            if (error !== "")
                statusText.text = error
        }
    }

    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("Finds the gaps between the dialogue, shows a picture from each to a model that can see, and writes a short line for the gaps worth describing. You read and change every line before anything is voiced.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    // 1. The gaps.
    SectionLabel {
        text: qsTr("1 · Gaps in the dialogue")
    }
    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        RowLayout {
            spacing: Theme.space(2)
            Text {
                text: qsTr("Shortest gap")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                Layout.alignment: Qt.AlignVCenter
            }
            PillTabs {
                id: gapTabs
                objectName: "audioDescriptionMinGap"
                compact: true
                currentIndex: 1
                model: [qsTr("1.5 s"), qsTr("2 s"), qsTr("3 s"), qsTr("5 s")]
                Accessible.name: qsTr("The shortest gap worth a line")
                onActivated: dialog.findGaps()
            }
        }
        Toggle {
            id: silentSwitch
            objectName: "audioDescriptionSilent"
            text: qsTr("This video has no speech")
            Accessible.name: qsTr("This video has no speech: describe all of it")
            onToggled: dialog.findGaps()
        }
        PillButton {
            objectName: "audioDescriptionFind"
            compact: true
            variant: "secondary"
            iconName: "search"
            text: qsTr("Find gaps")
            enabled: !AudioDescription.running
            onClicked: dialog.findGaps()
        }
    }
    Text {
        objectName: "audioDescriptionGapSummary"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: AudioDescription.lines.length === 0 ? (AudioDescription.message !== "" ? AudioDescription.message : qsTr("No gaps found yet"))
                                                  : qsTr("%n gap(s) found", "", AudioDescription.lines.length)
        color: AudioDescription.lines.length === 0 ? Theme.warningText : Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    // 2. The model.
    SectionLabel {
        text: qsTr("2 · Describe")
    }
    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        RowLayout {
            spacing: Theme.space(2)
            Text {
                text: qsTr("Pictures per gap")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                Layout.alignment: Qt.AlignVCenter
            }
            PillTabs {
                id: frameTabs
                objectName: "audioDescriptionFrames"
                compact: true
                model: ["1", "2", "3"]
                Accessible.name: qsTr("Pictures sent for each gap")
                onActivated: dialog.refreshQuote()
            }
        }
        RowLayout {
            spacing: Theme.space(2)
            Text {
                text: qsTr("Language")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                Layout.alignment: Qt.AlignVCenter
            }
            FieldInput {
                id: languageField
                objectName: "audioDescriptionLanguage"
                Layout.preferredWidth: Theme.space(20)
                text: qsTr("en")
                maximumLength: 8
                Accessible.name: qsTr("The language of the lines (ISO 639-1 code)")
            }
        }
    }
    FieldInput {
        id: guidanceField
        objectName: "audioDescriptionGuidance"
        Layout.fillWidth: true
        placeholderText: qsTr("Guidance for the describer, e.g. name the host Ana; British spelling")
        Accessible.name: qsTr("Guidance for the describer")
    }
    Text {
        objectName: "audioDescriptionQuote"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: dialog.quote.available === true
              ? (dialog.quote.local ? qsTr("%1 on this computer: %2 · %n picture(s)", "", dialog.quote.pictures).arg(dialog.quote.name).arg(dialog.quote.cost)
                                    : qsTr("%1: %2 · %n picture(s) sent, with the words around each gap, as text", "", dialog.quote.pictures).arg(dialog.quote.name).arg(dialog.quote.cost))
              : (dialog.quote.reason ?? "")
        color: dialog.quote.available === true ? Theme.textSecondary : Theme.warningText
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        PillButton {
            objectName: "audioDescriptionDescribe"
            variant: "primary"
            iconName: "sparkles"
            text: qsTr("Describe")
            enabled: !AudioDescription.running && AudioDescription.lines.length > 0 && dialog.quote.available === true
            onClicked: dialog.describe()
        }
        PillButton {
            objectName: "audioDescriptionCancel"
            variant: "ghost"
            text: qsTr("Stop")
            visible: AudioDescription.running
            onClicked: AudioDescription.cancel()
        }
        BeamProgress {
            objectName: "audioDescriptionProgress"
            Layout.fillWidth: true
            visible: AudioDescription.running
            value: AudioDescription.progress
            label: qsTr("Describing")
        }
        Text {
            id: statusText
            objectName: "audioDescriptionStatus"
            Layout.fillWidth: true
            visible: !AudioDescription.running
            wrapMode: Text.Wrap
            text: AudioDescription.lines.length > 0 ? AudioDescription.message : ""
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    // 3. The lines.
    SectionLabel {
        text: qsTr("3 · Lines")
    }
    ListView {
        id: lineList
        objectName: "audioDescriptionLines"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(Theme.space(80), Math.max(Theme.space(30), contentHeight))
        clip: true
        spacing: Theme.space(1.5)
        model: AudioDescription.lines
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Audio description lines by time")

        delegate: Rectangle {
            id: row
            required property var modelData
            width: ListView.view.width
            implicitHeight: lineColumn.implicitHeight + Theme.space(3)
            height: implicitHeight
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: row.modelData.over ? Theme.warning : Theme.border

            ColumnLayout {
                id: lineColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.space(1.5)
                spacing: Theme.space(1)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Text {
                        text: row.modelData.startText
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textSm
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("%1 s gap").arg(row.modelData.seconds)
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        elide: Text.ElideRight
                    }
                    Text {
                        objectName: "audioDescriptionWords_" + row.modelData.index
                        text: qsTr("%1 / %2 words").arg(row.modelData.words).arg(row.modelData.maxWords)
                        color: row.modelData.over ? Theme.warningText : Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    IconButton {
                        objectName: "audioDescriptionClear_" + row.modelData.index
                        compact: true
                        iconName: "trash-2"
                        tooltip: qsTr("Leave this gap silent")
                        enabled: row.modelData.text !== ""
                        onClicked: AudioDescription.removeLine(row.modelData.index)
                    }
                }
                FieldInput {
                    objectName: "audioDescriptionText_" + row.modelData.index
                    Layout.fillWidth: true
                    text: row.modelData.text
                    placeholderText: qsTr("Silent. Write a line if something here needs saying.")
                    Accessible.name: qsTr("The line at %1").arg(row.modelData.startText)
                    onEditingFinished: AudioDescription.setLineText(row.modelData.index, text)
                }
                Text {
                    Layout.fillWidth: true
                    visible: row.modelData.before !== "" || row.modelData.after !== ""
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    text: qsTr("Said before: “%1”   After: “%2”").arg(row.modelData.before).arg(row.modelData.after)
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
        }
    }

    footerData: [
        PillButton {
            objectName: "audioDescriptionClose"
            variant: "ghost"
            text: qsTr("Close")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "audioDescriptionScript"
            variant: "secondary"
            iconName: "file-down"
            text: qsTr("Export script…")
            tooltip: qsTr("A script with timecodes for a narrator to read")
            enabled: AudioDescription.scriptText().length > 60
            onClicked: scriptDialog.open()
        },
        PillButton {
            objectName: "audioDescriptionPlace"
            variant: dialog.placed ? "secondary" : "primary"
            iconName: "captions"
            text: dialog.placed ? qsTr("Put on the track again") : qsTr("Put on a track")
            tooltip: qsTr("Makes the caption track “Audio Description” (switched off, so it is not burned in)")
            enabled: !AudioDescription.running && AudioDescription.lines.some(l => l.text !== "")
            onClicked: {
                const error = AudioDescription.apply(languageField.text.trim())
                dialog.placed = error === ""
                if (error !== "")
                    statusText.text = error
                else
                    statusText.text = qsTr("The lines are on the “Audio Description” caption track (Captions tab)")
            }
        },
        PillButton {
            objectName: "audioDescriptionVoice"
            variant: "primary"
            iconName: "mic"
            text: qsTr("Voice…")
            tooltip: qsTr("Opens the Dub dialog for the track: choose the voice and see the cost first")
            enabled: dialog.placed
            onClicked: {
                const error = AudioDescription.voice(languageField.text.trim())
                if (error === "")
                    dialog.close()
                else
                    statusText.text = error
            }
        }
    ]
}
