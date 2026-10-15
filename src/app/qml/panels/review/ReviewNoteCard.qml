pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// One review note (M5m): its timecode (a click moves the playhead there), author and time, a mark badge when it points
// at a spot of the frame, the text, the thread of replies and a reply field; resolve / reopen and delete. A resolved
// note is dimmed with a green "Resolved" tag; the note at the playhead and the selected one are outlined.
Rectangle {
    id: card

    /// A row of Review.notes.
    required property var note
    readonly property int noteId: note.id ?? 0
    readonly property bool selected: Review.selectedNote === noteId
    readonly property bool atPlayhead: Review.currentNote === noteId

    objectName: "reviewNote_" + noteId
    implicitHeight: column.implicitHeight + Theme.space(4)
    radius: Theme.radiusCard
    color: selected ? Theme.accentWash : Theme.sunken
    border.width: selected || atPlayhead ? Theme.selectionWidth : Theme.hairline
    border.color: selected ? Theme.accentStroke : atPlayhead ? Theme.borderStrong : Theme.border

    Accessible.role: Accessible.ListItem
    Accessible.name: qsTr("Note at %1 by %2: %3").arg(note.startText ?? "").arg(note.author ?? "").arg(note.text ?? "")
    Accessible.selected: selected
    Accessible.onPressAction: Review.selectedNote = card.noteId

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        onTapped: Review.selectedNote = card.noteId
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space(2)
        spacing: Theme.space(1.5)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1.5)

            PillButton {
                objectName: "reviewNoteTime_" + card.noteId
                compact: true
                variant: "ghost"
                iconName: card.note.hasMark ? "crosshair" : ""
                text: card.note.range ? qsTr("%1–%2").arg(card.note.startText).arg(card.note.endText) : (card.note.startText ?? "")
                tooltip: card.note.hasMark ? qsTr("Go to the note (it marks a spot on the frame)") : qsTr("Go to the note")
                font.family: Theme.fontMono
                onClicked: Review.seekToNote(card.noteId)
            }
            Text {
                Layout.fillWidth: true
                text: (card.note.author ?? "") + ((card.note.createdText ?? "") !== "" ? "  ·  " + card.note.createdText : "")
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            StatusPill {
                visible: card.note.resolved === true
                label: qsTr("Resolved")
                dotColor: Theme.success
            }
            IconButton {
                objectName: "reviewNoteResolve_" + card.noteId
                compact: true
                iconName: card.note.resolved ? "rotate-ccw" : "check"
                tooltip: card.note.resolved ? qsTr("Reopen") : qsTr("Resolve")
                onClicked: Review.resolve(card.noteId, !card.note.resolved)
            }
            IconButton {
                objectName: "reviewNoteDelete_" + card.noteId
                compact: true
                iconName: "trash-2"
                tooltip: qsTr("Delete the note")
                onClicked: Review.remove(card.noteId)
            }
        }

        Text {
            Layout.fillWidth: true
            visible: Review.allSequences
            text: card.note.sequenceName ?? ""
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }

        Text {
            objectName: "reviewNoteText"
            Layout.fillWidth: true
            text: card.note.text ?? ""
            wrapMode: Text.Wrap
            color: card.note.resolved ? Theme.textSecondary : Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }

        // The thread.
        Repeater {
            model: card.note.replies ?? []
            delegate: RowLayout {
                id: replyRow
                required property var modelData
                Layout.fillWidth: true
                spacing: Theme.space(1.5)
                Rectangle {
                    Layout.fillHeight: true
                    implicitWidth: Theme.hairline * 2
                    color: Theme.borderStrong
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        Layout.fillWidth: true
                        text: (replyRow.modelData.author ?? "") + ((replyRow.modelData.createdText ?? "") !== ""
                                                                    ? "  ·  " + replyRow.modelData.createdText : "")
                        elide: Text.ElideRight
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: replyRow.modelData.text ?? ""
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }
            }
        }

        FieldInput {
            id: replyField
            objectName: "reviewNoteReply_" + card.noteId
            Layout.fillWidth: true
            visible: card.selected
            placeholderText: qsTr("Reply…")
            Accessible.name: qsTr("Reply to the note")
            onAccepted: {
                if (Review.reply(card.noteId, text) === "")
                    text = ""
            }
        }
    }
}
