pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Preferences > AI > Usage: this month's spend per provider against an optional monthly cap (enforced on this
// computer from the registry's prices, before each call), and the local request log: every call and HTTP exchange
// with keys and tokens removed, so users can audit what was sent where.
Item {
    id: page

    RowLayout {
        anchors.fill: parent
        spacing: Theme.space(6)

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            spacing: Theme.space(2)

            SectionHeader {
                Layout.alignment: Qt.AlignLeft
                text: qsTr("This month")
            }

            ListView {
                objectName: "aiSpendList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: Theme.space(2)
                boundsBehavior: Flickable.StopAtBounds
                model: AiController.spend
                ScrollBar.vertical: ThinScrollBar {}

                delegate: Rectangle {
                    id: spendRow
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: spendContent.implicitHeight + Theme.space(4)
                    radius: Theme.radiusField
                    color: Theme.raised
                    border.width: Theme.hairline
                    border.color: Theme.border

                    ColumnLayout {
                        id: spendContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Theme.space(3)
                        anchors.rightMargin: Theme.space(3)
                        spacing: Theme.space(1)

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space(2)

                            Text {
                                Layout.fillWidth: true
                                text: spendRow.modelData.providerName
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.DemiBold
                            }

                            Text {
                                text: spendRow.modelData.capped ? qsTr("%1 of %2").arg(spendRow.modelData.spent).arg(spendRow.modelData.cap)
                                                                : spendRow.modelData.spent
                                color: Theme.textSecondary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textSm
                            }

                            Text {
                                text: qsTr("Cap $")
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }

                            NumberField {
                                objectName: "aiCapField_" + spendRow.modelData.provider
                                Layout.preferredWidth: Theme.numberFieldWidth + Theme.space(4)
                                value: spendRow.modelData.capDollars
                                from: 0
                                to: 100000
                                stepSize: 1
                                decimals: 2
                                Accessible.name: qsTr("Monthly cap for %1 in dollars, 0 for none").arg(spendRow.modelData.providerName)
                                onValueModified: value => AiController.setMonthlyCap(spendRow.modelData.provider, value)
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            visible: spendRow.modelData.capped
                            implicitHeight: Theme.space(1)
                            radius: height / 2
                            color: Theme.sunken

                            Rectangle {
                                width: parent.width * spendRow.modelData.fraction
                                height: parent.height
                                radius: height / 2
                                color: spendRow.modelData.fraction >= 0.9 ? Theme.warning : Theme.accentStroke
                            }
                        }
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            spacing: Theme.space(2)

            RowLayout {
                Layout.fillWidth: true

                SectionHeader {
                    text: qsTr("Request log")
                }

                Item {
                    Layout.fillWidth: true
                }

                PillButton {
                    compact: true
                    variant: "ghost"
                    enabled: AiController.requestLog.length > 0
                    text: qsTr("Clear")
                    onClicked: AiController.clearRequestLog()
                }
            }

            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: AiController.requestLog.length === 0
                      ? qsTr("No AI requests yet. Every call is listed here, on this computer only, with keys and tokens removed.")
                      : qsTr("Kept on this computer only, with keys and tokens removed.")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            ListView {
                objectName: "aiRequestLog"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: Theme.space(1)
                boundsBehavior: Flickable.StopAtBounds
                model: AiController.requestLog
                ScrollBar.vertical: ThinScrollBar {}

                delegate: RowLayout {
                    id: logRow
                    required property var modelData
                    width: ListView.view.width
                    spacing: Theme.space(2)

                    Text {
                        text: logRow.modelData.time
                        color: Theme.textTertiary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }

                    Text {
                        Layout.fillWidth: true
                        text: logRow.modelData.what
                        elide: Text.ElideMiddle
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }

                    Text {
                        text: logRow.modelData.error !== "" ? logRow.modelData.error : logRow.modelData.status
                        color: logRow.modelData.error !== "" ? Theme.danger : Theme.textTertiary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }

                    Text {
                        visible: logRow.modelData.cost !== ""
                        text: logRow.modelData.cost
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                }
            }
        }
    }
}
