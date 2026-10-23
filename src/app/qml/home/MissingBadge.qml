import QtQuick

// "Missing" on a project whose file is gone (design language v3: a black chip with an amber warning glyph).
Rectangle {
    id: badge

    objectName: "homeMissingBadge"
    implicitWidth: missingRow.implicitWidth + Theme.space(3)
    implicitHeight: Theme.space(5.5)
    radius: height / 2
    color: Theme.clipInk
    Accessible.role: Accessible.StaticText
    Accessible.name: qsTr("Missing")

    Row {
        id: missingRow
        anchors.centerIn: parent
        spacing: Theme.space(1)
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: "triangle-alert"
            size: Theme.iconXs
            color: Theme.warning
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Missing")
            color: Theme.contentWhite
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Theme.weightLabel
        }
    }
}
