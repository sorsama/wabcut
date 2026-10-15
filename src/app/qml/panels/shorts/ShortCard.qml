pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// One shorts candidate (AI-9): a looping strip of frames across its range (muted: pictures only), its range, length and
// score, an editable title, the opening line, the score's breakdown as bars, the reasons, and Include / Create. Laid out
// in one column so it fits the narrow browser slot.
Rectangle {
    id: card

    required property var modelData
    required property int index
    property bool selected: true
    readonly property var thumbs: modelData.thumbs ?? []
    property int frame: 0

    signal createRequested()
    signal titleEdited(string title)

    implicitHeight: column.implicitHeight + Theme.space(4)
    radius: Theme.radiusField
    color: Theme.sunken
    border.width: Theme.hairline
    border.color: hover.hovered ? Theme.accentStroke : Theme.border
    Accessible.role: Accessible.ListItem
    Accessible.name: qsTr("Short %1: %2, %3").arg(modelData.rank).arg(modelData.title).arg(modelData.lengthText)

    component Badge: Rectangle {
        property alias text: label.text
        property alias textColor: label.color
        width: label.implicitWidth + Theme.space(2)
        height: label.implicitHeight + Theme.space(0.5)
        radius: Theme.radiusBadge
        color: Theme.scrim
        Text {
            id: label
            anchors.centerIn: parent
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
    }

    HoverHandler { id: hover }

    // The strip loops through the short's frames (still under reduced motion).
    Timer {
        interval: 700
        repeat: true
        running: card.visible && card.thumbs.length > 1 && !Theme.reduceMotion
        onTriggered: card.frame = (card.frame + 1) % card.thumbs.length
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space(2)
        spacing: Theme.space(1.5)

        // The current frame, large, with the rank, length and score on it.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(width * 9 / 16, Theme.space(48))
            radius: Theme.radiusBadge
            color: Theme.canvas
            clip: true
            Image {
                objectName: "shortPreviewFrame"
                anchors.fill: parent
                source: card.thumbs.length > 0 ? card.thumbs[card.frame % card.thumbs.length] : ""
                sourceSize: Qt.size(480, 270)
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
            }
            Badge {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: Theme.space(1)
                text: qsTr("#%1  %2").arg(card.modelData.rank).arg(card.modelData.range)
            }
            Badge {
                objectName: "shortScore"
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.space(1)
                text: qsTr("score %1").arg(Math.round(card.modelData.score * 100))
                textColor: Theme.accentStroke
            }
            Badge {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.space(1)
                text: card.modelData.lengthText
            }
        }
        Row {
            id: strip
            Layout.fillWidth: true
            spacing: Theme.space(0.5)
            readonly property real cell: card.thumbs.length > 0 ? (width - spacing * (card.thumbs.length - 1)) / card.thumbs.length : 0
            Repeater {
                model: card.thumbs
                delegate: Rectangle {
                    id: thumb
                    required property string modelData
                    required property int index
                    width: strip.cell
                    height: width * 9 / 16
                    radius: 2
                    color: Theme.canvas
                    border.width: index === card.frame ? Theme.hairline * 2 : 0
                    border.color: Theme.accentStroke
                    clip: true
                    Image {
                        anchors.fill: parent
                        anchors.margins: thumb.border.width
                        source: thumb.modelData
                        sourceSize: Qt.size(96, 54)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }
                }
            }
        }

        // The title (the hook title and the sequence name): editable.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeightSmall
            radius: Theme.radiusField
            color: Theme.raised
            border.width: Theme.hairline
            border.color: titleField.activeFocus ? Theme.accentStroke : Theme.border
            TextInput {
                id: titleField
                objectName: "shortTitleField"
                anchors.fill: parent
                anchors.leftMargin: Theme.space(2)
                anchors.rightMargin: Theme.space(2)
                verticalAlignment: TextInput.AlignVCenter
                text: card.modelData.title
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
                clip: true
                selectByMouse: true
                maximumLength: 120
                Accessible.name: qsTr("Title")
                onEditingFinished: if (text !== card.modelData.title) card.titleEdited(text)
            }
        }

        Text {
            Layout.fillWidth: true
            text: card.modelData.hook
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.italic: true
            elide: Text.ElideRight
            maximumLineCount: 2
            wrapMode: Text.Wrap
        }

        // The score's breakdown: two columns where there is room.
        GridLayout {
            Layout.fillWidth: true
            columns: card.width >= Theme.space(110) ? 2 : 1
            columnSpacing: Theme.space(3)
            rowSpacing: Theme.space(0.5)
            Repeater {
                model: card.modelData.parts ?? []
                delegate: RowLayout {
                    id: part
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    Text {
                        Layout.preferredWidth: Theme.space(16)
                        text: part.modelData.label
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Theme.space(1)
                        radius: height / 2
                        color: Theme.raised
                        Rectangle {
                            width: parent.width * Math.max(0, Math.min(1, part.modelData.value))
                            height: parent.height
                            radius: height / 2
                            color: part.modelData.value >= 0.66 ? Theme.success : part.modelData.value >= 0.4 ? Theme.accentFill : Theme.warning
                        }
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: text !== ""
            text: (card.modelData.reasons ?? []).join(" · ") + (card.modelData.cloudReason ? (" · " + card.modelData.cloudReason) : "")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Toggle {
                objectName: "shortSelectToggle"
                text: qsTr("Include")
                checked: card.selected
                onToggled: card.selected = checked
            }
            Item { Layout.fillWidth: true }
            PillButton {
                objectName: "shortCreateButton"
                compact: true
                iconName: "clapperboard"
                text: qsTr("Create")
                enabled: !Shorts.busy
                onClicked: card.createRequested()
            }
        }
    }
}
