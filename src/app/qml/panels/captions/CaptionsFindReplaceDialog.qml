pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Find and Replace (0.2, Premiere's Captions / Text panel): in the shown caption track, every caption track or the
// transcript, with Match Case and Whole Word. The arrows step through the matches (captions.find, transcript.find):
// the caption is picked and the playhead goes to it. Replace changes the match shown and moves on to the next;
// Replace All changes every match. Each replacement is one undo step (captions.findReplace, transcript.findReplace
// through Captions).
WabcutDialog {
    id: dialog
    objectName: "captionsFindReplaceDialog"
    title: qsTr("Find and Replace")

    readonly property var scopes: ["track", "all", "transcript"]
    readonly property string scope: scopes[scopeTabs.currentIndex]
    /// The matches of the current search and the one shown (-1: none yet).
    property var matches: []
    property int current: -1

    function show() {
        errorText.text = ""
        matches = []
        current = -1
        open()
        findField.forceActiveFocus()
        findField.selectAll()
    }
    function search() {
        matches = findField.text !== "" ? Captions.findMatches(findField.text, matchCase.checked, wholeWord.checked, scope) : []
    }
    /// Steps to the next (+1) or previous (-1) match, wrapping round, and shows it.
    function step(direction) {
        search()
        if (matches.length === 0) {
            current = -1
            errorText.text = findField.text === "" ? "" : qsTr("\"%1\" was not found").arg(findField.text)
            return
        }
        current = current < 0 ? (direction > 0 ? 0 : matches.length - 1)
                              : (current + direction + matches.length) % matches.length
        reveal()
    }
    function reveal() {
        const m = matches[current]
        if (m === undefined)
            return
        errorText.text = qsTr("%1 of %2 · %3").arg(current + 1).arg(matches.length).arg(m.time)
        if (m.cue !== undefined) {
            if (m.trackKey !== undefined)
                Captions.track = m.trackKey
            Captions.seekToCue(m.cue)
        }
    }
    function replaceOne() {
        if (current < 0 || current >= matches.length) {
            step(1)
            return
        }
        const keep = current
        const error = Captions.replaceMatch(findField.text, replaceField.text, matchCase.checked, wholeWord.checked, scope,
                                            matches[current])
        if (error !== "") {
            errorText.text = error
            return
        }
        // The replaced match is gone: the one after it is now at the same place in the list.
        search()
        if (matches.length === 0) {
            current = -1
            errorText.text = Captions.message
            return
        }
        current = Math.min(keep, matches.length - 1)
        reveal()
    }
    function replaceAll() {
        const error = Captions.findReplace(findField.text, replaceField.text, matchCase.checked, wholeWord.checked, scope)
        errorText.text = error === "" ? Captions.message : error
        matches = []
        current = -1
    }

    FieldInput {
        id: findField
        objectName: "captionsFindField"
        Layout.fillWidth: true
        placeholderText: qsTr("Find")
        Accessible.name: qsTr("Find")
        onTextEdited: dialog.current = -1
        onAccepted: dialog.step(1)
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
    }
    FieldInput {
        id: replaceField
        objectName: "captionsReplaceField"
        Layout.fillWidth: true
        placeholderText: qsTr("Replace with")
        Accessible.name: qsTr("Replace with")
        onAccepted: dialog.replaceOne()
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
    }
    PillTabs {
        id: scopeTabs
        objectName: "captionsFindScope"
        Layout.fillWidth: true
        compact: true
        model: [qsTr("This track"), qsTr("All captions"), qsTr("Transcript")]
        Accessible.name: qsTr("Search in")
        onActivated: dialog.current = -1
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        Toggle {
            id: matchCase
            objectName: "captionsFindMatchCase"
            text: qsTr("Match Case")
            onToggled: dialog.current = -1
        }
        Toggle {
            id: wholeWord
            objectName: "captionsFindWholeWord"
            text: qsTr("Whole Word")
            onToggled: dialog.current = -1
        }
        Item { Layout.fillWidth: true; Layout.preferredHeight: 1 }
        IconButton {
            objectName: "captionsFindPrevious"
            compact: true
            iconName: "chevron-up"
            enabled: findField.text !== ""
            tooltip: qsTr("Find Previous")
            onClicked: dialog.step(-1)
        }
        IconButton {
            objectName: "captionsFindNext"
            compact: true
            iconName: "chevron-down"
            enabled: findField.text !== ""
            tooltip: qsTr("Find Next")
            onClicked: dialog.step(1)
        }
    }
    Text {
        id: errorText
        objectName: "captionsFindStatus"
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
            objectName: "captionsReplaceOne"
            variant: "secondary"
            text: qsTr("Replace")
            enabled: findField.text !== ""
            onClicked: dialog.replaceOne()
        },
        PillButton {
            objectName: "captionsReplaceAll"
            variant: "primary"
            text: qsTr("Replace All")
            enabled: findField.text !== ""
            onClicked: dialog.replaceAll()
        }
    ]
}
