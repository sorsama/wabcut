pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Preferences > AI > Models: which model runs each capability. Rows show the provider, the price per unit, the
// features that matter for editing (tools, images, native video; word timing, speakers) and why a model cannot be
// used right now (no key, offline). Transcription defaults to this computer (whisper.cpp).
Item {
    id: page

    component ModelRow: Rectangle {
        id: row

        required property var modelData
        property bool selected: false
        signal chosen()

        width: ListView.view ? ListView.view.width : implicitWidth
        implicitHeight: rowContent.implicitHeight + Theme.space(5)
        radius: Theme.radiusField
        color: rowMouse.containsMouse ? Theme.hover : Theme.raised
        border.width: row.selected ? Theme.focusRingWidth : Theme.hairline
        border.color: row.selected ? Theme.accentStroke : Theme.border
        opacity: row.modelData.usable === false && !row.selected ? 0.7 : 1

        Accessible.role: Accessible.RadioButton
        Accessible.name: row.modelData.name
        Accessible.checked: row.selected
        activeFocusOnTab: true
        Keys.onSpacePressed: row.chosen()
        Keys.onReturnPressed: row.chosen()

        MouseArea {
            id: rowMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: row.chosen()
        }

        FocusRing {
            targetRadius: Theme.radiusField
            visible: row.activeFocus
        }

        ColumnLayout {
            id: rowContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: Theme.space(3)
            anchors.rightMargin: Theme.space(3)
            spacing: Theme.space(1)

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)

                Rectangle {
                    implicitWidth: Theme.dotSize * 1.5
                    implicitHeight: implicitWidth
                    radius: width / 2
                    color: row.selected ? Theme.accentStroke : "transparent"
                    border.width: Theme.hairline
                    border.color: row.selected ? Theme.accentStroke : Theme.borderStrong
                }

                Text {
                    text: row.modelData.name
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.fillWidth: true
                    text: row.modelData.providerName
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }

                Text {
                    text: row.modelData.price
                    color: Theme.textSecondary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }
            }

            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1)

                Repeater {
                    model: row.modelData.badges
                    delegate: AiBadge {
                        required property string modelData
                        text: modelData
                    }
                }

                AiBadge {
                    visible: row.modelData.verified === false
                    warning: true
                    text: qsTr("Unverified")
                }

                AiBadge {
                    visible: (row.modelData.reason || "") !== ""
                    warning: true
                    text: row.modelData.reason || ""
                }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.space(4)

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 3
            spacing: Theme.space(2)

            SectionHeader {
                Layout.alignment: Qt.AlignLeft
                text: qsTr("Assistant")
            }

            ListView {
                objectName: "aiChatModelList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: Theme.space(2)
                boundsBehavior: Flickable.StopAtBounds
                model: AiController.chatModels
                ScrollBar.vertical: ThinScrollBar {}
                delegate: ModelRow {
                    selected: modelData.id === AiController.chatModel
                    onChosen: AiController.chatModel = modelData.id
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 2
            spacing: Theme.space(2)

            SectionHeader {
                Layout.alignment: Qt.AlignLeft
                text: qsTr("Transcription")
            }

            ModelRow {
                Layout.fillWidth: true
                modelData: ({ id: "", name: qsTr("On this computer"), providerName: qsTr("whisper.cpp"), price: qsTr("Free"),
                              badges: [qsTr("Word timing"), qsTr("Private")], verified: true, usable: true, reason: "" })
                selected: AiController.transcriptionModel === ""
                onChosen: AiController.transcriptionModel = ""
            }

            ListView {
                objectName: "aiTranscriptionModelList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: Theme.space(2)
                boundsBehavior: Flickable.StopAtBounds
                model: AiController.transcriptionModels
                ScrollBar.vertical: ThinScrollBar {}
                delegate: ModelRow {
                    selected: modelData.id === AiController.transcriptionModel
                    onChosen: AiController.transcriptionModel = modelData.id
                }
            }

            // 0.2: the AI helpers on this computer run below normal priority under a CPU cap, so playback stays smooth
            // while they work in the background.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Text {
                    text: qsTr("AI helpers' CPU")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                PillTabs {
                    id: helperCpu
                    readonly property var limits: [100, 75, 50, 25]
                    objectName: "aiHelperCpuLimit"
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("No limit"), qsTr("75 %"), qsTr("50 %"), qsTr("25 %")]
                    currentIndex: Math.max(0, helperCpu.limits.indexOf(AiController.helperCpuLimit))
                    Accessible.name: qsTr("Limit the CPU use of AI helpers on this computer")
                    onActivated: index => AiController.helperCpuLimit = helperCpu.limits[index]
                }
            }

            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Model list of %1. Prices are the providers' list prices and drive the spend caps.").arg(AiController.registryUpdated)
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
    }
}
