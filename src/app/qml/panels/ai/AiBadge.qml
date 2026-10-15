import QtQuick

// Small rounded label for model features ("Tools", "Video", "Speakers") and states ("Unverified").
Rectangle {
    id: root

    property string text: ""
    property bool warning: false

    implicitWidth: label.implicitWidth + Theme.space(4)
    implicitHeight: label.implicitHeight + Theme.space(1)
    radius: height / 2
    color: warning ? Qt.alpha(Theme.warning, 0.16) : Theme.hover
    border.width: Theme.hairline
    border.color: warning ? Qt.alpha(Theme.warning, 0.5) : Theme.border

    Accessible.role: Accessible.StaticText
    Accessible.name: root.text

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: root.warning ? Theme.warning : Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Font.DemiBold
    }
}
