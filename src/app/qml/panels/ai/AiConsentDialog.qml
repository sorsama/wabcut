pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The first-use consent prompt (docs/ai.md principle 4): before the first upload to a provider for a capability,
// show what is sent, to whom, what the provider does with it (with a link to its terms) and what it may cost.
// "Always allow" is remembered per provider x capability (AiController.grantConsent); "Allow once" is not.
//
//   dialog.ask(AiController.consentPrompt(modelId, "transcribe", qsTr("audio of 3 clips, 12 min")))
//   onAllowed: always => ...start the job with CallOptions.allowOnce when !always...
Popup {
    id: root
    objectName: "aiConsentDialog"

    property var prompt: ({})
    property bool answered: false

    signal allowed(bool always)
    signal declined()

    function ask(details) {
        root.prompt = details
        root.answered = false
        open()
    }

    parent: Overlay.overlay
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: Math.min(Theme.space(130), (parent ? parent.width : Theme.windowDefaultWidth) - Theme.space(16))
    modal: true
    focus: true
    padding: Theme.space(6)
    closePolicy: Popup.CloseOnEscape
    onClosed: if (!root.answered) root.declined() // Escape or Cancel

    Overlay.modal: Rectangle {
        color: Theme.scrim
    }

    background: Rectangle {
        radius: Theme.radiusPanel
        color: Theme.panel
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }

    component Fact: ColumnLayout {
        id: fact
        property string label: ""
        property string value: ""
        property bool monospace: false
        Layout.fillWidth: true
        spacing: Theme.space(1)

        Text {
            text: fact.label
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
            font.letterSpacing: Theme.labelTracking
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: fact.value
            color: Theme.textPrimary
            font.family: fact.monospace ? Theme.fontMono : Theme.fontSans
            font.pixelSize: Theme.textMd
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.space(4)
        Accessible.role: Accessible.Dialog
        Accessible.name: title.text

        Text {
            id: title
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Send to %1?").arg(root.prompt.providerName || "")
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textTitle
            font.weight: Font.DemiBold
        }

        Fact {
            label: qsTr("What is sent")
            value: root.prompt.payload || ""
        }

        Fact {
            label: qsTr("Where")
            value: root.prompt.destination || ""
            monospace: true
        }

        Fact {
            label: qsTr("What the provider does with it")
            value: root.prompt.dataPolicy || ""
        }

        Fact {
            label: qsTr("Price")
            value: root.prompt.cost || ""
            monospace: true
        }

        PillButton {
            visible: (root.prompt.termsUrl || "") !== ""
            compact: true
            variant: "ghost"
            text: qsTr("Read the provider's data terms")
            tooltip: root.prompt.termsUrl || ""
            onClicked: Qt.openUrlExternally(root.prompt.termsUrl)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)

            Item {
                Layout.fillWidth: true
            }

            PillButton {
                text: qsTr("Cancel")
                variant: "ghost"
                onClicked: root.close()
            }

            PillButton {
                objectName: "aiConsentAllowOnce"
                text: qsTr("Allow once")
                onClicked: {
                    root.answered = true
                    root.allowed(false)
                    root.close()
                }
            }

            PillButton {
                objectName: "aiConsentAlways"
                text: qsTr("Always allow")
                variant: "primary"
                onClicked: {
                    AiController.grantConsent(root.prompt.provider, root.prompt.capability)
                    root.answered = true
                    root.allowed(true)
                    root.close()
                }
            }
        }
    }
}
