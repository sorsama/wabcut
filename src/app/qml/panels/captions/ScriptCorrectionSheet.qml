pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Correct the transcript from your script (0.6, P1 round #37; Sequence > Captions > Correct Transcript from Script…,
// the Transcript tab's spell-check button). Paste the script the narration was read from (or open a .txt / .md file)
// and Find corrections: every line is found in the sequence's transcript, on a worker (progress, Stop), and the words
// the recogniser got wrong (names, jargon, numbers written another way, names in the wrong capitals) are listed as
// "time  before → after" (a click goes there). Apply makes them, one undo step; then Update captions brings them into
// the caption track. Only substitutions that sound like the script: a word you skipped is never added, a word you added
// never removed, a word you changed is kept and listed under "Left as said"; lines not found closely enough are listed
// and left alone. Per-change accept/reject is not here: undo and edit the script instead (ScriptCorrection,
// transcript.correctFromScript).
WabcutDialog {
    id: sheet
    objectName: "scriptCorrectionSheet"

    /// The last Find corrections (ScriptCorrection.result): {} before one.
    readonly property var result: ScriptCorrection.result
    readonly property var changes: sheet.result.changes ?? []
    readonly property var skipped: sheet.result.skipped ?? []
    readonly property var kept: sheet.result.kept ?? []
    readonly property bool hasResult: sheet.result.corrected !== undefined
    readonly property bool busy: ScriptCorrection.busy
    readonly property int appliedCount: ScriptCorrection.applied

    title: qsTr("Correct the transcript from your script")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(220), Theme.space(220))

    /// Why Find did not start (the bridge refused before running), until the script changes.
    property string refusal: ""

    function findCorrections() {
        sheet.refusal = ScriptCorrection.find(scriptArea.text)
    }
    function applyCorrections() {
        ScriptCorrection.apply()
    }

    onAboutToShow: {
        sheet.refusal = ""
        ScriptCorrection.discard()
    }
    onClosed: ScriptCorrection.discard()

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
        objectName: "scriptCorrectionFileDialog"
        title: qsTr("Open a script")
        nameFilters: [qsTr("Scripts (*.txt *.md *.markdown *.fountain)")]
        onAccepted: {
            sheet.refusal = "" // a failed read must show its own error, not an earlier refusal
            const text = ScriptCorrection.readScript(selectedFile)
            if (text !== "")
                scriptArea.text = text
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(4)

        // ---- The script ----
        ColumnLayout {
            Layout.preferredWidth: Theme.space(84)
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(2)

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Your script"); Layout.fillWidth: true }
                PillButton {
                    objectName: "scriptCorrectionOpen"
                    compact: true
                    iconName: "folder-open"
                    text: qsTr("Open script…")
                    tooltip: qsTr("A .txt, .md or .fountain file")
                    enabled: !sheet.busy
                    onClicked: openScript.open()
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(70)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: scriptArea.activeFocus ? Theme.focusRingWidth : Theme.hairline
                border.color: scriptArea.activeFocus ? Theme.focus : Theme.border
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    TextArea {
                        id: scriptArea
                        objectName: "scriptCorrectionText"
                        wrapMode: TextEdit.Wrap
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        selectByMouse: true
                        selectionColor: Theme.accentFill
                        selectedTextColor: Theme.accentOnFill
                        placeholderText: qsTr("Paste the script you read from.\nToday we test Wabcut with Dr. Somchai.")
                        placeholderTextColor: Theme.textTertiary
                        background: null
                        readOnly: sheet.busy
                        Accessible.name: qsTr("Script")
                        // A changed script makes the listed corrections stale.
                        onTextChanged: {
                            sheet.refusal = ""
                            if (sheet.hasResult || ScriptCorrection.message !== "")
                                ScriptCorrection.discard()
                        }
                    }
                }
            }
            Note {
                text: qsTr("Names, jargon and misheard words take your script's spelling, capitals and punctuation. Words you skipped are not added, words you added are not removed, and words you changed are left as you said them.")
            }
            PillButton {
                objectName: "scriptCorrectionFind"
                Layout.fillWidth: true
                visible: !sheet.busy
                variant: sheet.hasResult ? "secondary" : "primary"
                iconName: "search"
                text: qsTr("Find corrections")
                enabled: scriptArea.text.trim() !== ""
                onClicked: sheet.findCorrections()
            }
            // Finding: how far it is, and Stop.
            ColumnLayout {
                objectName: "scriptCorrectionProgress"
                Layout.fillWidth: true
                visible: sheet.busy
                spacing: Theme.space(1)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    StatusPill {
                        Layout.fillWidth: true
                        label: qsTr("Finding your lines in the transcript")
                        detail: ScriptCorrection.progress >= 0 ? Math.round(ScriptCorrection.progress * 100) + "%" : ""
                        dotColor: Theme.accentFill
                    }
                    PillButton {
                        objectName: "scriptCorrectionStop"
                        compact: true
                        iconName: "x"
                        text: qsTr("Stop")
                        onClicked: ScriptCorrection.cancel()
                    }
                }
                BeamProgress {
                    Layout.fillWidth: true
                    value: ScriptCorrection.progress >= 0 ? ScriptCorrection.progress : 1
                    indeterminate: ScriptCorrection.progress < 0
                    active: sheet.busy
                    label: qsTr("Finding your lines in the transcript")
                }
            }
            Note {
                objectName: "scriptCorrectionError"
                visible: text !== "" && !sheet.busy && sheet.appliedCount < 0
                text: sheet.refusal !== "" ? sheet.refusal : ScriptCorrection.message
                color: Theme.warningText
                maximumLineCount: 4
                elide: Text.ElideRight
            }
        }

        // ---- The corrections ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(2)

            RowLayout {
                Layout.fillWidth: true
                SectionHeader {
                    Layout.fillWidth: true
                    text: qsTr("Corrections")
                }
                Text {
                    objectName: "scriptCorrectionSummary"
                    visible: sheet.hasResult
                    text: ((sheet.result.corrected ?? 0) === 1 ? qsTr("1 correction")
                                                              : qsTr("%1 corrections").arg(sheet.result.corrected ?? 0)) + " · "
                          + qsTr("%1 of %2 lines found").arg(sheet.result.beatsMatched ?? 0).arg(sheet.result.beats ?? 0)
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
            ListView {
                id: changeList
                objectName: "scriptCorrectionChanges"
                Layout.fillWidth: true
                Layout.preferredHeight: sheet.kept.length > 0 ? Theme.space(60) : Theme.space(84)
                clip: true
                spacing: Theme.space(1)
                model: sheet.changes
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar {}
                delegate: Rectangle {
                    id: changeRow
                    required property var modelData
                    required property int index
                    objectName: "scriptCorrectionChange_" + index
                    width: changeList.width - Theme.space(2)
                    implicitHeight: changeLine.implicitHeight + Theme.space(2)
                    radius: Theme.radiusField
                    color: changeMouse.containsMouse ? Theme.hover : Theme.panel
                    border.width: Theme.hairline
                    border.color: Theme.border
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("At %1: %2 becomes %3").arg(modelData.time).arg(modelData.before).arg(modelData.after)
                    Accessible.onPressAction: ScriptCorrection.reveal(changeRow.index, false)
                    RowLayout {
                        id: changeLine
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: Theme.space(1.5)
                        spacing: Theme.space(2)
                        Text {
                            text: changeRow.modelData.time
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            Layout.maximumWidth: (changeLine.width - Theme.space(40)) / 2
                            text: changeRow.modelData.before
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.strikeout: true
                        }
                        Text {
                            text: qsTr("→")
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                        }
                        Text {
                            Layout.fillWidth: true
                            text: changeRow.modelData.after
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                        }
                        // Capitals only ("kubernetes" → "Kubernetes").
                        Rectangle {
                            visible: changeRow.modelData.kind === "case"
                            implicitWidth: caseTag.implicitWidth + Theme.space(3)
                            implicitHeight: caseTag.implicitHeight + Theme.space(1)
                            radius: height / 2
                            color: Theme.raised
                            border.width: Theme.hairline
                            border.color: Theme.border
                            Text {
                                id: caseTag
                                anchors.centerIn: parent
                                text: qsTr("Capitals")
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                            }
                        }
                    }
                    MouseArea {
                        id: changeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: ScriptCorrection.reveal(changeRow.index, false)
                    }
                }
                EmptyState {
                    anchors.centerIn: parent
                    width: parent.width - Theme.space(4)
                    visible: changeList.count === 0 && !sheet.busy
                    illustration: "transcript"
                    text: !sheet.hasResult ? qsTr("Paste your script and find corrections: the transcript's misheard words are listed here before anything changes.")
                                           : qsTr("Nothing to correct: where your lines were found, the transcript already reads like the script.")
                }
            }
            Note {
                visible: (sheet.result.corrected ?? 0) > sheet.changes.length
                text: qsTr("The first %1 are listed; Apply makes all %2.").arg(sheet.changes.length).arg(sheet.result.corrected ?? 0)
            }

            // What the speaker said differently: not a mishearing, so not changed.
            ColumnLayout {
                objectName: "scriptCorrectionKept"
                Layout.fillWidth: true
                visible: sheet.kept.length > 0
                spacing: Theme.space(1)
                Label {
                    text: qsTr("Left as said (%n)", "", sheet.result.keptCount ?? 0)
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
                Note {
                    text: qsTr("These words do not sound like your script's, so they are what you said: they stay.")
                }
                ListView {
                    id: keptList
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(contentHeight, Theme.space(26))
                    clip: true
                    spacing: Theme.space(0.5)
                    model: sheet.kept
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThinScrollBar {}
                    delegate: Item {
                        id: keptRow
                        required property var modelData
                        required property int index
                        objectName: "scriptCorrectionKept_" + index
                        width: keptList.width - Theme.space(2)
                        implicitHeight: keptLine.implicitHeight + Theme.space(1)
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("At %1 you said %2; the script has %3").arg(modelData.time).arg(modelData.said).arg(modelData.script)
                        Accessible.onPressAction: ScriptCorrection.reveal(keptRow.index, true)
                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusField
                            color: keptMouse.containsMouse ? Theme.hover : "transparent"
                        }
                        RowLayout {
                            id: keptLine
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: Theme.space(1.5)
                            anchors.rightMargin: Theme.space(1.5)
                            spacing: Theme.space(2)
                            Text {
                                text: keptRow.modelData.time
                                color: Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textSm
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("“%1” (script: “%2”)").arg(keptRow.modelData.said).arg(keptRow.modelData.script)
                                elide: Text.ElideRight
                                color: Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                        }
                        MouseArea {
                            id: keptMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: ScriptCorrection.reveal(keptRow.index, true)
                        }
                    }
                }
            }

            // Lines that were not found closely enough: left alone.
            Note {
                objectName: "scriptCorrectionSkipped"
                visible: sheet.skipped.length > 0
                text: qsTr("Not found closely enough, so not corrected: %1")
                      .arg(sheet.skipped.slice(0, 5).map(s => qsTr("line %1 (“%2”)").arg(s.line).arg(s.text.length > 40 ? s.text.slice(0, 40) + "…" : s.text)).join(", ")
                           + (sheet.skipped.length > 5 ? qsTr(" and %n more", "", sheet.skipped.length - 5) : ""))
            }
            // Corrections it could not make: words across a cut, inside a nested or multi-camera clip, or redacted.
            Note {
                objectName: "scriptCorrectionLeftOut"
                visible: (sheet.result.leftOut ?? 0) > 0
                text: (sheet.result.leftOut ?? 0) === 1
                      ? qsTr("1 correction left out: its words cross a cut, play through a nested clip or are redacted. Fix it by hand in the Transcript tab.")
                      : qsTr("%1 corrections left out: their words cross a cut, play through a nested clip or are redacted. Fix them by hand in the Transcript tab.").arg(sheet.result.leftOut ?? 0)
            }
            // After Apply: the captions made earlier still read the old words until they are updated.
            Rectangle {
                objectName: "scriptCorrectionApplied"
                Layout.fillWidth: true
                visible: sheet.appliedCount >= 0
                implicitHeight: appliedRow.implicitHeight + Theme.space(3)
                radius: Theme.radiusField
                color: Theme.accentWash
                border.width: Theme.hairline
                border.color: Theme.accentStroke
                RowLayout {
                    id: appliedRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: Theme.space(1.5)
                    spacing: Theme.space(2)
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: (Captions.track !== "" ? (sheet.appliedCount === 1
                                                        ? qsTr("1 correction applied. Update the captions so they read the corrections too.")
                                                        : qsTr("%1 corrections applied. Update the captions so they read the corrections too.").arg(sheet.appliedCount))
                                                     : (sheet.appliedCount === 1
                                                        ? qsTr("1 correction applied. Captions you make from the transcript will read it.")
                                                        : qsTr("%1 corrections applied. Captions you make from the transcript will read them.").arg(sheet.appliedCount)))
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    PillButton {
                        objectName: "scriptCorrectionUpdateCaptions"
                        visible: Captions.track !== ""
                        compact: true
                        variant: "primary"
                        iconName: "rotate-ccw"
                        text: qsTr("Update captions")
                        tooltip: qsTr("Rewrite the captions' text from the corrected transcript; their timing stays")
                        onClicked: {
                            if (Captions.updateFromTranscript() === "")
                                sheet.close()
                        }
                    }
                }
            }
        }
    }

    footerData: [
        PillButton {
            objectName: "scriptCorrectionCancel"
            text: sheet.appliedCount >= 0 ? qsTr("Close") : qsTr("Cancel")
            onClicked: sheet.close()
        },
        PillButton {
            objectName: "scriptCorrectionApply"
            variant: "primary"
            iconName: "check"
            text: (sheet.result.corrected ?? 0) === 1 ? qsTr("Apply 1 correction")
                                                      : qsTr("Apply %1 corrections").arg(sheet.result.corrected ?? 0)
            enabled: ScriptCorrection.staged && !sheet.busy && sheet.appliedCount < 0
            onClicked: sheet.applyCorrections()
        }
    ]
}
