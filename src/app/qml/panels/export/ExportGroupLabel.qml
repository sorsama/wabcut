import QtQuick

// A group's label in the Export panel's preset grid (design language v3): the destination's hue dot, its name as a
// small caps tag and how many presets it holds, e.g. "● YOUTUBE 3". A heading for screen readers.
Item {
    id: root

    property string text: ""
    property color hue: Theme.accentStroke
    property int count: 0

    implicitWidth: row.implicitWidth
    implicitHeight: Theme.space(5)

    Accessible.role: Accessible.Heading
    Accessible.name: root.text

    Row {
        id: row
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, root.width)
        spacing: Theme.space(2)

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.dotSize
            height: Theme.dotSize
            radius: width / 2
            color: root.hue
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, root.width - Theme.dotSize - countText.implicitWidth - Theme.space(4))
            text: root.text
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMicro + 1
            font.weight: Theme.weightTitle
            font.capitalization: Font.AllUppercase
            font.letterSpacing: Theme.microTracking * 1.5
        }
        Text {
            id: countText
            anchors.verticalCenter: parent.verticalCenter
            visible: root.count > 0
            text: root.count
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
    }
}
