pragma ComponentBehavior: Bound
import QtQuick

// 0.6 (card #74): the Program Monitor's take strip (Clip > Auditions > Show Takes in Program Monitor, or the Take
// Selector's layers button): the takes of the audition the Take Selector shows - the selected one, else the one under
// the playhead - as cards along the bottom of the picture, each with its first frame, number, name and length (a short
// take says how short). A click uses the take (one undo step, the playhead stays); the play button auditions it in
// place. Over video it uses the scrim colours, so it reads the same in the dark and light themes.
Rectangle {
    id: strip
    objectName: "takeStripOverlay"

    readonly property var takes: Auditions.takes
    readonly property real cardWidth: Math.min(Theme.space(40), (strip.width - Theme.space(2) * (strip.takes.length + 1))
                                                            / Math.max(1, strip.takes.length))

    height: header.height + Theme.space(3) + Math.round(strip.cardWidth * 9 / 16) + Theme.space(9)
    radius: Theme.radiusField
    color: Theme.scrim
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Takes of %1").arg(Auditions.name)

    Text {
        id: header
        x: Theme.space(2)
        y: Theme.space(1.5)
        width: strip.width - Theme.space(4)
        text: qsTr("%1 · %2").arg(Auditions.badge).arg(Auditions.name)
        elide: Text.ElideRight
        color: Theme.scrimText
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
    }

    Row {
        id: cards
        x: Theme.space(2)
        anchors.top: header.bottom
        anchors.topMargin: Theme.space(1.5)
        spacing: Theme.space(2)

        Repeater {
            model: strip.takes
            delegate: Item {
                id: card

                required property var modelData
                readonly property bool active: modelData.active === true

                objectName: "takeCard_" + modelData.index
                width: strip.cardWidth
                height: frame.height + label.height + Theme.space(1)
                Accessible.role: Accessible.Button
                Accessible.name: card.active ? qsTr("Take %1, %2, active").arg(modelData.index + 1).arg(modelData.name)
                                             : qsTr("Use take %1, %2").arg(modelData.index + 1).arg(modelData.name)
                Accessible.onPressAction: Auditions.useTake(card.modelData.index)

                Rectangle {
                    id: frame
                    width: parent.width
                    height: Math.round(width * 9 / 16)
                    radius: Theme.radiusField / 2
                    color: Qt.rgba(1, 1, 1, 0.08)
                    border.width: card.active ? 2 : Theme.hairline
                    border.color: card.active ? Theme.accentStroke : (hover.hovered ? Theme.scrimText : Theme.scrimTextMuted)
                    clip: true
                    Image {
                        id: picture
                        anchors.fill: parent
                        anchors.margins: card.active ? 2 : 1
                        source: card.modelData.thumbnail
                        asynchronous: true
                        fillMode: Image.PreserveAspectCrop
                        visible: status === Image.Ready
                    }
                    Icon {
                        anchors.centerIn: parent
                        visible: !picture.visible
                        name: card.modelData.kind === "audio" ? "audio-lines" : "film"
                        size: Theme.iconMd
                        color: Theme.scrimTextMuted
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: Theme.space(1)
                        width: number.implicitWidth + Theme.space(2)
                        height: number.implicitHeight + Theme.space(0.5)
                        radius: height / 2
                        color: card.active ? Theme.accentFill : Theme.scrim
                        Text {
                            id: number
                            anchors.centerIn: parent
                            text: card.modelData.index + 1
                            color: card.active ? Theme.accentOnFill : Theme.scrimText
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                            font.weight: Font.DemiBold
                        }
                    }
                    IconButton {
                        id: playButton
                        objectName: "takeCardPlay_" + card.modelData.index
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: Theme.space(1)
                        compact: true
                        variant: "primary"
                        visible: hover.hovered || Auditions.previewing === card.modelData.index
                        iconName: Auditions.previewing === card.modelData.index ? "pause" : "play"
                        tooltip: qsTr("Play: audition this take in place")
                        Accessible.name: Auditions.previewing === card.modelData.index
                                         ? qsTr("Stop take %1: %2").arg(card.modelData.index + 1).arg(card.modelData.name)
                                         : qsTr("Play take %1: %2").arg(card.modelData.index + 1).arg(card.modelData.name)
                        onClicked: Auditions.previewing === card.modelData.index ? Auditions.stopPreview()
                                                                                  : Auditions.previewTake(card.modelData.index)
                    }
                }
                Column {
                    id: label
                    anchors.top: frame.bottom
                    anchors.topMargin: Theme.space(1)
                    width: parent.width
                    Text {
                        width: parent.width
                        text: card.modelData.name
                        elide: Text.ElideRight
                        color: Theme.scrimText
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        font.weight: card.active ? Font.DemiBold : Font.Normal
                    }
                    Text {
                        width: parent.width
                        text: card.modelData.isShort ? qsTr("%1 · %2").arg(card.modelData.durationText).arg(card.modelData.shortText)
                                                     : card.modelData.durationText
                        elide: Text.ElideRight
                        color: card.modelData.isShort ? Theme.warning : Theme.scrimTextMuted
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                }

                HoverHandler {
                    id: hover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        if (!playButton.hovered) // the play button auditions instead
                            Auditions.useTake(card.modelData.index)
                    }
                }
            }
        }
    }
}
