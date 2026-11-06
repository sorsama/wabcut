import QtQuick

// What Settings > AI shows, read from AiController in one place (r07): the AI capabilities with their icons; what each
// one uses right now (routing); the models a capability can use; the providers with a list group; and helpers to pick
// a default or use a model by id. Every page of Settings > AI reads the platform through this object, so the pages stay
// simple and agree with each other.
QtObject {
    id: catalog

    /// Icons per capability id (AiController.capabilityList's ids).
    readonly property var capabilityIcons: ({
        chat: "message-square", transcribe: "captions", translate: "arrow-left-right", speak: "volume-2",
        image: "image", video: "film", sound: "music"
    })
    /// [{id, name, description, icon}] in AiController.capabilityList's order.
    readonly property var capabilities: AiController.capabilityList.map(c => ({
        id: c.id, name: c.name, description: c.description ?? "", icon: catalog.capabilityIcons[c.id] ?? "sparkles"
    }))
    /// One row per capability: {capability, name, model, modelName, provider, providerName, usable, reason, local,
    /// automatic}.
    readonly property var routing: AiController.routing

    /// The providers with a list group: "connected" (a key, a working check, or the built-in local assistant),
    /// "custom", "cloud" or "local" (local and self-hosted servers). `caps` = the capability ids.
    readonly property var providers: {
        const list = AiController.providers
        const out = []
        for (let i = 0; i < list.length; ++i) {
            const p = Object.assign({}, list[i])
            const caps = []
            const source = p.capabilities ?? []
            for (let j = 0; j < source.length; ++j)
                caps.push(source[j])
            p.caps = caps
            // QA-05: the kinds of model "Add model id" takes (an OpenAI-compatible host takes every OpenAI path).
            const addable = []
            const addableSource = p.addableCapabilities ?? source
            for (let j = 0; j < addableSource.length; ++j)
                addable.push(addableSource[j])
            p.addable = addable
            p.custom = p.custom === true
            p.description = p.description ?? ""
            p.website = p.website ?? ""
            p.connected = p.hasKey === true || p.status === "ready" || (p.local === true && p.configurableBaseUrl !== true)
            p.group = p.custom ? "custom" : p.connected ? "connected" : p.category === "local" ? "local" : "cloud"
            out.push(p)
        }
        return out
    }
    readonly property var groupNames: ({
        connected: qsTr("Connected"), custom: qsTr("Your custom providers"), cloud: qsTr("Cloud providers"),
        local: qsTr("On this computer or your server")
    })
    readonly property var groupOrder: ["connected", "custom", "cloud", "local"]
    /// The custom provider protocols ([{id, name, capabilities, description}]).
    readonly property var protocols: AiController.customProtocols

    function capability(id: string): var {
        return catalog.capabilities.find(c => c.id === id) ?? { id: id, name: id, description: "", icon: "sparkles" }
    }
    function capabilityName(id: string): string {
        return catalog.capability(id).name
    }
    function capabilityIcon(id: string): string {
        return catalog.capabilityIcons[id] ?? "sparkles"
    }
    function route(capabilityId: string): var {
        return catalog.routing.find(r => r.capability === capabilityId) ?? null
    }
    function provider(id: string): var {
        return catalog.providers.find(p => p.id === id) ?? null
    }
    function stateColor(state: string): color {
        return state === "ready" ? Theme.success
             : state === "local" ? Theme.info
             : state === "error" ? Theme.danger
             : state === "notSupported" ? Theme.warning
             : Theme.textTertiary
    }

    /// The models `capabilityId` can use (local first).
    function modelsFor(capabilityId: string): var {
        return AiController.modelsFor(capabilityId)
    }
    /// Every (model, capability) pair of one provider, in the capabilities' order.
    function modelsOf(providerId: string): var {
        const out = []
        for (let i = 0; i < catalog.capabilities.length; ++i) {
            const cap = catalog.capabilities[i].id
            const list = AiController.modelsFor(cap)
            for (let j = 0; j < list.length; ++j) {
                if (list[j].provider === providerId)
                    out.push(Object.assign({ capability: cap }, list[j]))
            }
        }
        return out
    }
    /// The model a capability uses by default ("" = automatic).
    function defaultModel(capabilityId: string): string {
        return AiController.defaultModel(capabilityId)
    }
    /// Makes `modelId` ("" = automatic) the default of `capabilityId`. False when it cannot.
    function setDefault(capabilityId: string, modelId: string): bool {
        return AiController.setDefaultModel(capabilityId, modelId)
    }
    /// Adds `modelId` to a provider for `capabilityId` (a no-op when it is listed already), then makes it the default.
    /// Returns "" or what went wrong.
    function useModelId(capabilityId: string, providerId: string, modelId: string): string {
        const id = modelId.trim()
        if (id === "" || providerId === "")
            return qsTr("Choose a provider and type the model's id.")
        const known = AiController.modelsFor(capabilityId).find(m => (m.id === id || m.name === id) && m.provider === providerId)
        if (!known && !AiController.addModel(providerId, id, capabilityId))
            return qsTr("%1 cannot take that model id.").arg(catalog.provider(providerId)?.name ?? providerId)
        // A name another provider already uses is registered as "<provider>:<name>": find what was added.
        const added = known ?? AiController.modelsFor(capabilityId).find(m => (m.id === id || m.name === id) && m.provider === providerId)
        if (!AiController.setDefaultModel(capabilityId, added ? added.id : id))
            return qsTr("%1 cannot be used for %2.").arg(id).arg(catalog.capabilityName(capabilityId).toLowerCase())
        return ""
    }
    function addModel(providerId: string, modelId: string, capabilityId: string): bool {
        return AiController.addModel(providerId, modelId, capabilityId)
    }
    /// The providers that can serve `capabilityId` (for "Other model id"), local ones first.
    function providersFor(capabilityId: string): var {
        const list = catalog.providers.filter(p => p.caps.indexOf(capabilityId) >= 0)
        return list.filter(p => p.category === "local").concat(list.filter(p => p.category !== "local"))
    }
}
