pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// p02 audio: the voice-over take recovery prompt. When a project opens with takes a crash (or a quit before they were
// placed) left in its Voice-over folder (VoiceOver.recoveredTakes, scanned read-only), a card offers each one:
// Recover repairs the take and puts it back on its track where it was recorded (one undo step); Discard stops offering
// them (the files stay in the folder). Non-modal, in the bottom right corner.
Item {
    id: root

    readonly property var takes: VoiceOver.recoveredTakes ?? []

    Rectangle {
        id: card
        objectName: "takeRecoveryPrompt"
        visible: root.takes.length > 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space(4)
        width: Math.min(Theme.space(110), parent.width - Theme.space(8))
        implicitHeight: content.implicitHeight + Theme.space(6)
        height: implicitHeight
        radius: Theme.radiusPanel
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.accentStroke

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: Theme.space(3)
            spacing: Theme.space(2)

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Icon {
                    name: "mic"
                    size: Theme.iconSm
                    color: Theme.accentStroke
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Recover voice-over takes")
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("These takes were recorded but not placed (the app closed while recording, or before the take was kept).")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Repeater {
                model: root.takes
                delegate: RowLayout {
                    id: row
                    required property var modelData
                    required property int index
                    objectName: "recoveredTake_" + index
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            elide: Text.ElideMiddle
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("%1 at %2 · %3 s%4").arg(row.modelData.track).arg(row.modelData.startText)
                                  .arg(Number(row.modelData.seconds).toFixed(1))
                                  .arg(row.modelData.interrupted ? qsTr(" · interrupted") : "")
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }
                    PillButton {
                        objectName: "recoverTake"
                        compact: true
                        variant: "primary"
                        text: qsTr("Recover")
                        enabled: row.modelData.placeable === true
                        tooltip: row.modelData.placeable === true ? qsTr("Repair the take and put it back where it was recorded")
                                                                 : qsTr("Its track or sequence is gone: import it from the Voice-over folder")
                        onClicked: recoverMessage.text = VoiceOver.placeRecoveredTake(row.modelData.path)
                    }
                }
            }
            Text {
                id: recoverMessage
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.Wrap
                color: Theme.danger
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                PillButton {
                    objectName: "discardTakes"
                    compact: true
                    variant: "ghost"
                    text: qsTr("Discard")
                    tooltip: qsTr("Stop offering these takes (the files stay in the project's Voice-over folder)")
                    onClicked: VoiceOver.dismissRecoveredTakes()
                }
            }
        }
    }
}
