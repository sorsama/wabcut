pragma ComponentBehavior: Bound
import QtQuick

// 0.6 (P1 round, #36): the menu of a paragraph's speaker on the Transcript tab (its name, a button): Rename or recolour…
// (the name and colour row opens in the paragraph once the menu has closed), This paragraph is ▸ (a project speaker, or
// New speaker…: transcript.assignSpeaker over the paragraph's words) and Merge into ▸ (transcript.mergeSpeakers: the
// speaker becomes the chosen one everywhere). One undo step each (Transcript). Premiere: Text > Transcript > the speaker
// menu (Edit Speakers). Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "transcriptSpeakerMenu"

    /// The paragraph (a row of Transcript.paragraphs) and its speaker key ("" for a label nobody named yet).
    property int paragraph: -1
    property string speakerKey: ""
    property string speakerName: ""
    /// "rename" or "new" once the menu has closed: the host opens the paragraph's name row.
    property string pending: ""

    /// The host opens the name row of paragraph `paragraph` for `mode` ("rename": its speaker's name and colour; "new":
    /// a new speaker for its words).
    signal editRequested(int paragraph, string mode)

    function openFor(target: Item, paragraphIndex: int, key: string, name: string) {
        menu.paragraph = paragraphIndex
        menu.speakerKey = key
        menu.speakerName = name
        menu.pending = ""
        popup(target, 0, target.height + Theme.space(1))
    }

    onClosed: {
        if (menu.pending !== "")
            menu.editRequested(menu.paragraph, menu.pending)
        menu.pending = ""
    }

    ContextMenuItem {
        objectName: "transcriptSpeakerRename"
        text: qsTr("Rename or recolour…")
        iconName: "pencil"
        onTriggered: menu.pending = "rename"
    }
    SpeakerChoiceMenu {
        id: assignMenu
        objectName: "transcriptSpeakerAssign"
        title: qsTr("This paragraph is")
        current: menu.speakerKey
        onChosen: key => Transcript.assignParagraph(menu.paragraph, key, "")
        onNewRequested: menu.pending = "new"
    }
    SpeakerChoiceMenu {
        id: mergeMenu
        objectName: "transcriptSpeakerMerge"
        title: qsTr("Merge %1 into").arg(menu.speakerName)
        excluded: menu.speakerKey
        offerNew: false
        enabled: mergeMenu.offered.length > 0
        onChosen: key => Transcript.mergeParagraphSpeaker(menu.paragraph, key)
    }
}
