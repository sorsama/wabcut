pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Rough cut from a script (AI-11): paste a script, an outline or bullet points (or open a .txt / .md file), choose
// where the takes come from and how to pick them, and Make the cut. Every line is found in the recordings' transcripts
// on this computer; the alignment report lists each line with the take chosen, its confidence and the alternates.
// Clicking an alternate swaps it into the staged cut; Create sequence applies the cut as one undo step and opens it.
// Optional: clean-up of fillers and pauses, B-roll suggestions on their own track, and (with a cloud model set up and
// consent) placing loose outline bullets with the user's AI, transcript text only.
WabcutDialog {
    id: sheet
    objectName: "roughCutSheet"

    readonly property var report: RoughCut.report
    readonly property var beats: report.beats ?? []
    readonly property var prefers: ["best", "cleanest", "first", "last"]

    title: qsTr("Rough cut from a script")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(230), Theme.space(230))
    onClosed: RoughCut.discard()

    /// Make the cut: with "Place bullets with my AI" the consent prompt comes first when the chat model needs it.
    function make() {
        if (interpretToggle.checked) {
            const prompt = RoughCut.consentPrompt(scriptArea.text.length)
            if (prompt.needed) {
                consent.ask(prompt)
                return
            }
        }
        run(false)
    }
    /// `allowOnce`: the consent dialog's "Allow once".
    function run(allowOnce) {
        const error = RoughCut.preview(scriptArea.text, {
            scope: scopeTabs.currentIndex === 1 ? "sequence" : "all",
            prefer: prefers[preferTabs.currentIndex],
            cleanup: cleanupToggle.checked,
            broll: brollToggle.checked,
            interpret: interpretToggle.checked,
            allowOnce: allowOnce
        })
        if (error !== "")
            messageText.text = error
    }

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    FileDialog {
        id: openScript
        title: qsTr("Open a script")
        nameFilters: [qsTr("Scripts (*.txt *.md *.markdown *.fountain)")]
        onAccepted: {
            const text = RoughCut.readScript(selectedFile)
            if (text !== "")
                scriptArea.text = text
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(4)

        // ---- The script and the options ----
        ColumnLayout {
            Layout.preferredWidth: Theme.space(84)
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(2)

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Script, outline or bullets"); Layout.fillWidth: true }
                PillButton {
                    compact: true
                    iconName: "folder-open"
                    text: qsTr("Open…")
                    onClicked: openScript.open()
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(70)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: scriptArea.activeFocus ? Theme.focus : Theme.border
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    TextArea {
                        id: scriptArea
                        objectName: "roughCutScript"
                        wrapMode: TextEdit.Wrap
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        selectByMouse: true
                        placeholderText: qsTr("# My video\nHi everyone, welcome back.\n- why we built it\n[B-roll: the product close-up]")
                        placeholderTextColor: Theme.textTertiary
                        background: null
                        Accessible.name: qsTr("Script")
                    }
                }
            }
            Label { text: qsTr("Takes from") }
            PillTabs {
                id: scopeTabs
                objectName: "roughCutScope"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("All recordings"), qsTr("This sequence's media")]
            }
            Label { text: qsTr("Pick") }
            PillTabs {
                id: preferTabs
                objectName: "roughCutPrefer"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Best"), qsTr("Cleanest"), qsTr("First"), qsTr("Last")]
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Toggle { id: cleanupToggle; objectName: "roughCutCleanup"; text: qsTr("Remove fillers and pauses") }
                Toggle { id: brollToggle; objectName: "roughCutBroll"; text: qsTr("Suggest B-roll") }
                Toggle {
                    id: interpretToggle
                    objectName: "roughCutInterpret"
                    text: qsTr("Place bullets with my AI")
                    enabled: RoughCut.cloudAvailable
                    tooltip: RoughCut.cloudAvailable ? qsTr("Sends the outline and matching transcript sentences (text only) to your chat model; asks for consent first")
                                                     : qsTr("No cloud chat model, or this project is no-cloud")
                }
            }
            Note { text: qsTr("Lines are matched on this computer, in English, Thai and other languages, with or without spaces between words.") }
            PillButton {
                objectName: "roughCutMake"
                Layout.fillWidth: true
                variant: "primary"
                iconName: "scissors"
                text: qsTr("Make the cut")
                enabled: !RoughCut.busy && scriptArea.text.trim() !== ""
                onClicked: sheet.make()
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: RoughCut.busy
                spacing: Theme.space(1)
                StatusPill {
                    Layout.fillWidth: true
                    label: RoughCut.phase
                    detail: RoughCut.progress >= 0 ? Math.round(RoughCut.progress * 100) + "%" : ""
                    dotColor: Theme.accentFill
                }
                BeamProgress {
                    Layout.fillWidth: true
                    value: RoughCut.progress >= 0 ? RoughCut.progress : 1
                    active: RoughCut.busy
                    label: RoughCut.phase
                }
            }
            Note {
                id: messageText
                objectName: "roughCutMessage"
                visible: text !== ""
                text: RoughCut.message
                color: Theme.warning
                maximumLineCount: 5
                elide: Text.ElideRight
            }
        }

        // ---- The alignment report ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(2)

            RowLayout {
                Layout.fillWidth: true
                SectionHeader {
                    Layout.fillWidth: true
                    text: qsTr("Alignment report")
                }
                Text {
                    objectName: "roughCutSummary"
                    visible: sheet.beats.length > 0
                    text: qsTr("%1 of %2 lines found · %3 words searched").arg(sheet.report.found ?? 0).arg(sheet.report.beatCount ?? 0)
                          .arg(sheet.report.words ?? 0)
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
            ListView {
                id: beatList
                objectName: "roughCutReport"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(118)
                clip: true
                spacing: Theme.space(1.5)
                model: sheet.beats
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar {}
                delegate: Rectangle {
                    id: beatCard
                    required property var modelData
                    required property int index
                    readonly property var takes: modelData.takes ?? []
                    readonly property real confidence: modelData.confidence ?? 0
                    width: beatList.width
                    implicitHeight: beatColumn.implicitHeight + Theme.space(3)
                    radius: Theme.radiusCard
                    color: Theme.raised
                    border.width: Theme.hairline
                    border.color: modelData.found ? Theme.border : Theme.warning
                    ColumnLayout {
                        id: beatColumn
                        anchors.fill: parent
                        anchors.margins: Theme.space(1.5)
                        spacing: Theme.space(1)
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space(2)
                            Text {
                                text: beatCard.modelData.beat
                                color: Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textSm
                            }
                            Text {
                                Layout.fillWidth: true
                                text: beatCard.modelData.text
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                            // Confidence: a short meter, the number for screen readers.
                            Rectangle {
                                Layout.preferredWidth: Theme.space(12)
                                Layout.preferredHeight: 4
                                radius: 2
                                color: Theme.sunken
                                Accessible.name: qsTr("Confidence %1%").arg(Math.round(beatCard.confidence * 100))
                                Rectangle {
                                    width: parent.width * beatCard.confidence
                                    height: parent.height
                                    radius: 2
                                    color: beatCard.confidence > 0.75 ? Theme.success : beatCard.confidence > 0.5 ? Theme.warning : Theme.danger
                                }
                            }
                            Text {
                                text: beatCard.modelData.range ?? ""
                                color: Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textSm
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: (beatCard.modelData.note ?? "") !== ""
                            text: beatCard.modelData.note ?? ""
                            color: beatCard.modelData.found ? Theme.textTertiary : Theme.warning
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        // The takes: the chosen one first; click another to swap it into the cut.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space(1)
                            Repeater {
                                model: beatCard.takes
                                delegate: Rectangle {
                                    id: takeChip
                                    required property var modelData
                                    required property int index
                                    readonly property bool chosen: modelData.chosen === true
                                    objectName: "roughCutTake_" + beatCard.modelData.beat + "_" + modelData.take
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: takeRow.implicitHeight + Theme.space(1.5)
                                    radius: Theme.radiusField
                                    color: chosen ? Theme.accentWash : (takeMouse.containsMouse ? Theme.hover : Theme.panel)
                                    border.width: chosen ? 1.5 : Theme.hairline
                                    border.color: chosen ? Theme.accentStroke : Theme.border
                                    Accessible.role: Accessible.Button
                                    Accessible.name: (chosen ? qsTr("Chosen take: ") : qsTr("Use take: ")) + modelData.text
                                    RowLayout {
                                        id: takeRow
                                        anchors.fill: parent
                                        anchors.margins: Theme.space(0.75)
                                        spacing: Theme.space(1.5)
                                        Image {
                                            Layout.preferredWidth: Theme.space(12)
                                            Layout.preferredHeight: Theme.space(6.75)
                                            visible: (takeChip.modelData.thumb ?? "") !== ""
                                            source: takeChip.modelData.thumb ?? ""
                                            sourceSize: Qt.size(160, 90)
                                            fillMode: Image.PreserveAspectCrop
                                            asynchronous: true
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Text {
                                                Layout.fillWidth: true
                                                text: takeChip.modelData.text
                                                color: Theme.textPrimary
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.textSm
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: qsTr("%1 · %2 · score %3").arg(takeChip.modelData.mediaName).arg(takeChip.modelData.sourceRange)
                                                      .arg(Math.round((takeChip.modelData.score ?? 0) * 100))
                                                      + ((takeChip.modelData.reasons ?? []).length > 0 ? " · " + takeChip.modelData.reasons.join(", ") : "")
                                                color: Theme.textTertiary
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.textXs
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }
                                    MouseArea {
                                        id: takeMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        enabled: !takeChip.chosen && !RoughCut.busy
                                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                        onClicked: RoughCut.swapTake(beatCard.modelData.beat, takeChip.modelData.take)
                                    }
                                }
                            }
                        }
                    }
                }
                EmptyState {
                    anchors.centerIn: parent
                    width: parent.width - Theme.space(4)
                    visible: beatList.count === 0 && !RoughCut.busy
                    illustration: "transcript"
                    text: qsTr("Paste a script and make the cut: every line is found in your transcribed recordings, with the best take chosen.")
                }
            }
        }
    }

    AiConsentDialog {
        id: consent
        onAllowed: always => {
            if (always)
                AiController.grantConsent(consent.prompt.provider, "chat")
            sheet.run(!always)
        }
    }

    footerData: [
        PillButton {
            text: qsTr("Cancel")
            onClicked: sheet.close()
        },
        PillButton {
            objectName: "roughCutApply"
            variant: "primary"
            text: qsTr("Create sequence")
            enabled: RoughCut.staged && !RoughCut.busy
            onClicked: {
                const error = RoughCut.apply()
                if (error === "")
                    sheet.close()
                else
                    messageText.text = error
            }
        }
    ]
}
