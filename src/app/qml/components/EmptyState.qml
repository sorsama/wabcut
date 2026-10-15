import QtQuick

// An empty state (design language v3): a line illustration with violet and lime accents (resources/illustrations), one line
// of text and one primary action. `illustration`: "library", "timeline", "transcript", "search", "effects" or
// "markers". Every panel shows its empty and no-match states with it (M5j).
Column {
    id: root

    property string illustration: ""
    property string text: ""
    property string actionText: ""
    property string actionIcon: ""
    signal actionTriggered()

    spacing: Theme.space(3)

    SvgArt {
        objectName: "emptyStateIllustration"
        visible: root.illustration !== "" // compact (palettes, lists): the line only
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(Theme.space(40), root.width)
        height: width * 0.75
        source: root.illustration !== "" ? "illustrations/empty-" + root.illustration + ".svg" : ""
        tint: Theme.textTertiary
    }
    Text {
        width: root.width
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: root.text
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    PillButton {
        anchors.horizontalCenter: parent.horizontalCenter
        visible: root.actionText !== ""
        variant: "primary"
        compact: true
        text: root.actionText
        iconName: root.actionIcon
        onClicked: root.actionTriggered()
    }
}
