import QtQuick

// Panel section header (design language v3): a round dot and a left-aligned bold title, sentence case, e.g.
// "● Motion". The dot takes the section's hue (accent by default).
Item {
    id: root

    property string text: ""
    property color dotColor: Theme.accentStroke

    implicitWidth: row.implicitWidth
    implicitHeight: Theme.sectionHeaderHeight

    Accessible.role: Accessible.Heading
    Accessible.name: root.text

    Row {
        id: row
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space(2)

        SliceMark {
            anchors.verticalCenter: parent.verticalCenter
            color: root.dotColor
        }

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: Theme.textPrimary
            font.family: Theme.fontDisplay
            font.pixelSize: Theme.textMd
            font.weight: Theme.weightLabel
        }
    }
}
