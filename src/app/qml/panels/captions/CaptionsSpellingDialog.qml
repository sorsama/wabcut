pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Check Spelling (0.2, spell check in captions; Spelling, Captions.spellingIssues): steps through the misspelled words
// of the shown caption track in its language. For each: the caption with the word marked, the suggestions, and Change
// (this one), Change All (every one in the track, one undo step), Ignore (go on), Ignore All (until the app quits) and
// Add to Dictionary. The playhead goes to each caption in turn.
WabcutDialog {
    id: dialog
    objectName: "captionsSpellingDialog"
    title: qsTr("Check Spelling")

    /// The track's misspellings, checked again after each change (the text moves).
    property var issues: []
    property int current: 0
    /// Words ignored once (Ignore) in this run of the dialog.
    property var skipped: []
    readonly property var issue: issues[current] ?? ({})
    readonly property bool done: issues.length === 0
    property string chosen: ""

    function show() {
        skipped = []
        refresh()
        open()
    }
    /// Checks again and shows the first misspelling not skipped.
    function refresh() {
        const all = Captions.spellingIssues()
        issues = all.filter(i => skipped.indexOf(i.cue + ":" + i.start + ":" + i.word) < 0)
        current = 0
        chosen = issues.length > 0 && issues[0].suggestions.length > 0 ? issues[0].suggestions[0] : ""
        changeField.text = chosen
        if (issues.length > 0)
            Captions.seekToCue(issues[0].cue)
    }
    function marked(i) {
        if (i.text === undefined)
            return ""
        const esc = s => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        return esc(i.text.slice(0, i.start)) + "<b><u>" + esc(i.text.slice(i.start, i.end)) + "</u></b>" + esc(i.text.slice(i.end))
    }
    function change() {
        if (dialog.done)
            return
        const error = Captions.changeWord(issue.cue, issue.start, issue.end, issue.action === "delete" ? "" : changeField.text)
        status.text = error
        refresh()
    }
    function changeAll() {
        if (dialog.done)
            return
        const error = Captions.changeAll(issue.word, changeField.text)
        status.text = error === "" ? Captions.message : error
        refresh()
    }
    function ignore() {
        if (dialog.done)
            return
        skipped = skipped.concat([issue.cue + ":" + issue.start + ":" + issue.word])
        refresh()
    }

    Text {
        Layout.fillWidth: true
        visible: !Spelling.available
        wrapMode: Text.Wrap
        text: qsTr("No dictionary is installed for this language: add one in Windows Settings > Time & language > Language & region.")
        color: Theme.warning
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    Text {
        objectName: "spellingDone"
        Layout.fillWidth: true
        visible: dialog.done
        wrapMode: Text.Wrap
        text: qsTr("No misspelled words in this caption track.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: !dialog.done
        spacing: Theme.space(2)
        Text {
            objectName: "spellingCounter"
            Layout.fillWidth: true
            text: qsTr("Caption %1 · %n misspelled word(s) left", "", dialog.issues.length).arg((dialog.issue.cue ?? 0) + 1)
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: context.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.border
            Text {
                id: context
                objectName: "spellingContext"
                anchors.fill: parent
                anchors.margins: Theme.space(1.5)
                textFormat: Text.StyledText
                wrapMode: Text.Wrap
                text: dialog.marked(dialog.issue)
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                Accessible.name: qsTr("Misspelled word %1 in: %2").arg(dialog.issue.word ?? "").arg(dialog.issue.text ?? "")
            }
        }
        FieldInput {
            id: changeField
            objectName: "spellingChangeTo"
            Layout.fillWidth: true
            placeholderText: dialog.issue.action === "delete" ? qsTr("Delete the repeated word") : qsTr("Change to")
            enabled: dialog.issue.action !== "delete"
            Accessible.name: qsTr("Change to")
            onAccepted: dialog.change()
            Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Repeater {
                model: dialog.issue.suggestions ?? []
                delegate: PillButton {
                    required property var modelData
                    required property int index
                    objectName: "spellingPick_" + index
                    compact: true
                    variant: changeField.text === modelData ? "primary" : "secondary"
                    text: modelData
                    tooltip: qsTr("Change to %1").arg(modelData)
                    onClicked: changeField.text = modelData
                }
            }
            Text {
                visible: (dialog.issue.suggestions ?? []).length === 0 && dialog.issue.action !== "delete"
                text: qsTr("No suggestions")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            PillButton {
                objectName: "spellingIgnore"
                compact: true
                variant: "ghost"
                text: qsTr("Ignore")
                onClicked: dialog.ignore()
            }
            PillButton {
                objectName: "spellingIgnoreAll"
                compact: true
                variant: "ghost"
                text: qsTr("Ignore All")
                onClicked: {
                    Spelling.ignoreWord(dialog.issue.word)
                    dialog.refresh()
                }
            }
            PillButton {
                objectName: "spellingAdd"
                compact: true
                variant: "ghost"
                text: qsTr("Add to Dictionary")
                onClicked: {
                    if (!Spelling.addWord(dialog.issue.word))
                        status.text = qsTr("Could not add \"%1\" to the dictionary").arg(dialog.issue.word)
                    dialog.refresh()
                }
            }
            PillButton {
                objectName: "spellingChangeAll"
                compact: true
                variant: "secondary"
                text: qsTr("Change All")
                enabled: dialog.issue.action !== "delete" && changeField.text !== ""
                onClicked: dialog.changeAll()
            }
        }
    }
    Text {
        id: status
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Close")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "spellingChange"
            variant: "primary"
            text: dialog.issue.action === "delete" ? qsTr("Delete") : qsTr("Change")
            enabled: !dialog.done && (dialog.issue.action === "delete" || changeField.text !== "")
            onClicked: dialog.change()
        }
    ]
}
