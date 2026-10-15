pragma ComponentBehavior: Bound
import QtQuick

// Settings > AI (0.1.3; it replaces the Preferences > AI popup): one switch for every AI feature, the assistant's model
// with a field for any model by name (a provider's newest model, an OpenRouter or Ollama id), then the AI platform's
// pages: Providers (keys, checked and kept in the OS keychain, never shown; server addresses), Models, Privacy, Usage
// and the local assistant. Turning AI off hides the AI entry points (the rail's Generate, the sequence menu's script
// tools) and turns AiController's offline mode on, so no AI request leaves the computer; turning it back on restores
// the offline mode it had. objectName and currentTab keep the old popup's names (tests, the transcript's "set up").
Item {
    id: page
    objectName: "aiPreferences"

    /// The Settings window is open (the old popup's `opened`: true while this page shows).
    property bool settingsOpen: false
    readonly property bool opened: settingsOpen && visible
    property alias currentTab: tabs.currentIndex
    property string message: ""
    property bool messageOk: true
    // The provider the custom model field adds to.
    property string customProvider: ""

    readonly property var providerList: AiController.providers
    readonly property var currentModel: AiController.chatModels.find(m => m.id === AiController.chatModel) ?? null
    readonly property var userModels: AiController.chatModels.filter(m => m.userAdded === true)
    readonly property string customProviderName: {
        const p = page.providerList.find(p => p.id === page.customProvider)
        return p ? p.name : qsTr("Provider")
    }

    signal actionRequested(string what)

    /// AI on or off (see the header).
    function setAiEnabled(on: bool) {
        if (on === Preferences.aiEnabled)
            return
        if (!on) {
            Preferences.aiOfflineBefore = AiController.offlineMode
            AiController.offlineMode = true
        } else {
            AiController.offlineMode = Preferences.aiOfflineBefore
        }
        Preferences.aiEnabled = on
    }
    /// Adds `modelId` to the chosen provider (when it is not listed yet) and makes it the assistant's model.
    function useCustomModel(modelId: string): bool {
        const id = modelId.trim()
        if (id === "" || page.customProvider === "") {
            page.setMessage(false, qsTr("Choose a provider and type the model's id."))
            return false
        }
        const known = AiController.chatModels.find(m => m.id === id)
        if (!known && !AiController.addModel(page.customProvider, id)) {
            page.setMessage(false, qsTr("%1 cannot take that model id.").arg(page.customProviderName))
            return false
        }
        AiController.chatModel = id
        page.setMessage(true, qsTr("The assistant now uses %1.").arg(id))
        return true
    }
    function setMessage(ok: bool, text: string) {
        page.messageOk = ok
        page.message = text
    }

    Component.onCompleted: {
        if (page.customProvider === "" && page.providerList.length > 0)
            page.customProvider = (page.currentModel?.provider ?? "") !== "" ? page.currentModel.provider : page.providerList[0].id
    }

    // ---- Header ----
    Column {
        id: header
        x: Theme.space(6)
        y: Theme.space(5)
        width: parent.width - Theme.space(12)
        spacing: Theme.space(4)

        Column {
            width: parent.width
            spacing: Theme.space(1)

            Text {
                text: qsTr("AI")
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.textTitle
                font.weight: Font.DemiBold
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            Text {
                width: parent.width - Theme.space(10)
                text: qsTr("Transcription, the assistant, generation and the other AI tools: which providers they use, your keys, and what may leave this computer.")
                wrapMode: Text.WordWrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        SettingsSection {
            SettingsRow {
                label: qsTr("AI features")
                description: Preferences.aiEnabled
                             ? qsTr("On. %1").arg(AiController.statusLabel)
                             : qsTr("Off: the AI tools are hidden and nothing is sent to AI providers.")
                Toggle {
                    objectName: "aiFeaturesToggle"
                    tooltip: qsTr("AI features")
                    checked: Preferences.aiEnabled
                    onToggled: page.setAiEnabled(checked)
                }
            }
            SettingsRow {
                objectName: "aiAssistantModelRow"
                enabled: Preferences.aiEnabled
                opacity: enabled ? 1 : Theme.disabledOpacity
                label: qsTr("Assistant model")
                description: page.currentModel ? qsTr("%1 · %2").arg(page.currentModel.name).arg(page.currentModel.providerName)
                                               : qsTr("None chosen yet: pick one in Models, or use one by name.")

                PillButton {
                    id: providerButton
                    objectName: "aiCustomModelProvider"
                    compact: true
                    variant: "secondary"
                    iconName: "chevron-down"
                    text: page.customProviderName
                    LayoutMirroring.enabled: true
                    LayoutMirroring.childrenInherit: true
                    tooltip: qsTr("The provider the model belongs to")
                    onClicked: providerMenu.popup(providerButton, 0, providerButton.height + Theme.space(1))

                    ContextMenu {
                        id: providerMenu
                        objectName: "aiCustomModelProviderMenu"

                        Instantiator {
                            model: page.providerList
                            delegate: ContextMenuItem {
                                required property var modelData
                                text: modelData.name
                                checkable: true
                                checked: modelData.id === page.customProvider
                                onTriggered: page.customProvider = modelData.id
                            }
                            onObjectAdded: (index, object) => providerMenu.insertItem(index, object as ContextMenuItem)
                            onObjectRemoved: (index, object) => providerMenu.removeItem(object as ContextMenuItem)
                        }
                    }
                }
                FieldInput {
                    id: customModelField
                    objectName: "aiCustomModelField"
                    width: Theme.space(52)
                    font.family: Theme.fontMono
                    placeholderText: qsTr("Model id, e.g. claude-sonnet-5-5")
                    Accessible.name: qsTr("Model id to use")
                    onAccepted: useButton.clicked()
                }
                PillButton {
                    id: useButton
                    objectName: "aiCustomModelUse"
                    compact: true
                    variant: "primary"
                    text: qsTr("Use")
                    enabled: customModelField.text.trim() !== ""
                    onClicked: {
                        if (page.useCustomModel(customModelField.text))
                            customModelField.clear()
                    }
                }
            }
            // Models added by name, removable.
            Flow {
                width: parent.width
                visible: page.userModels.length > 0 || page.message !== ""
                spacing: Theme.space(1)
                bottomPadding: Theme.space(2)

                Text {
                    visible: page.message !== ""
                    height: Theme.controlHeightSmall
                    verticalAlignment: Text.AlignVCenter
                    rightPadding: Theme.space(2)
                    text: page.message
                    color: page.messageOk ? Theme.textSecondary : Theme.danger
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Repeater {
                    model: page.userModels
                    delegate: Rectangle {
                        id: chip
                        required property var modelData
                        objectName: "aiUserModel_" + modelData.id
                        height: Theme.controlHeightSmall
                        width: chipText.implicitWidth + chipRemove.width + Theme.space(3)
                        radius: Theme.radiusPill
                        color: Theme.sunken
                        border.width: Theme.hairline
                        border.color: modelData.id === AiController.chatModel ? Theme.accentStroke : Theme.border

                        Text {
                            id: chipText
                            x: Theme.space(2.5)
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("%1 · %2").arg(chip.modelData.id).arg(chip.modelData.providerName)
                            color: Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                        IconButton {
                            id: chipRemove
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            compact: true
                            iconName: "x"
                            iconSize: Theme.iconXs
                            tooltip: qsTr("Remove %1").arg(chip.modelData.id)
                            onClicked: AiController.removeModel(chip.modelData.id)
                        }
                    }
                }
            }
        }

        PillTabs {
            id: tabs
            objectName: "aiPreferencesTabs"
            width: Math.min(parent.width, Theme.space(150))
            tabs: true
            enabled: Preferences.aiEnabled
            model: [qsTr("Providers and keys"), qsTr("Models"), qsTr("Privacy"), qsTr("Usage"), qsTr("Local assistant")]
            Accessible.name: qsTr("AI settings pages")
        }
    }

    // ---- The AI platform's pages ----
    Item {
        id: body
        anchors.top: header.bottom
        anchors.topMargin: Theme.space(3)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.space(6)
        anchors.rightMargin: Theme.space(6)
        anchors.bottomMargin: Theme.space(5)
        enabled: Preferences.aiEnabled
        opacity: enabled ? 1 : Theme.disabledOpacity

        Loader {
            anchors.fill: parent
            active: tabs.currentIndex === 0
            sourceComponent: AiProvidersPage {}
        }
        Loader {
            anchors.fill: parent
            active: tabs.currentIndex === 1
            sourceComponent: AiModelsPage {}
        }
        Loader {
            anchors.fill: parent
            active: tabs.currentIndex === 2
            sourceComponent: AiPrivacyPage {}
        }
        Loader {
            anchors.fill: parent
            active: tabs.currentIndex === 3
            sourceComponent: AiUsagePage {}
        }
        Loader {
            anchors.fill: parent
            active: tabs.currentIndex === 4
            sourceComponent: LocalLlmPage {}
        }
    }
}
