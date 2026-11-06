pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Settings > AI > Providers > Custom provider (r07): adds or edits a provider of the user's own, so every AI feature
// can use any service or server that speaks a known protocol. Easy: a name, the protocol (OpenAI-, Anthropic- or
// Gemini-compatible), the base URL, the key and the model ids (each with what it is for, or fetched from the server).
// Advanced: how the key is sent (Bearer, a header of its own, none), per-capability paths and extra headers. Test
// connection and Fetch models check the form as it is WITHOUT saving anything (AiController.checkCustomProvider /
// listCustomProviderModels; QA-08): a saved provider only changes on Save. Save checks a new key first and keeps the
// form open when the check fails, with "Save without checking" for a server that cannot be reached yet; then it keeps
// the provider (AiController.saveCustomProvider; the key goes to the key store through saveKey, never into settings).
// A provider on this computer (localhost) counts as local.
WabcutDialog {
    id: dialog
    objectName: "aiCustomProviderDialog"

    required property var catalog
    /// The provider being edited ("" = a new one).
    property string providerId: ""
    property string error: ""
    property string testMessage: ""
    property bool testOk: true
    property bool testing: false
    /// QA-08: the check (Test connection, or Save's key check) and the model listing being waited for.
    property string checkRequest: ""
    property string listRequest: ""
    /// Save is waiting for its key check; it failed, so "Save without checking" is offered.
    property bool savePending: false
    property bool offerUnchecked: false
    property bool advanced: false
    /// [{id, capability}] and [{name, value}] being edited.
    property var models: []
    property var headers: []
    /// {capability: path}
    property var paths: ({})
    property string authStyle: "bearer"

    readonly property var protocol: dialog.catalog.protocols[protocolTabs.currentIndex] ?? dialog.catalog.protocols[0]
    readonly property var protocolCaps: dialog.protocol ? Array.from(dialog.protocol.capabilities ?? ["chat"]) : ["chat"]
    readonly property bool editing: dialog.providerId !== ""

    /// The provider was saved (`id`): the page selects it.
    signal saved(string id)
    /// Delete… (the page asks first).
    signal deleteRequested(string id)

    /// Opens the dialog for a new provider ("") or to edit `id`.
    function openFor(id: string) {
        dialog.providerId = id
        dialog.error = ""
        dialog.testMessage = ""
        dialog.testing = false
        dialog.checkRequest = ""
        dialog.listRequest = ""
        dialog.savePending = false
        dialog.offerUnchecked = false
        keyField.clear()
        const spec = id !== "" ? AiController.customProvider(id) : ({})
        nameField.text = spec.name ?? ""
        const protocolIndex = dialog.catalog.protocols.findIndex(p => p.id === (spec.protocol ?? "openai"))
        protocolTabs.currentIndex = Math.max(0, protocolIndex)
        urlField.text = spec.baseUrl ?? ""
        dialog.authStyle = spec.auth ?? "bearer"
        headerField.text = spec.authHeader ?? ""
        const models = []
        const specModels = spec.models ?? []
        for (let i = 0; i < specModels.length; ++i)
            models.push({ id: specModels[i].id ?? "", capability: specModels[i].capability ?? "chat" })
        dialog.models = models.length > 0 ? models : [{ id: "", capability: "chat" }]
        const headers = []
        const specHeaders = spec.headers ?? {}
        for (const name in specHeaders)
            headers.push({ name: name, value: String(specHeaders[name]) })
        dialog.headers = headers
        dialog.paths = Object.assign({}, spec.paths ?? {})
        dialog.advanced = dialog.authStyle !== "bearer" || headers.length > 0 || Object.keys(dialog.paths).length > 0
        dialog.open()
        nameField.forceActiveFocus(Qt.TabFocusReason)
    }

    /// What saveCustomProvider takes.
    function spec(): var {
        const models = []
        for (let i = 0; i < dialog.models.length; ++i) {
            const id = String(dialog.models[i].id).trim()
            if (id !== "")
                models.push({ id: id, capability: dialog.models[i].capability })
        }
        const headers = {}
        for (let i = 0; i < dialog.headers.length; ++i) {
            const name = String(dialog.headers[i].name).trim()
            if (name !== "")
                headers[name] = String(dialog.headers[i].value)
        }
        const paths = {}
        for (const cap in dialog.paths) {
            const path = String(dialog.paths[cap]).trim()
            if (path !== "" && dialog.protocolCaps.indexOf(cap) >= 0)
                paths[cap] = path
        }
        const out = { name: nameField.text.trim(), protocol: dialog.protocol.id, baseUrl: urlField.text.trim(), auth: dialog.authStyle,
                      authHeader: dialog.authStyle === "header" ? headerField.text.trim() : "", headers: headers, models: models,
                      paths: paths }
        if (dialog.editing)
            out.id = dialog.providerId
        return out
    }

    /// "" when the form can be saved, else what to fix (shown under the form).
    function validate(): string {
        const s = dialog.spec()
        if (s.name === "")
            return qsTr("Give the provider a name.")
        if (!/^https?:\/\/[^\s\/?#]+/i.test(s.baseUrl))
            return qsTr("The base URL must start with http:// or https://, e.g. https://api.example.com/v1.")
        if (/\s/.test(s.baseUrl))
            return qsTr("The base URL cannot contain spaces.")
        const token = /^[A-Za-z0-9!#$%&'*+.^_`|~-]+$/
        if (s.auth === "header" && s.authHeader !== "" && !token.test(s.authHeader))
            return qsTr("%1 is not a valid header name for the key.").arg(s.authHeader)
        const seen = {}
        for (let i = 0; i < s.models.length; ++i) {
            const key = s.models[i].capability + "/" + s.models[i].id
            if (seen[key] === true)
                return qsTr("%1 is listed twice for the same feature.").arg(s.models[i].id)
            seen[key] = true
            if (/\s/.test(s.models[i].id))
                return qsTr("Model ids cannot contain spaces: %1").arg(s.models[i].id)
        }
        for (const name in s.headers) {
            if (!token.test(name))
                return qsTr("%1 is not a valid header name.").arg(name)
            if (/[\r\n]/.test(s.headers[name]))
                return qsTr("Header values must be on one line.")
        }
        for (const cap in s.paths) {
            if (s.paths[cap].charAt(0) !== "/")
                return qsTr("Paths start with /, e.g. /v1/chat/completions.")
        }
        return ""
    }

    /// Saves (creates or updates) the provider; returns its id, or "" with `error` set.
    function store(): string {
        dialog.error = dialog.validate()
        if (dialog.error !== "")
            return ""
        const result = AiController.saveCustomProvider(dialog.spec())
        if (!result || result.ok !== true) {
            dialog.error = result && result.error ? result.error : qsTr("The provider could not be saved.")
            return ""
        }
        return result.id
    }
    /// Whether a key was typed that Save has to check and store.
    function newKey(): bool {
        return keyField.text.trim().length > 0 && dialog.authStyle !== "none"
    }
    /// Checks the form as it is (nothing is saved); `forSave`: Save waits for it.
    function check(forSave: bool) {
        dialog.error = dialog.validate()
        if (dialog.error !== "")
            return
        dialog.offerUnchecked = false
        const result = AiController.checkCustomProvider(dialog.spec(), keyField.text)
        if (!result || result.ok !== true) {
            dialog.testOk = false
            dialog.testMessage = result && result.error ? result.error : qsTr("The provider could not be checked.")
            dialog.offerUnchecked = forSave
            return
        }
        dialog.checkRequest = result.request
        dialog.savePending = forSave
        dialog.testing = true
        dialog.testOk = true
        dialog.testMessage = qsTr("Checking…")
    }
    /// Save: a new key is checked first (QA-08); without one the provider is saved at once.
    function save() {
        if (dialog.newKey())
            dialog.check(true)
        else
            dialog.commit(false)
    }
    /// Saves after a failed check, at the user's word.
    function saveWithoutChecking() {
        dialog.commit(false)
    }
    /// Saves the provider and stores the typed key (checked again against the saved provider when `checked`: the
    /// provider detail then says "Key saved and checked").
    function commit(checked: bool) {
        const id = dialog.store()
        if (id === "")
            return
        if (dialog.newKey())
            AiController.saveKey(id, keyField.text, !checked)
        keyField.clear()
        dialog.savePending = false
        dialog.offerUnchecked = false
        dialog.close()
        dialog.saved(id)
    }
    function testConnection() {
        dialog.check(false)
    }
    /// Closes without saving (Cancel, Escape): nothing was changed.
    function cancel() {
        dialog.close()
    }
    function setModel(index: int, field: string, value: string) {
        const copy = dialog.models.map(m => Object.assign({}, m))
        copy[index][field] = value
        dialog.models = copy
    }
    function setHeader(index: int, field: string, value: string) {
        const copy = dialog.headers.map(h => Object.assign({}, h))
        copy[index][field] = value
        dialog.headers = copy
    }

    title: dialog.editing ? qsTr("Edit custom provider") : qsTr("Add a custom provider")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))
    onClosed: {
        dialog.testing = false
        dialog.savePending = false
        dialog.checkRequest = ""
        dialog.listRequest = ""
    }

    Connections {
        target: AiController
        function onCustomProviderChecked(request: string, ok: bool, message: string) {
            if (request === "" || request !== dialog.checkRequest)
                return
            dialog.checkRequest = ""
            dialog.testing = false
            dialog.testOk = ok
            dialog.testMessage = ok ? qsTr("Connected. %1").arg(message) : message
            if (!dialog.savePending)
                return
            dialog.savePending = false
            if (ok)
                dialog.commit(true)
            else
                dialog.offerUnchecked = true
        }
    }

    // A label above a field.
    component FieldLabel: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Theme.weightLabel
    }
    component Hint: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }

    Flickable {
        id: flick
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(form.implicitHeight,
                                         (dialog.parent ? dialog.parent.height : Theme.windowDefaultHeight) - Theme.space(64))
        contentWidth: width
        contentHeight: form.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: form
            width: flick.width - Theme.space(3)
            spacing: Theme.space(2)

            FieldLabel { text: qsTr("Name") }
            AiTextField {
                id: nameField
                objectName: "aiCustomName"
                Layout.fillWidth: true
                placeholderText: qsTr("e.g. My company's gateway")
                Accessible.name: qsTr("Provider name")
            }

            FieldLabel { text: qsTr("Speaks"); Layout.topMargin: Theme.space(2) }
            PillTabs {
                id: protocolTabs
                objectName: "aiCustomProtocol"
                Layout.fillWidth: true
                compact: true
                model: dialog.catalog.protocols.map(p => p.name)
                Accessible.name: qsTr("The API the provider speaks")
            }
            Hint {
                text: dialog.protocol.id === "openai"
                      ? qsTr("Most services and local servers (vLLM, LiteLLM, LocalAI, Ollama, LM Studio) speak this. It covers the assistant, transcription, translation, voice, images and video, and sound effects from servers with a sound-generation endpoint (LocalAI).")
                      : qsTr("For a gateway, proxy or self-hosted service that speaks this API (assistant only).")
            }

            FieldLabel { text: qsTr("Base URL"); Layout.topMargin: Theme.space(2) }
            AiTextField {
                id: urlField
                objectName: "aiCustomBaseUrl"
                Layout.fillWidth: true
                monospace: true
                placeholderText: qsTr("https://api.example.com/v1")
                Accessible.name: qsTr("Base URL")
            }
            Hint { text: qsTr("A server on this computer (localhost) counts as local AI: nothing leaves the computer.") }

            FieldLabel { text: qsTr("API key"); Layout.topMargin: Theme.space(2); visible: dialog.authStyle !== "none" }
            AiTextField {
                id: keyField
                objectName: "aiCustomKey"
                visible: dialog.authStyle !== "none"
                Layout.fillWidth: true
                masked: true
                placeholderText: dialog.editing ? qsTr("Paste a new key to replace the saved one") : qsTr("Paste API key (optional)")
                Accessible.name: qsTr("API key")
            }
            Hint {
                visible: dialog.authStyle !== "none"
                text: qsTr("Kept in %1, never in projects, settings or logs.").arg(AiController.keyStoreName)
            }

            // Models, each with what it is for.
            FieldLabel { text: qsTr("Models"); Layout.topMargin: Theme.space(2) }
            Repeater {
                model: dialog.models
                delegate: RowLayout {
                    id: modelRow
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: Theme.space(2)

                    AiTextField {
                        objectName: "aiCustomModelId_" + modelRow.index
                        Layout.fillWidth: true
                        Layout.minimumWidth: Theme.space(30)
                        monospace: true
                        text: modelRow.modelData.id
                        placeholderText: qsTr("Model id")
                        Accessible.name: qsTr("Model id %1").arg(modelRow.index + 1)
                        onTextEdited: dialog.setModel(modelRow.index, "id", text)
                    }
                    AiChoiceButton {
                        objectName: "aiCustomModelCapability_" + modelRow.index
                        Layout.preferredWidth: Theme.space(36)
                        maximumWidth: Theme.space(36)
                        options: dialog.protocolCaps.map(c => ({ id: c, name: dialog.catalog.capabilityName(c) }))
                        current: modelRow.modelData.capability
                        tooltip: qsTr("What model %1 is for").arg(modelRow.index + 1)
                        onChosen: id => dialog.setModel(modelRow.index, "capability", id)
                    }
                    IconButton {
                        compact: true
                        iconName: "x"
                        tooltip: qsTr("Remove this model")
                        onClicked: dialog.models = dialog.models.filter((m, i) => i !== modelRow.index)
                    }
                }
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(2)

                PillButton {
                    objectName: "aiCustomAddModel"
                    compact: true
                    variant: "ghost"
                    iconName: "plus"
                    text: qsTr("Add model")
                    onClicked: dialog.models = dialog.models.concat([{ id: "", capability: dialog.protocolCaps[0] ?? "chat" }])
                }
                PillButton {
                    objectName: "aiCustomFetchModels"
                    compact: true
                    variant: "ghost"
                    iconName: "download"
                    visible: dialog.protocol.id === "openai"
                    enabled: dialog.listRequest === ""
                    text: dialog.listRequest !== "" ? qsTr("Fetching…") : qsTr("Fetch models")
                    tooltip: qsTr("Ask the server which models it offers (nothing is saved)")
                    onClicked: {
                        dialog.error = dialog.validate()
                        if (dialog.error !== "")
                            return
                        const result = AiController.listCustomProviderModels(dialog.spec(), keyField.text)
                        if (!result || result.ok !== true) {
                            dialog.testOk = false
                            dialog.testMessage = result && result.error ? result.error : qsTr("The models could not be listed.")
                            return
                        }
                        dialog.listRequest = result.request
                    }
                }
            }
            Connections {
                target: AiController
                function onRemoteModelsListed(providerId: string, ok: bool, models: var, message: string) {
                    if (providerId === "" || providerId !== dialog.listRequest)
                        return
                    dialog.listRequest = ""
                    if (!ok) {
                        dialog.testOk = false
                        dialog.testMessage = message
                        return
                    }
                    const have = dialog.models.filter(m => String(m.id).trim() !== "")
                    const ids = have.map(m => m.id)
                    for (let i = 0; i < (models?.length ?? 0); ++i) {
                        if (ids.indexOf(models[i].id) < 0)
                            have.push({ id: models[i].id, capability: models[i].capability ?? "chat" })
                    }
                    dialog.models = have
                    dialog.testOk = true
                    dialog.testMessage = qsTr("%n model(s) listed: remove the ones you do not want.", "", models?.length ?? 0)
                }
            }

            // ---- Advanced ----
            PillButton {
                objectName: "aiCustomAdvanced"
                Layout.topMargin: Theme.space(2)
                compact: true
                variant: "ghost"
                iconName: dialog.advanced ? "chevron-up" : "chevron-down"
                text: qsTr("Advanced")
                Accessible.description: dialog.advanced ? qsTr("Expanded") : qsTr("Collapsed")
                onClicked: dialog.advanced = !dialog.advanced
            }
            ColumnLayout {
                visible: dialog.advanced
                Layout.fillWidth: true
                spacing: Theme.space(2)

                FieldLabel { text: qsTr("The key is sent as") }
                PillTabs {
                    id: authTabs
                    objectName: "aiCustomAuth"
                    readonly property var styles: ["bearer", "header", "none"]
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("Bearer token"), qsTr("A header of its own"), qsTr("No key")]
                    currentIndex: Math.max(0, authTabs.styles.indexOf(dialog.authStyle))
                    Accessible.name: qsTr("How the key is sent")
                    onActivated: index => dialog.authStyle = authTabs.styles[index]
                }
                AiTextField {
                    id: headerField
                    objectName: "aiCustomAuthHeader"
                    visible: dialog.authStyle === "header"
                    Layout.fillWidth: true
                    monospace: true
                    placeholderText: qsTr("Header name (empty = the usual one, e.g. api-key)")
                    Accessible.name: qsTr("Header the key goes in")
                }

                FieldLabel { text: qsTr("Paths per feature"); Layout.topMargin: Theme.space(2) }
                Hint { text: qsTr("Only when the server uses its own paths. Empty = the protocol's usual path after the base URL.") }
                Repeater {
                    model: dialog.protocolCaps
                    delegate: RowLayout {
                        id: pathRow
                        required property string modelData
                        Layout.fillWidth: true
                        spacing: Theme.space(2)

                        Text {
                            Layout.preferredWidth: Theme.space(28)
                            text: dialog.catalog.capabilityName(pathRow.modelData)
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        AiTextField {
                            objectName: "aiCustomPath_" + pathRow.modelData
                            Layout.fillWidth: true
                            monospace: true
                            text: dialog.paths[pathRow.modelData] ?? ""
                            placeholderText: pathRow.modelData === "chat" ? "/chat/completions"
                                           : pathRow.modelData === "transcribe" ? "/audio/transcriptions"
                                           : pathRow.modelData === "speak" ? "/audio/speech"
                                           : pathRow.modelData === "image" ? "/images/generations"
                                           : pathRow.modelData === "video" ? "/videos"
                                           : pathRow.modelData === "sound" ? "/sound-generation"
                                           : qsTr("default")
                            Accessible.name: qsTr("Path for %1").arg(dialog.catalog.capabilityName(pathRow.modelData))
                            onTextEdited: {
                                const copy = Object.assign({}, dialog.paths)
                                copy[pathRow.modelData] = text
                                dialog.paths = copy
                            }
                        }
                    }
                }

                FieldLabel { text: qsTr("Extra headers"); Layout.topMargin: Theme.space(2) }
                Repeater {
                    model: dialog.headers
                    delegate: RowLayout {
                        id: headerRow
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: Theme.space(2)

                        AiTextField {
                            Layout.preferredWidth: Theme.space(40)
                            Layout.fillWidth: true
                            monospace: true
                            text: headerRow.modelData.name
                            placeholderText: qsTr("Name")
                            Accessible.name: qsTr("Header %1 name").arg(headerRow.index + 1)
                            onTextEdited: dialog.setHeader(headerRow.index, "name", text)
                        }
                        AiTextField {
                            Layout.preferredWidth: Theme.space(60)
                            Layout.fillWidth: true
                            monospace: true
                            text: headerRow.modelData.value
                            placeholderText: qsTr("Value")
                            Accessible.name: qsTr("Header %1 value").arg(headerRow.index + 1)
                            onTextEdited: dialog.setHeader(headerRow.index, "value", text)
                        }
                        IconButton {
                            compact: true
                            iconName: "x"
                            tooltip: qsTr("Remove this header")
                            onClicked: dialog.headers = dialog.headers.filter((h, i) => i !== headerRow.index)
                        }
                    }
                }
                PillButton {
                    objectName: "aiCustomAddHeader"
                    compact: true
                    variant: "ghost"
                    iconName: "plus"
                    text: qsTr("Add header")
                    onClicked: dialog.headers = dialog.headers.concat([{ name: "", value: "" }])
                }
                Hint { text: qsTr("Header values are kept with the provider's settings: put secrets in the API key, not here.") }
            }
        }
    }

    Text {
        objectName: "aiCustomError"
        Layout.fillWidth: true
        visible: dialog.error !== ""
        text: dialog.error
        wrapMode: Text.WordWrap
        color: Theme.dangerText
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    Text {
        objectName: "aiCustomTestMessage"
        Layout.fillWidth: true
        visible: dialog.testMessage !== ""
        text: dialog.testMessage
        wrapMode: Text.WordWrap
        color: dialog.testOk ? Theme.success : Theme.dangerText
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "aiCustomDelete"
            visible: dialog.editing
            variant: "ghost"
            iconName: "trash-2"
            text: qsTr("Delete…")
            onClicked: {
                const id = dialog.providerId
                dialog.close()
                dialog.deleteRequested(id)
            }
        },
        PillButton {
            objectName: "aiCustomCancel"
            text: qsTr("Cancel")
            onClicked: dialog.cancel()
        },
        PillButton {
            objectName: "aiCustomTest"
            enabled: !dialog.testing
            text: dialog.testing && !dialog.savePending ? qsTr("Testing…") : qsTr("Test connection")
            onClicked: dialog.testConnection()
        },
        PillButton {
            objectName: "aiCustomSaveUnchecked"
            visible: dialog.offerUnchecked
            variant: "ghost"
            text: qsTr("Save without checking")
            tooltip: qsTr("Keeps the provider and the key; the key is checked on first use")
            onClicked: dialog.saveWithoutChecking()
        },
        PillButton {
            objectName: "aiCustomSave"
            variant: "primary"
            enabled: !dialog.testing
            text: dialog.savePending ? qsTr("Checking…") : qsTr("Save")
            onClicked: dialog.save()
        }
    ]
}
