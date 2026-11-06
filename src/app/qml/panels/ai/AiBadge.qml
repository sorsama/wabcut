import QtQuick

// Small rounded label for model features ("Tools", "Video", "Speakers"), states ("Unverified") and, with `iconName`,
// a provider's capabilities ("Voice" with its icon; r07).
Rectangle {
    id: root

    property string text: ""
    property bool warning: false
    property string iconName: ""

    implicitWidth: row.implicitWidth + Theme.space(4)
    implicitHeight: Math.max(label.implicitHeight, glyph.visible ? glyph.size : 0) + Theme.space(1)
    radius: height / 2
    color: warning ? Qt.alpha(Theme.warning, 0.16) : Theme.hover
    border.width: Theme.hairline
    border.color: warning ? Qt.alpha(Theme.warning, 0.5) : Theme.border

    Accessible.role: Accessible.StaticText
    Accessible.name: root.text

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.space(1)

        Icon {
            id: glyph
            anchors.verticalCenter: parent.verticalCenter
            visible: root.iconName !== ""
            name: root.iconName
            size: Theme.iconXs - 2
            color: root.warning ? Theme.warning : Theme.textSecondary
        }
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: root.warning ? Theme.warning : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.DemiBold
        }
    }
}
