import QtQuick

// A key as a keycap (onboarding, M5j; design language v3): mono text on a raised, softly rounded chip with a heavier
// bottom edge. `lit` lights it with the accent and black text (the welcome's key run, the trainer's answer).
Rectangle {
    id: root

    property string text: ""
    property bool lit: false
    property bool large: false

    implicitWidth: Math.max(implicitHeight, label.implicitWidth + Theme.space(large ? 5 : 3))
    implicitHeight: large ? Theme.space(11) : Theme.space(7)
    radius: large ? Theme.radiusBadge + 6 : Theme.radiusBadge + 2
    color: lit ? Theme.accentFill : Theme.raised
    border.width: lit ? 0 : Theme.hairline
    border.color: Theme.borderStrong
    Accessible.role: Accessible.StaticText
    Accessible.name: text

    Behavior on color {
        ColorAnimation { duration: Theme.durState; easing.type: Easing.OutCubic }
    }

    // The keycap's lower lip.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.radius
        anchors.rightMargin: root.radius
        height: Theme.hairline * 2
        color: root.lit ? Theme.accentDeep : Theme.border
    }

    Text {
        id: label
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -Theme.hairline
        text: root.text
        color: root.lit ? Theme.accentOnFill : Theme.textPrimary
        font.family: Theme.fontMono
        font.pixelSize: root.large ? Theme.textTitle : Theme.textSm
        font.weight: root.lit ? Font.Bold : Font.Medium
    }
}
