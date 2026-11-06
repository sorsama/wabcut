import QtQuick

// A small read-only tag of the local model library (design language v3: a raised pill with a hairline edge, or the
// accent fill with black text for "accent"): a model's quantization, size, licence, "Installed", "Recommended", ...
// `tone` colours the edge ("success", "warning", "danger") so meaning never rests on colour alone: the text says it.
// `dot` adds a status dot before the text.
Rectangle {
    id: tag

    property string text: ""
    property string tone: "neutral"
    property color dot: "transparent"

    implicitWidth: row.implicitWidth + Theme.space(4)
    implicitHeight: Math.max(Theme.space(5.5), label.implicitHeight + Theme.space(1.5))
    radius: height / 2
    color: tag.tone === "accent" ? Theme.accentFill : Theme.raised
    border.width: tag.tone === "accent" ? 0 : Theme.hairline
    border.color: tag.tone === "success" ? Theme.success
                : tag.tone === "warning" ? Theme.warning
                : tag.tone === "danger" ? Theme.danger
                : Theme.border
    Accessible.role: Accessible.StaticText
    Accessible.name: tag.text

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.space(1)

        Rectangle {
            visible: tag.dot.a > 0
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.dotSize
            height: Theme.dotSize
            radius: width / 2
            color: tag.dot
        }

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: tag.text
            color: tag.tone === "accent" ? Theme.accentOnFill : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.DemiBold
        }
    }
}
