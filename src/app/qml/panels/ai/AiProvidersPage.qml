pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Preferences > AI > Providers: one card per provider. Cloud providers take a key (pasted into a masked field,
// checked with the provider's cheap authenticated call, then stored in the OS keychain); local servers take an
// address and model ids. Each card states what the provider does with uploaded data, with a link to its terms.
Item {
    id: page

    ListView {
        id: list
        objectName: "aiProviderList"
        anchors.fill: parent
        clip: true
        spacing: Theme.space(2)
        boundsBehavior: Flickable.StopAtBounds
        model: AiController.providers
        ScrollBar.vertical: ThinScrollBar {}

        header: Column {
            width: list.width
            spacing: Theme.space(2)
            bottomPadding: Theme.space(2)

            // Keys left in plaintext by the removed opt-in (review wave 4, P2-5): move them, then delete the file.
            Rectangle {
                id: legacyKeys
                objectName: "legacyKeysBanner"
                property string result: ""
                visible: AiController.legacyPlaintextKeys > 0 || result !== ""
                width: parent.width
                implicitHeight: legacyColumn.implicitHeight + Theme.space(4)
                radius: Theme.radiusField
                color: Theme.raised
                border.color: Theme.warning
                border.width: 1
                Column {
                    id: legacyColumn
                    anchors.fill: parent
                    anchors.margins: Theme.space(2)
                    spacing: Theme.space(2)
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: legacyKeys.result !== "" ? legacyKeys.result
                              : qsTr("%n API key(s) are still saved in a plaintext file from an earlier version. Move them into %1 and delete the file?", "", AiController.legacyPlaintextKeys).arg(AiController.keyStoreName)
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Row {
                        spacing: Theme.space(2)
                        visible: AiController.legacyPlaintextKeys > 0
                        PillButton {
                            objectName: "importLegacyKeys"
                            text: qsTr("Move keys")
                            onClicked: legacyKeys.result = AiController.importPlaintextKeys()
                        }
                        PillButton {
                            objectName: "deleteLegacyKeys"
                            text: qsTr("Delete file")
                            onClicked: legacyKeys.result = AiController.deletePlaintextKeys() ? qsTr("The plaintext key file was deleted.")
                                                                                              : qsTr("The plaintext key file could not be deleted.")
                        }
                    }
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Wabcut works without any key: local AI runs on this computer. Add a key to use a cloud provider; keys stay in %1 and never go into projects or logs.").arg(AiController.keyStoreName)
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        delegate: Rectangle {
            id: card

            required property var modelData
            required property int index
            property string message: ""
            property bool messageOk: true
            property bool busy: false

            width: list.width
            implicitHeight: content.implicitHeight + Theme.space(6)
            radius: Theme.radiusField
            color: Theme.raised
            border.width: Theme.hairline
            border.color: Theme.border

            Accessible.role: Accessible.Grouping
            Accessible.name: card.modelData.name

            Connections {
                target: AiController
                function onKeyCheckFinished(providerId, ok, message) {
                    if (providerId !== card.modelData.id)
                        return
                    card.busy = false
                    card.messageOk = ok
                    card.message = message
                    if (ok)
                        keyField.clear()
                }
            }

            ColumnLayout {
                id: content
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.space(3)
                spacing: Theme.space(2)

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)

                    Text {
                        text: card.modelData.name
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textLg
                        font.weight: Font.DemiBold
                    }

                    AiBadge {
                        visible: card.modelData.capabilities !== ""
                        text: card.modelData.capabilities
                    }

                    AiBadge {
                        visible: !card.modelData.verified
                        warning: true
                        text: qsTr("Unverified")
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    StatusPill {
                        label: card.modelData.statusText
                        dotColor: card.modelData.status === "none" ? Theme.textTertiary
                                : card.modelData.status === "local" ? Theme.info : Theme.success
                        tooltip: card.modelData.validatedAt !== "" ? qsTr("Last checked %1").arg(card.modelData.validatedAt) : ""
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: card.modelData.dataPolicy !== ""
                    wrapMode: Text.WordWrap
                    text: card.modelData.dataPolicy
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)

                    // Cloud: paste a key.
                    AiTextField {
                        id: keyField
                        objectName: "aiKeyField_" + card.modelData.id
                        visible: card.modelData.keyRequired
                        Layout.preferredWidth: Theme.space(80)
                        masked: true
                        placeholderText: card.modelData.hasKey ? qsTr("Paste a new key to replace it") : qsTr("Paste API key")
                        onAccepted: saveButton.clicked()
                    }

                    PillButton {
                        id: saveButton
                        visible: card.modelData.keyRequired
                        compact: true
                        variant: "primary"
                        enabled: keyField.text.length > 0 && !card.busy
                        text: qsTr("Save and check")
                        onClicked: {
                            card.busy = true
                            card.message = qsTr("Checking…")
                            card.messageOk = true
                            AiController.saveKey(card.modelData.id, keyField.text, false)
                        }
                    }

                    // Local / OpenAI-compatible: the server address.
                    AiTextField {
                        id: urlField
                        visible: card.modelData.configurableBaseUrl
                        Layout.preferredWidth: Theme.space(64)
                        monospace: true
                        text: card.modelData.baseUrl
                        placeholderText: qsTr("Server address")
                        onEditingFinished: if (text !== card.modelData.baseUrl) AiController.setBaseUrl(card.modelData.id, text)
                    }

                    AiTextField {
                        id: modelField
                        visible: card.modelData.configurableBaseUrl
                        Layout.preferredWidth: Theme.space(44)
                        monospace: true
                        placeholderText: qsTr("Model id, e.g. qwen3.5:9b")
                        onAccepted: addButton.clicked()
                    }

                    PillButton {
                        id: addButton
                        visible: card.modelData.configurableBaseUrl
                        compact: true
                        enabled: modelField.text.length > 0
                        text: qsTr("Add model")
                        onClicked: {
                            if (AiController.addModel(card.modelData.id, modelField.text)) {
                                card.messageOk = true
                                card.message = qsTr("Added %1.").arg(modelField.text)
                                modelField.clear()
                            } else {
                                card.messageOk = false
                                card.message = qsTr("That model id is empty or already listed.")
                            }
                        }
                    }

                    PillButton {
                        compact: true
                        enabled: (card.modelData.hasKey || !card.modelData.keyRequired) && !card.busy
                        text: card.modelData.keyRequired ? qsTr("Check") : qsTr("Check server")
                        onClicked: {
                            card.busy = true
                            card.message = qsTr("Checking…")
                            card.messageOk = true
                            AiController.testKey(card.modelData.id)
                        }
                    }

                    PillButton {
                        visible: card.modelData.keyRequired
                        compact: true
                        variant: "ghost"
                        enabled: card.modelData.keySource === "store"
                        iconName: "trash-2"
                        text: qsTr("Remove")
                        onClicked: AiController.removeKey(card.modelData.id)
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    PillButton {
                        visible: card.modelData.consoleUrl !== ""
                        compact: true
                        variant: "ghost"
                        text: qsTr("Get a key")
                        tooltip: card.modelData.consoleUrl
                        onClicked: Qt.openUrlExternally(card.modelData.consoleUrl)
                    }

                    PillButton {
                        visible: card.modelData.termsUrl !== ""
                        compact: true
                        variant: "ghost"
                        text: qsTr("Data terms")
                        tooltip: card.modelData.termsUrl
                        onClicked: Qt.openUrlExternally(card.modelData.termsUrl)
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: card.message !== ""
                    wrapMode: Text.WordWrap
                    text: card.message
                    color: card.messageOk ? Theme.success : Theme.danger
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
        }
    }
}
