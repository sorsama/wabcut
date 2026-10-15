pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects

// A project's picture on Home (design language v3): its thumbnail when it has one, else a card in one of the six
// hues with a frame and a little timeline; rounded corners clip both. RecentProjects and ContinueCard show it.
Item {
    id: thumbRoot

    property url source: ""
    property string hueName: "violet"
    property int seed: 1
    /// A mono chip over the top-left corner (the format); "" for none.
    property string chip: ""
    property real cornerRadius: Theme.radiusCard - Theme.space(0.5)
    /// The frame and the timeline band (off for small list thumbnails).
    property bool detailed: true
    readonly property bool hasImage: picture.status === Image.Ready

    Item {
        id: art
        anchors.fill: parent
        layer.enabled: true
        visible: false

        Rectangle {
            anchors.fill: parent
            color: Theme.hue(thumbRoot.hueName)
        }
        Rectangle {
            visible: thumbRoot.detailed
            anchors.horizontalCenter: parent.horizontalCenter
            y: Math.round(parent.height * 0.12)
            height: Math.round(parent.height * 0.48)
            width: Math.round(height * 16 / 9)
            radius: Theme.space(2)
            color: "transparent"
            border.width: Theme.hairline * 2
            border.color: Qt.alpha(Theme.clipInk, 0.3)
        }
        Rectangle {
            id: band
            visible: thumbRoot.detailed
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Math.round(parent.height * 0.3)
            color: Qt.alpha(Theme.clipInk, 0.78)

            readonly property real bar: Math.max(3, Math.round(height * 0.2))
            Column {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space(2.5)
                anchors.rightMargin: Theme.space(2.5)
                spacing: Math.max(2, Math.round(band.bar * 0.6))
                Row {
                    spacing: 3
                    Repeater {
                        model: 4
                        delegate: Rectangle {
                            required property int index
                            width: band.width * (0.14 + Math.abs(Math.sin(thumbRoot.seed * 9.1 + index * 3.7)) * 0.16)
                            height: band.bar
                            radius: height / 2
                            color: Theme.contentWhite
                        }
                    }
                }
                Row {
                    spacing: 3
                    Item {
                        width: band.width * Math.abs(Math.sin(thumbRoot.seed + 3)) * 0.08
                        height: band.bar
                    }
                    Repeater {
                        model: 2
                        delegate: Rectangle {
                            required property int index
                            width: band.width * (0.28 + Math.abs(Math.sin(thumbRoot.seed * 9.1 + (index + 7) * 3.7)) * 0.26)
                            height: band.bar
                            radius: height / 2
                            color: index === 0 ? Theme.huePink : Theme.hueMint
                        }
                    }
                }
            }
        }
        Image {
            id: picture
            anchors.fill: parent
            source: thumbRoot.source
            sourceSize.height: Theme.mediaThumbnailHeight
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            visible: status === Image.Ready
        }
    }
    Item {
        id: mask
        anchors.fill: parent
        layer.enabled: true
        visible: false
        Rectangle {
            anchors.fill: parent
            radius: thumbRoot.cornerRadius
        }
    }
    MultiEffect {
        anchors.fill: parent
        source: art
        maskEnabled: true
        maskSource: mask
        maskThresholdMin: 0.5
        maskSpreadAtMin: 1.0
    }
    Rectangle {
        visible: thumbRoot.chip !== ""
        x: Theme.space(2)
        y: Theme.space(2)
        implicitWidth: chipText.implicitWidth + Theme.space(4)
        implicitHeight: Theme.space(5.5)
        radius: height / 2
        color: Theme.clipInk
        Text {
            id: chipText
            anchors.centerIn: parent
            text: thumbRoot.chip
            color: Theme.contentWhite
            font.family: Theme.fontMono
            font.pixelSize: Theme.textMicro
            font.weight: Font.DemiBold
        }
    }
}
