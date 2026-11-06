import QtQuick
import QtQuick.Controls.Basic

// 0.7 (card 125): Clip > Clip Note…: write a free-text note on a timeline clip. The note is part of the clip (saved with the
// project, carried by a razor cut and a copy) and is read on the timeline when Timeline Display's "Clip notes" is on, and
// in the clip's tooltip. Enter adds a line; Ctrl+Enter (or Save) writes it as one undo step, Esc cancels, Clear removes
// the note. At most ProjectController.clipNoteLimit bytes; the counter shows from 80 % of it.
Popover {
    id: notePopover
    objectName: "clipNotePopover"

    property string clipId: ""
    /// The clip has a note when the popover opens (Clear is for that note).
    property bool hasNote: false
    readonly property int limit: ProjectController.clipNoteLimit()
    readonly property int used: ProjectController.clipNoteBytes(noteArea.text)
    readonly property bool tooLong: notePopover.used > notePopover.limit

    function openFor(id: string, point: point, maxX: real) {
        clipId = id
        x = Math.round(Math.min(maxX - width, Math.max(0, point.x)))
        y = Math.round(Math.max(0, point.y - height))
        noteArea.text = ProjectController.clipNote(id)
        hasNote = noteArea.text !== ""
        open()
        noteArea.forceActiveFocus()
        noteArea.selectAll()
    }

    function save() {
        if (notePopover.tooLong)
            return
        ProjectController.setClipNote(notePopover.clipId, noteArea.text)
        notePopover.close()
    }

    title: qsTr("Clip note")
    transformOrigin: Item.BottomLeft

    Column {
        width: Theme.space(72)
        spacing: Theme.space(2)

        Rectangle {
            width: parent.width
            height: Theme.space(26)
            radius: Theme.radiusField
            color: Theme.raised
            border.width: noteArea.activeFocus ? Theme.focusRingWidth : 0
            border.color: Theme.focus

            ScrollView {
                anchors.fill: parent
                anchors.margins: Theme.space(1)
                TextArea {
                    id: noteArea
                    objectName: "clipNoteArea"
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    color: Theme.textPrimary
                    selectionColor: Theme.accentFill
                    selectedTextColor: Theme.accentOnFill
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    placeholderText: qsTr("Write a note about this clip")
                    placeholderTextColor: Theme.textTertiary
                    background: null
                    Accessible.name: qsTr("Clip note")
                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) {
                            notePopover.save()
                            event.accepted = true
                        }
                    }
                }
            }
        }
        Text {
            objectName: "clipNoteCounter"
            visible: notePopover.used >= notePopover.limit * 0.8
            width: parent.width
            text: notePopover.tooLong ? qsTr("Too long: %1 of %2 bytes").arg(notePopover.used).arg(notePopover.limit)
                                      : qsTr("%1 of %2 bytes").arg(notePopover.used).arg(notePopover.limit)
            color: notePopover.tooLong ? Theme.dangerText : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            wrapMode: Text.Wrap
        }
        Flow {
            width: parent.width
            spacing: Theme.space(2)
            PillButton {
                objectName: "clipNoteClear"
                compact: true
                iconName: "trash-2"
                text: qsTr("Clear")
                enabled: notePopover.hasNote
                onClicked: {
                    noteArea.text = ""
                    notePopover.save()
                }
            }
            PillButton {
                objectName: "clipNoteSave"
                compact: true
                variant: "primary"
                text: qsTr("Save")
                enabled: !notePopover.tooLong
                onClicked: notePopover.save()
            }
        }
    }
}
