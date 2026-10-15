import QtQuick
import QtQuick.Controls.Basic

// Rename a timeline clip: a popover with the name selected; Enter renames (one undo step), Esc cancels.
Popover {
    id: renamePopover
    objectName: "renameClipPopover"

    property string clipId: ""

    function openFor(id, point, maxX) {
        clipId = id
        x = Math.round(Math.min(maxX - width, Math.max(0, point.x)))
        y = Math.round(Math.max(0, point.y - height))
        nameField.text = ProjectController.clipName(id)
        open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }

    title: qsTr("Rename clip")
    transformOrigin: Item.BottomLeft

    TextField {
        id: nameField
        objectName: "clipNameField"
        width: Theme.space(56)
        padding: Theme.space(2)
        leftPadding: Theme.space(3.5)
        rightPadding: Theme.space(3.5)
        color: Theme.textPrimary
        selectionColor: Theme.accentFill
        selectedTextColor: Theme.accentOnFill
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        Accessible.name: qsTr("Clip name")
        background: Rectangle {
            radius: height / 2
            color: Theme.raised
            border.width: nameField.activeFocus ? Theme.focusRingWidth : 0
            border.color: Theme.focus
        }
        onAccepted: {
            ProjectController.renameClip(renamePopover.clipId, text)
            renamePopover.close()
        }
    }
}
