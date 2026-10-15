import QtQuick

// One setting (0.1.3 Settings window): its label and an optional description on the left, the control(s) on the
// right. Rows after the first in a section draw a hairline above themselves.
Item {
    id: row

    property string label: ""
    property string description: ""
    default property alias controls: slot.data

    width: parent ? parent.width : implicitWidth
    implicitHeight: Math.max(texts.implicitHeight, slot.implicitHeight) + Theme.space(5)

    Rectangle {
        visible: row.y > 0
        width: parent.width
        height: Theme.hairline
        color: Theme.border
    }

    Column {
        id: texts
        anchors.left: parent.left
        anchors.right: slot.left
        anchors.rightMargin: Theme.space(4)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space(0.5)

        Text {
            width: parent.width
            text: row.label
            wrapMode: Text.WordWrap
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        Text {
            width: parent.width
            visible: row.description !== ""
            text: row.description
            wrapMode: Text.WordWrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    Row {
        id: slot
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space(2)
    }
}
