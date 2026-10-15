pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Preferences > AI > Privacy: the global offline switch, the open project's no-cloud lock, where keys are stored,
// and the consent history ("Always allow" per provider and capability), each revocable.
Item {
    id: page

    component Explained: ColumnLayout {
        id: explained
        property string text: ""
        property string explanation: ""
        property string toggleName: ""
        property bool checked: false
        signal toggled(bool on)
        spacing: Theme.space(1)

        Toggle {
            objectName: explained.toggleName
            text: explained.text
            checked: explained.checked
            onToggled: explained.toggled(checked)
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.toggleWidth + Theme.space(3)
            wrapMode: Text.WordWrap
            text: explained.explanation
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.space(6)

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            spacing: Theme.space(4)

            SectionHeader {
                Layout.alignment: Qt.AlignLeft
                text: qsTr("Cloud AI")
            }

            Explained {
                Layout.fillWidth: true
                toggleName: "aiOfflineToggle"
                text: qsTr("Offline mode")
                checked: AiController.offlineMode
                onToggled: on => AiController.offlineMode = on
                explanation: qsTr("Turns off every cloud provider. Local AI keeps working.")
            }

            Explained {
                Layout.fillWidth: true
                toggleName: "aiProjectLockToggle"
                text: qsTr("Lock this project to no-cloud AI")
                checked: AiController.projectLock
                onToggled: on => AiController.projectLock = on
                explanation: qsTr("For footage under NDA: nothing from this project is sent to a cloud provider, even with consent.")
            }

            SectionHeader {
                Layout.alignment: Qt.AlignLeft
                text: qsTr("Keys")
            }

            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: AiController.keyStorePersistent
                      ? qsTr("Keys are kept in %1. Wabcut stores only the last four characters to show them here.").arg(AiController.keyStoreName)
                      : qsTr("No keychain service is available, so keys are kept for this session only.")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }

            Item {
                Layout.fillHeight: true
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
                    text: qsTr("Allowed uploads")
                }

                Item {
                    Layout.fillWidth: true
                }

                PillButton {
                    compact: true
                    variant: "ghost"
                    enabled: AiController.consents.length > 0
                    text: qsTr("Revoke all")
                    onClicked: AiController.revokeAllConsents()
                }
            }

            Text {
                Layout.fillWidth: true
                visible: AiController.consents.length === 0
                wrapMode: Text.WordWrap
                text: qsTr("No provider has been allowed yet. Wabcut asks before the first upload to each provider, and shows what is sent, where, and what it may cost.")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            ListView {
                objectName: "aiConsentList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: Theme.space(2)
                model: AiController.consents
                ScrollBar.vertical: ThinScrollBar {}

                delegate: Rectangle {
                    id: consentRow
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: Theme.controlHeight + Theme.space(2)
                    radius: Theme.radiusField
                    color: Theme.raised
                    border.width: Theme.hairline
                    border.color: Theme.border

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.space(3)
                        anchors.rightMargin: Theme.space(2)
                        spacing: Theme.space(2)

                        Text {
                            text: consentRow.modelData.providerName
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                        }

                        AiBadge {
                            text: consentRow.modelData.capabilityName
                        }

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("since %1").arg(consentRow.modelData.grantedAt)
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textSm
                        }

                        PillButton {
                            compact: true
                            variant: "ghost"
                            text: qsTr("Revoke")
                            onClicked: AiController.revokeConsent(consentRow.modelData.provider, consentRow.modelData.capability)
                        }
                    }
                }
            }
        }
    }
}
