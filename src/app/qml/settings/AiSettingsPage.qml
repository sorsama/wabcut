pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Settings > AI (0.1.3; r07 rework): one switch for every AI feature with a summary of what runs on this computer and
// what goes to the cloud, then five tabs. Features: what each AI part (assistant, transcription, translation, voice,
// image, video, sound effects) uses, with a picker per part. Providers: 50+ providers as a searchable, filterable list
// with a detail pane (keys, checked and kept in the OS keychain, never shown; server addresses; models; custom
// providers). Local models: the models that run on this computer (LocalLlmPage). Privacy and Usage. Turning AI off
// hides the AI entry points (the Window menu's Generate, the sequence menu's script tools) and turns AiController's
// offline mode on, so no AI request leaves the computer; turning it back on restores the offline mode it had.
// objectName and currentTab keep the old popup's names (tests, the transcript's "set up").
Item {
    id: page
    objectName: "aiPreferences"

    /// The Settings window is open (the old popup's `opened`: true while this page shows).
    property bool settingsOpen: false
    readonly property bool opened: settingsOpen && visible
    /// 0 Features, 1 Providers, 2 Local models, 3 Privacy, 4 Usage.
    property alias currentTab: tabs.currentIndex
    property string message: ""
    property bool messageOk: true
    // The provider useCustomModel adds to.
    property string customProvider: ""
    readonly property alias catalog: catalog
    readonly property alias customProviderDialog: customDialog
    // Features and Providers stay alive once shown (their scroll position, filter and chosen provider).
    property bool keepFeatures: false
    property bool keepProviders: false
    readonly property var currentModel: AiController.chatModels.find(m => m.id === AiController.chatModel) ?? null

    signal actionRequested(string what)

    AiCatalog {
        id: catalog
    }

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
    /// Adds `modelId` to `customProvider` (when it is not listed yet) and makes it the assistant's model.
    function useCustomModel(modelId: string): bool {
        if (page.customProvider === "")
            page.customProvider = (page.currentModel?.provider ?? "") !== "" ? page.currentModel.provider
                                : (AiController.providers.length > 0 ? AiController.providers[0].id : "")
        const problem = catalog.useModelId("chat", page.customProvider, modelId)
        page.setMessage(problem === "", problem === "" ? qsTr("The assistant now uses %1.").arg(modelId.trim()) : problem)
        return problem === ""
    }
    function setMessage(ok: bool, text: string) {
        page.messageOk = ok
        page.message = text
    }
    /// Shows Providers with the filter chip `filterId` ("all", "connected", "local", "custom" or a capability id).
    function showProviders(filterId: string) {
        tabs.currentIndex = 1
        const providers = providersLoader.item as AiProvidersPage
        if (providers)
            providers.showFilter(filterId)
    }
    /// Shows Providers with `providerId`'s detail.
    function showProvider(providerId: string) {
        tabs.currentIndex = 1
        const providers = providersLoader.item as AiProvidersPage
        if (providers)
            providers.select(providerId)
    }
    /// Opens the custom provider dialog: "" for a new provider, else the custom provider to edit.
    function openCustomProvider(providerId: string) {
        customDialog.openFor(providerId)
    }
    /// Asks before deleting a custom provider.
    function requestDelete(providerId: string) {
        deleteDialog.providerId = providerId
        deleteDialog.open()
    }
    /// The Settings search (SettingsPage.reveal's counterpart; AppSettings.index rows of page "ai"): "aiFeatures" flashes
    /// the switch; "assistantModel" shows Features and flashes the assistant's row; "aiDefaults" and "aiModels" show
    /// Features, "aiProviders" Providers, "aiCustomProvider" Providers with the custom provider button focused,
    /// "localAssistant" Local models, "aiPrivacy" and "aiUsage" their tabs. A capability id ("chat", "transcribe",
    /// "translate", "speak", "image", "video", "sound") shows Features and flashes that row (QA-12: the transcript
    /// panel's "Set up" reveals "transcribe"). False for another key.
    function reveal(target: var): bool {
        const capability = String(target)
        if (catalog.capabilities.some(c => c.id === capability)) {
            tabs.currentIndex = 0
            Qt.callLater(() => (featuresLoader.item as AiFeaturesPage)?.reveal(capability))
            return true
        }
        const tab = ({ aiDefaults: 0, aiModels: 0, assistantModel: 0, aiProviders: 1, aiCustomProvider: 1, localAssistant: 2,
                       aiPrivacy: 3, aiUsage: 4 })[target]
        if (tab !== undefined) {
            tabs.currentIndex = tab
            if (target === "assistantModel") {
                Qt.callLater(() => (featuresLoader.item as AiFeaturesPage)?.reveal("chat"))
            } else if (target === "aiCustomProvider") {
                Qt.callLater(() => (providersLoader.item as AiProvidersPage)?.focusCustomButton())
            } else if (tabs.enabled) {
                tabs.forceActiveFocus(Qt.TabFocusReason)
            }
            return true
        }
        if (target !== "aiFeatures")
            return false
        featuresRow.flash()
        featuresToggle.forceActiveFocus(Qt.TabFocusReason)
        return true
    }

    /// What runs where, for the header: "On this computer: …. Cloud: …. Not set up: …."
    readonly property string routingSummary: {
        const local = []
        const cloud = []
        const missing = []
        const routes = catalog.routing
        for (let i = 0; i < routes.length; ++i) {
            const r = routes[i]
            const name = catalog.capabilityName(r.capability)
            if (!r.usable)
                missing.push(name)
            else if (r.local)
                local.push(name)
            else
                cloud.push(r.providerName !== "" ? qsTr("%1 (%2)").arg(name).arg(r.providerName) : name)
        }
        const parts = []
        if (local.length > 0)
            parts.push(qsTr("On this computer: %1.").arg(local.join(", ")))
        if (cloud.length > 0)
            parts.push(qsTr("Cloud: %1.").arg(cloud.join(", ")))
        if (missing.length > 0)
            parts.push(qsTr("Not set up: %1.").arg(missing.join(", ")))
        return parts.join(" ")
    }

    // ---- Header ----
    Column {
        id: header
        x: Theme.space(6)
        y: Theme.space(5)
        width: parent.width - Theme.space(12)
        spacing: Theme.space(3)

        // The title with the switch for every AI feature beside it (r07: one line instead of a card, so the tabs below
        // get the room), then what runs where.
        Item {
            id: featuresRow
            objectName: "aiFeaturesRow"
            width: parent.width - Theme.space(10) // the Settings window's close button sits at the top right
            height: titleLine.height + Theme.space(1) + summary.implicitHeight

            /// Draws the eye to the switch (the Settings search's "aiFeatures").
            function flash() {
                highlight.opacity = 1
                flashFade.restart()
            }

            Rectangle {
                id: highlight
                objectName: "aiFeaturesFlash"
                anchors.fill: parent
                anchors.margins: -Theme.space(2)
                radius: Theme.radiusField
                color: Theme.accentWash
                border.width: Theme.hairline
                border.color: Theme.accentStroke
                opacity: 0
                visible: opacity > 0
            }
            SequentialAnimation {
                id: flashFade
                PauseAnimation { duration: 1200 }
                NumberAnimation { target: highlight; property: "opacity"; to: 0; duration: Theme.durCollapse * 2; easing.type: Easing.OutCubic }
            }

            Row {
                id: titleLine
                spacing: Theme.space(5)

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("AI")
                    color: Theme.textPrimary
                    font.family: Theme.fontDisplay
                    font.pixelSize: Theme.textTitle
                    font.weight: Font.DemiBold
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }
                Toggle {
                    id: featuresToggle
                    objectName: "aiFeaturesToggle"
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("AI features")
                    tooltip: qsTr("Turn every AI feature on or off")
                    checked: Preferences.aiEnabled
                    onToggled: page.setAiEnabled(checked)
                }
            }
            Text {
                id: summary
                objectName: "aiRoutingSummary"
                anchors.top: titleLine.bottom
                anchors.topMargin: Theme.space(1)
                width: parent.width
                text: !Preferences.aiEnabled ? qsTr("Off: the AI tools are hidden and nothing is sent to AI providers.")
                      : AiController.offlineMode ? qsTr("Offline: only AI on this computer runs. %1").arg(page.routingSummary)
                      : page.routingSummary
                wrapMode: Text.WordWrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        PillTabs {
            id: tabs
            objectName: "aiPreferencesTabs"
            width: Math.min(parent.width, Theme.space(140))
            tabs: true
            enabled: Preferences.aiEnabled
            model: [qsTr("Features"), qsTr("Providers"), qsTr("Local models"), qsTr("Privacy"), qsTr("Usage")]
            Accessible.name: qsTr("AI settings pages")
        }
        Text {
            width: parent.width
            visible: page.message !== ""
            text: page.message
            wrapMode: Text.WordWrap
            color: page.messageOk ? Theme.textSecondary : Theme.dangerText
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    // ---- The tabs' pages (each created when first shown, then kept) ----
    Item {
        id: body
        anchors.top: header.bottom
        anchors.topMargin: Theme.space(4)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.space(6)
        anchors.rightMargin: Theme.space(6)
        anchors.bottomMargin: Theme.space(5)
        enabled: Preferences.aiEnabled
        opacity: enabled ? 1 : Theme.disabledOpacity

        Loader {
            id: featuresLoader
            objectName: "aiFeaturesLoader"
            anchors.fill: parent
            active: tabs.currentIndex === 0 || page.keepFeatures
            visible: tabs.currentIndex === 0
            onLoaded: page.keepFeatures = true
            sourceComponent: AiFeaturesPage {
                catalog: catalog
                onAddProviderRequested: capabilityId => page.showProviders(capabilityId)
            }
        }
        Loader {
            id: providersLoader
            objectName: "aiProvidersLoader"
            anchors.fill: parent
            active: tabs.currentIndex === 1 || page.keepProviders
            visible: tabs.currentIndex === 1
            onLoaded: page.keepProviders = true
            sourceComponent: AiProvidersPage {
                catalog: catalog
                onCustomProviderRequested: providerId => page.openCustomProvider(providerId)
                onDeleteRequested: providerId => page.requestDelete(providerId)
                onUsageRequested: tabs.currentIndex = 4
            }
        }
        Loader {
            anchors.fill: parent
            active: tabs.currentIndex === 2
            sourceComponent: LocalLlmPage {}
        }
        Loader {
            anchors.fill: parent
            active: tabs.currentIndex === 3
            sourceComponent: AiPrivacyPage {}
        }
        Loader {
            anchors.fill: parent
            active: tabs.currentIndex === 4
            sourceComponent: AiUsagePage {}
        }
    }

    AiCustomProviderDialog {
        id: customDialog
        catalog: catalog
        onSaved: id => page.showProvider(id)
        onDeleteRequested: id => page.requestDelete(id)
    }

    WabcutDialog {
        id: deleteDialog
        objectName: "aiDeleteProviderDialog"
        property string providerId: ""
        readonly property string providerName: catalog.provider(deleteDialog.providerId)?.name ?? deleteDialog.providerId
        title: qsTr("Delete %1?").arg(deleteDialog.providerName)
        width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(110), Theme.space(110))

        Text {
            Layout.fillWidth: true
            text: qsTr("Its key, its models and the features that used it are cleared; those features go back to Automatic.")
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        footerData: [
            PillButton {
                objectName: "aiDeleteProviderCancel"
                text: qsTr("Cancel")
                onClicked: deleteDialog.close()
            },
            PillButton {
                objectName: "aiDeleteProviderConfirm"
                variant: "danger"
                text: qsTr("Delete")
                onClicked: {
                    const ok = AiController.removeCustomProvider(deleteDialog.providerId)
                    page.setMessage(ok, ok ? qsTr("%1 was deleted.").arg(deleteDialog.providerName)
                                           : qsTr("%1 could not be deleted.").arg(deleteDialog.providerName))
                    deleteDialog.close()
                }
            }
        ]
    }
}
