pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// The detail of one provider in Settings > AI > Providers (r07): its name, status, what it is good for and what it can
// do; its links (website, "Get a key", data terms); the API key (pasted into a masked field, checked with the provider's
// cheap authenticated call, then kept in the OS keychain: never shown, only its last four characters); the server
// address where it can change; its models, each with "Use for <capability>", plus "Add model id" and "Fetch models";
// what it does with uploaded data; this month's spend and the monthly cap. Custom providers add Edit and Delete. Every
// row of buttons wraps (Flow), so nothing runs off the edge at the Settings window's smallest size.
Item {
    id: detail

    required property var catalog
    /// The provider row (AiCatalog.providers), or null.
    property var provider: null
    /// Narrow layout: a Back button returns to the list.
    property bool showBack: false

    property string message: ""
    property bool messageOk: true
    property bool busy: false
    property string modelMessage: ""
    property bool modelMessageOk: true
    /// Fetch models: [{id, capability}] the provider lists, and whether a listing runs.
    property var remoteModels: []
    property bool fetching: false

    readonly property string providerId: detail.provider ? detail.provider.id : ""
    readonly property var models: detail.provider ? detail.catalog.modelsOf(detail.providerId) : []
    readonly property var spendRow: AiController.spend.find(s => s.provider === detail.providerId) ?? null
    readonly property bool cloud: detail.provider !== null && detail.provider.category !== "local" && detail.provider.local !== true
    readonly property bool canFetch: detail.provider !== null
                                     && (detail.provider.custom || detail.provider.configurableBaseUrl === true
                                         || detail.provider.api === "chat-completions")

    signal backRequested()
    signal editRequested(string providerId)
    signal deleteRequested(string providerId)
    signal usageRequested()

    onProviderIdChanged: {
        detail.message = ""
        detail.busy = false
        detail.modelMessage = ""
        detail.remoteModels = []
        detail.fetching = false
        addCapability.current = ""
        flick.contentY = 0
    }

    function saveKey(check: bool) {
        detail.busy = check
        detail.messageOk = true
        detail.message = check ? qsTr("Checking…") : ""
        AiController.saveKey(detail.providerId, keyField.text, !check)
    }
    function checkKey() {
        detail.busy = true
        detail.messageOk = true
        detail.message = qsTr("Checking…")
        AiController.testKey(detail.providerId)
    }
    function addModel(modelId: string, capabilityId: string): bool {
        const id = modelId.trim()
        const ok = id !== "" && detail.catalog.addModel(detail.providerId, id, capabilityId)
        detail.modelMessageOk = ok
        detail.modelMessage = ok ? qsTr("Added %1 for %2.").arg(id).arg(detail.catalog.capabilityName(capabilityId).toLowerCase())
                                 : qsTr("That model id is empty or already listed.")
        return ok
    }
    function fetchModels() {
        detail.fetching = true
        detail.modelMessage = qsTr("Asking %1 for its models…").arg(detail.provider.name)
        detail.modelMessageOk = true
        AiController.listRemoteModels(detail.providerId)
    }

    Connections {
        target: AiController
        function onKeyCheckFinished(providerId: string, ok: bool, message: string) {
            if (providerId !== detail.providerId)
                return
            detail.busy = false
            detail.messageOk = ok
            detail.message = message
            if (ok)
                keyField.clear()
        }
    }
    Connections {
        target: AiController
        function onRemoteModelsListed(providerId: string, ok: bool, models: var, message: string) {
            if (providerId !== detail.providerId)
                return
            detail.fetching = false
            detail.modelMessageOk = ok
            const known = detail.models.map(m => m.id)
            const fresh = []
            for (let i = 0; i < (models?.length ?? 0); ++i) {
                if (known.indexOf(models[i].id) < 0)
                    fresh.push(models[i])
            }
            detail.remoteModels = fresh
            detail.modelMessage = !ok ? message
                                : fresh.length === 0 ? qsTr("Every model %1 lists is here already.").arg(detail.provider.name)
                                : qsTr("%n more model(s) found: add the ones you want.", "", fresh.length)
        }
    }

    // A card of the detail: a title, an optional note, then its content.
    component Card: Rectangle {
        id: card
        property string title: ""
        property string note: ""
        default property alias content: cardBody.data

        width: parent ? parent.width : implicitWidth
        height: cardColumn.implicitHeight + Theme.space(8)
        radius: Theme.radiusCard
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.border
        Accessible.role: Accessible.Grouping
        Accessible.name: card.title

        Column {
            id: cardColumn
            x: Theme.space(4)
            y: Theme.space(4)
            width: parent.width - Theme.space(8)
            spacing: Theme.space(2)

            Text {
                width: parent.width
                visible: card.title !== ""
                text: card.title
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Theme.weightLabel
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            Text {
                width: parent.width
                visible: card.note !== ""
                text: card.note
                wrapMode: Text.WordWrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Column {
                id: cardBody
                width: parent.width
                spacing: Theme.space(2)
            }
        }
    }
    // A line of feedback under a card's controls.
    component Feedback: Text {
        property bool ok: true
        width: parent ? parent.width : implicitWidth
        visible: text !== ""
        wrapMode: Text.WordWrap
        color: ok ? Theme.textSecondary : Theme.dangerText
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight + Theme.space(4)
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: column
            width: flick.width - Theme.space(3)
            spacing: Theme.space(3)
            visible: detail.provider !== null

            // ---- Header ----
            PillButton {
                visible: detail.showBack
                objectName: "aiProviderBack"
                compact: true
                variant: "ghost"
                iconName: "chevron-left"
                text: qsTr("All providers")
                onClicked: detail.backRequested()
            }
            Flow {
                width: parent.width
                spacing: Theme.space(3)

                Text {
                    objectName: "aiProviderName"
                    width: Math.min(implicitWidth, parent.width)
                    height: Math.max(implicitHeight, Theme.controlHeightSmall)
                    verticalAlignment: Text.AlignVCenter
                    text: detail.provider ? detail.provider.name : ""
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontDisplay
                    font.pixelSize: Theme.textXl
                    font.weight: Theme.weightTitle
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }
                StatusPill {
                    objectName: "aiProviderStatus"
                    label: detail.provider ? detail.provider.statusText ?? "" : ""
                    dotColor: detail.provider ? detail.catalog.stateColor(detail.provider.status) : Theme.textTertiary
                    tooltip: detail.provider && (detail.provider.validatedAt ?? "") !== "" ? qsTr("Last checked %1").arg(detail.provider.validatedAt) : ""
                }
            }
            Text {
                width: parent.width
                visible: text !== ""
                text: detail.provider ? detail.provider.description : ""
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            Flow {
                objectName: "aiProviderBadges"
                width: parent.width
                spacing: Theme.space(1.5)

                Repeater {
                    model: detail.provider ? detail.provider.caps : []
                    delegate: AiBadge {
                        required property string modelData
                        iconName: detail.catalog.capabilityIcon(modelData)
                        text: detail.catalog.capabilityName(modelData)
                    }
                }
                AiBadge {
                    visible: detail.provider !== null && (detail.provider.category === "local" || detail.provider.local === true)
                    iconName: "house"
                    text: qsTr("On this computer or your server")
                }
                AiBadge {
                    visible: detail.provider !== null && detail.provider.custom === true
                    iconName: "settings"
                    text: qsTr("Custom")
                }
                AiBadge {
                    visible: detail.provider !== null && detail.provider.verified === false
                    warning: true
                    text: qsTr("Unverified")
                }
            }
            Flow {
                width: parent.width
                spacing: Theme.space(2)

                PillButton {
                    visible: detail.provider !== null && detail.provider.website !== ""
                    compact: true
                    variant: "ghost"
                    iconName: "external-link"
                    text: qsTr("Website")
                    tooltip: detail.provider ? detail.provider.website : ""
                    onClicked: Qt.openUrlExternally(detail.provider.website)
                }
                PillButton {
                    objectName: "aiProviderGetKey"
                    visible: detail.provider !== null && (detail.provider.consoleUrl ?? "") !== ""
                    compact: true
                    variant: "ghost"
                    iconName: "external-link"
                    text: qsTr("Get a key")
                    tooltip: detail.provider ? detail.provider.consoleUrl ?? "" : ""
                    onClicked: Qt.openUrlExternally(detail.provider.consoleUrl)
                }
                PillButton {
                    objectName: "aiProviderTerms"
                    visible: detail.provider !== null && (detail.provider.termsUrl ?? "") !== ""
                    compact: true
                    variant: "ghost"
                    iconName: "external-link"
                    text: qsTr("Data terms")
                    tooltip: detail.provider ? detail.provider.termsUrl ?? "" : ""
                    onClicked: Qt.openUrlExternally(detail.provider.termsUrl)
                }
                PillButton {
                    objectName: "aiProviderEdit"
                    visible: detail.provider !== null && detail.provider.custom === true
                    compact: true
                    iconName: "pencil"
                    text: qsTr("Edit…")
                    onClicked: detail.editRequested(detail.providerId)
                }
                PillButton {
                    objectName: "aiProviderDelete"
                    visible: detail.provider !== null && detail.provider.custom === true
                    compact: true
                    variant: "ghost"
                    iconName: "trash-2"
                    text: qsTr("Delete…")
                    onClicked: detail.deleteRequested(detail.providerId)
                }
            }

            // ---- The key ----
            Card {
                objectName: "aiProviderKeyCard"
                visible: detail.provider !== null && detail.provider.keyRequired === true
                title: qsTr("API key")
                note: !detail.provider ? ""
                      : detail.provider.keySource === "environment" ? qsTr("Read from the environment: %1.").arg(detail.provider.statusText)
                      : detail.provider.hasKey ? qsTr("Saved in %1 (%2). Paste a new key to replace it.").arg(AiController.keyStoreName).arg(detail.provider.masked ?? "")
                      : qsTr("Paste a key from your %1 account. It is checked, then kept in %2; it never goes into projects or logs.").arg(detail.provider.name).arg(AiController.keyStoreName)

                AiTextField {
                    id: keyField
                    objectName: "aiKeyField_" + detail.providerId
                    width: parent.width
                    masked: true
                    placeholderText: detail.provider && detail.provider.hasKey ? qsTr("Paste a new key to replace it") : qsTr("Paste API key")
                    Accessible.name: qsTr("%1 API key").arg(detail.provider ? detail.provider.name : "")
                    onAccepted: if (text.length > 0 && !detail.busy) detail.saveKey(true)
                }
                Flow {
                    width: parent.width
                    spacing: Theme.space(2)

                    PillButton {
                        objectName: "aiKeySave"
                        compact: true
                        variant: "primary"
                        enabled: keyField.text.length > 0 && !detail.busy
                        text: qsTr("Save and check")
                        onClicked: detail.saveKey(true)
                    }
                    PillButton {
                        objectName: "aiKeyCheck"
                        compact: true
                        enabled: detail.provider !== null && detail.provider.hasKey === true && !detail.busy
                        text: qsTr("Check")
                        tooltip: qsTr("Check the saved key again")
                        onClicked: detail.checkKey()
                    }
                    PillButton {
                        objectName: "aiKeyRemove"
                        compact: true
                        variant: "ghost"
                        enabled: detail.provider !== null && detail.provider.keySource === "store"
                        iconName: "trash-2"
                        text: qsTr("Remove")
                        onClicked: {
                            AiController.removeKey(detail.providerId)
                            detail.messageOk = true
                            detail.message = qsTr("The key was removed.")
                        }
                    }
                    PillButton {
                        objectName: "aiKeySaveUnchecked"
                        compact: true
                        variant: "ghost"
                        enabled: keyField.text.length > 0 && !detail.busy
                        text: qsTr("Save without checking")
                        tooltip: qsTr("For a proxy or a service whose check call is blocked")
                        onClicked: detail.saveKey(false)
                    }
                }
                Feedback {
                    objectName: "aiKeyMessage"
                    text: detail.message
                    ok: detail.messageOk
                    color: detail.messageOk ? Theme.success : Theme.dangerText
                }
            }

            // ---- The server address ----
            Card {
                objectName: "aiProviderServerCard"
                visible: detail.provider !== null && detail.provider.configurableBaseUrl === true
                title: qsTr("Server address")
                note: qsTr("Where the provider answers, e.g. a server on this computer or your network. Leave it empty for the default.")

                AiTextField {
                    id: urlField
                    objectName: "aiBaseUrlField"
                    width: parent.width
                    monospace: true
                    text: detail.provider ? detail.provider.baseUrl ?? "" : ""
                    placeholderText: qsTr("Server address")
                    Accessible.name: qsTr("%1 server address").arg(detail.provider ? detail.provider.name : "")
                    onAccepted: saveUrl.clicked()
                }
                Flow {
                    width: parent.width
                    spacing: Theme.space(2)

                    PillButton {
                        id: saveUrl
                        objectName: "aiBaseUrlSave"
                        compact: true
                        enabled: detail.provider !== null && urlField.text !== (detail.provider.baseUrl ?? "")
                        text: qsTr("Save address")
                        onClicked: {
                            detail.messageOk = AiController.setBaseUrl(detail.providerId, urlField.text.trim())
                            detail.message = detail.messageOk ? qsTr("Saved.") : qsTr("That is not a valid http or https address.")
                        }
                    }
                    PillButton {
                        compact: true
                        variant: "ghost"
                        text: qsTr("Use the default")
                        onClicked: {
                            AiController.setBaseUrl(detail.providerId, "")
                            urlField.text = Qt.binding(() => detail.provider ? detail.provider.baseUrl ?? "" : "")
                        }
                    }
                    PillButton {
                        objectName: "aiServerCheck"
                        visible: detail.provider !== null && detail.provider.keyRequired !== true
                        compact: true
                        enabled: !detail.busy
                        text: qsTr("Check server")
                        onClicked: detail.checkKey()
                    }
                }
                Feedback {
                    visible: text !== "" && detail.provider !== null && detail.provider.keyRequired !== true
                    text: detail.message
                    ok: detail.messageOk
                    color: detail.messageOk ? Theme.success : Theme.dangerText
                }
            }

            // ---- Models ----
            Card {
                objectName: "aiProviderModelsCard"
                visible: detail.provider !== null
                title: qsTr("Models")
                note: detail.models.length === 0 ? qsTr("No model listed yet. Add a model id, or fetch the models the provider offers.")
                                                 : qsTr("Use a model for a feature here, or choose per feature in Features.")

                Repeater {
                    model: detail.models
                    delegate: Rectangle {
                        id: modelRow
                        required property var modelData
                        readonly property var route: detail.catalog.route(modelRow.modelData.capability)
                        readonly property bool inUse: modelRow.route !== null && modelRow.route.model === modelRow.modelData.id
                                                      && modelRow.route.provider === detail.providerId

                        objectName: "aiProviderModel_" + modelRow.modelData.capability + "_" + modelRow.modelData.id
                        width: parent.width
                        height: modelColumn.implicitHeight + Theme.space(4)
                        radius: Theme.radiusField
                        color: Theme.sunken

                        Column {
                            id: modelColumn
                            x: Theme.space(3)
                            y: Theme.space(2)
                            width: parent.width - Theme.space(6)
                            spacing: Theme.space(1.5)

                            Flow {
                                width: parent.width
                                spacing: Theme.space(2)

                                Text {
                                    width: Math.min(implicitWidth, parent.width)
                                    text: modelRow.modelData.name
                                    elide: Text.ElideRight
                                    color: Theme.textPrimary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textMd
                                    font.weight: Theme.weightLabel
                                }
                                Text {
                                    width: Math.min(implicitWidth, parent.width)
                                    visible: modelRow.modelData.name !== modelRow.modelData.id
                                    text: modelRow.modelData.id
                                    elide: Text.ElideMiddle
                                    color: Theme.textTertiary
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.textXs
                                    topPadding: 2
                                }
                            }
                            Flow {
                                width: parent.width
                                spacing: Theme.space(2)

                                AiBadge {
                                    iconName: detail.catalog.capabilityIcon(modelRow.modelData.capability)
                                    text: detail.catalog.capabilityName(modelRow.modelData.capability)
                                }
                                Text {
                                    visible: (modelRow.modelData.price ?? "") !== ""
                                    text: modelRow.modelData.price ?? ""
                                    color: Theme.textSecondary
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.textXs
                                }
                                Text {
                                    visible: modelRow.modelData.usable === false && (modelRow.modelData.reason ?? "") !== ""
                                    text: modelRow.modelData.reason ?? ""
                                    color: Theme.warningText
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textXs
                                }
                            }
                            Flow {
                                width: parent.width
                                spacing: Theme.space(2)

                                PillButton {
                                    objectName: "aiUseModel_" + modelRow.modelData.capability + "_" + modelRow.modelData.id
                                    compact: true
                                    variant: modelRow.inUse ? "secondary" : "ghost"
                                    enabled: !modelRow.inUse
                                    iconName: modelRow.inUse ? "check" : ""
                                    text: modelRow.inUse ? qsTr("Used for %1").arg(detail.catalog.capabilityName(modelRow.modelData.capability).toLowerCase())
                                                         : qsTr("Use for %1").arg(detail.catalog.capabilityName(modelRow.modelData.capability).toLowerCase())
                                    onClicked: {
                                        const ok = detail.catalog.setDefault(modelRow.modelData.capability, modelRow.modelData.id)
                                        detail.modelMessageOk = ok
                                        detail.modelMessage = ok ? qsTr("%1 now uses %2.").arg(detail.catalog.capabilityName(modelRow.modelData.capability)).arg(modelRow.modelData.name)
                                                                 : qsTr("%1 cannot be chosen here yet.").arg(modelRow.modelData.name)
                                    }
                                }
                                PillButton {
                                    visible: modelRow.modelData.userAdded === true
                                    compact: true
                                    variant: "ghost"
                                    iconName: "x"
                                    text: qsTr("Remove")
                                    onClicked: AiController.removeModel(modelRow.modelData.id)
                                }
                            }
                        }
                    }
                }

                // Add any model id, for one of the provider's capabilities.
                Flow {
                    objectName: "aiAddModelRow"
                    visible: detail.provider !== null && (detail.provider.custom === true || detail.provider.configurableBaseUrl === true
                                                          || detail.provider.api === "chat-completions" || detail.provider.caps.length > 0)
                    width: parent.width
                    spacing: Theme.space(2)

                    AiTextField {
                        id: modelField
                        objectName: "aiAddModelField"
                        width: Math.max(Theme.space(40), parent.width - addCapability.width - addButton.width - 2 * parent.spacing)
                        monospace: true
                        placeholderText: qsTr("Model id")
                        Accessible.name: qsTr("Model id to add")
                        onAccepted: addButton.clicked()
                    }
                    AiChoiceButton {
                        id: addCapability
                        objectName: "aiAddModelCapability"
                        visible: options.length > 1
                        width: visible ? implicitWidth : 0
                        options: detail.provider ? (detail.provider.addable ?? detail.provider.caps).map(c => ({ id: c, name: detail.catalog.capabilityName(c) })) : []
                        placeholder: options.length > 0 ? options[0].name : ""
                        tooltip: qsTr("What the model is for")
                        onChosen: id => addCapability.current = id
                    }
                    PillButton {
                        id: addButton
                        objectName: "aiAddModelButton"
                        compact: true
                        enabled: modelField.text.trim() !== ""
                        iconName: "plus"
                        text: qsTr("Add")
                        onClicked: {
                            const options = addCapability.options
                            const capabilityId = addCapability.current !== "" ? addCapability.current
                                               : options.length > 0 ? options[0].id : "chat"
                            if (detail.addModel(modelField.text, capabilityId))
                                modelField.clear()
                        }
                    }
                }
                PillButton {
                    objectName: "aiFetchModels"
                    visible: detail.canFetch
                    compact: true
                    variant: "ghost"
                    iconName: "download"
                    enabled: !detail.fetching
                    text: detail.fetching ? qsTr("Fetching…") : qsTr("Fetch models")
                    tooltip: qsTr("List the models the provider offers")
                    onClicked: detail.fetchModels()
                }
                Flow {
                    objectName: "aiRemoteModels"
                    visible: detail.remoteModels.length > 0
                    width: parent.width
                    spacing: Theme.space(1.5)

                    Repeater {
                        model: detail.remoteModels.slice(0, 40)
                        delegate: PillButton {
                            required property var modelData
                            compact: true
                            variant: "ghost"
                            iconName: "plus"
                            text: modelData.id
                            tooltip: qsTr("Add for %1").arg(detail.catalog.capabilityName(modelData.capability ?? "chat").toLowerCase())
                            onClicked: {
                                if (detail.addModel(modelData.id, modelData.capability ?? "chat"))
                                    detail.remoteModels = detail.remoteModels.filter(m => m.id !== modelData.id)
                            }
                        }
                    }
                }
                Feedback {
                    objectName: "aiModelMessage"
                    text: detail.modelMessage
                    ok: detail.modelMessageOk
                }
            }

            // ---- Data ----
            Card {
                visible: detail.provider !== null && (detail.provider.dataPolicy ?? "") !== ""
                title: qsTr("Your data")

                Text {
                    width: parent.width
                    text: detail.provider ? detail.provider.dataPolicy ?? "" : ""
                    wrapMode: Text.WordWrap
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Text {
                    width: parent.width
                    text: qsTr("Wabcut asks before the first upload to a provider and shows what is sent, where, and what it may cost.")
                    wrapMode: Text.WordWrap
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
            }

            // ---- Spending ----
            Card {
                objectName: "aiProviderSpendCard"
                visible: detail.cloud
                title: qsTr("Spending")
                note: detail.spendRow ? qsTr("This month: %1.").arg(detail.spendRow.spent) : qsTr("Nothing spent this month.")

                Flow {
                    width: parent.width
                    spacing: Theme.space(2)

                    Text {
                        height: Theme.controlHeightSmall
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Monthly cap in $ (0 = none)")
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    NumberField {
                        objectName: "aiDetailCapField"
                        width: Theme.numberFieldWidth + Theme.space(6)
                        value: detail.spendRow ? detail.spendRow.capDollars : 0
                        from: 0
                        to: 100000
                        stepSize: 1
                        decimals: 2
                        Accessible.name: qsTr("Monthly cap for %1 in dollars, 0 for none").arg(detail.provider ? detail.provider.name : "")
                        onValueModified: value => AiController.setMonthlyCap(detail.providerId, value)
                    }
                    PillButton {
                        compact: true
                        variant: "ghost"
                        text: qsTr("See usage")
                        onClicked: detail.usageRequested()
                    }
                }
            }
        }
    }
}
