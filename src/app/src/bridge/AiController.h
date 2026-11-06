#pragma once

// AiController: the QML bridge of the AI platform (owner: AI platform engineer; docs/ai.md "UI integration").
//
// A QML singleton over wabcut_ai / wabcut_ai_net. It owns the model and provider registries (with the cloud
// adapters), the key ring over the OS keychain (QtKeychain when built with WABCUT_WITH_CLOUD_AI, else this session
// only), the consent ledger, the spend guard, the redacted request log and the network stack, and persists the
// user's choices in QSettings (group "ai"). Keys never reach QSettings: only {hasKey, last4, validatedAt}.
//
// QML sees display values only (principle 9): ids, names, formatted prices ("$2.00 / $10.00 per 1M tokens"), money
// as "$1.23" strings and plain dollars for editing, ISO dates. Long work (key checks) runs on the thread pool and
// reports back with signals; nothing blocks the GUI thread except the OS keychain itself.
//
// Environment: WABCUT_AI_KEYSTORE=memory keeps keys in memory (tests, CI); WABCUT_<PROVIDER>_API_KEY overrides a
// stored key.

#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/SpendGuard.h>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <expected>
#include <memory>

class QJSEngine;
class QQmlEngine;

namespace wabcut::ai::net {
class NetworkStack;
}
namespace wabcut::app {
class ProjectController;
}

class AiController : public QObject {
    Q_OBJECT

    /// [{id, name, local, keyRequired, hasKey, keySource, masked, validatedAt, status, statusText, consoleUrl, termsUrl,
    ///   dataPolicy, baseUrl, configurableBaseUrl, verified, capabilities, capabilityNames, category, custom,
    ///   description, website, api, modelCount, models, lastError}]
    /// r07: `capabilities` = feature ids ["chat","speak",...] (its models' and its API's), `capabilityNames` their
    /// display names, `category` "cloud" | "local" | "custom", `status` "ready" | "needsKey" | "error" | "local" |
    /// "notSupported" with `statusText`; `models` = [{id, name, capabilities, userAdded}].
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged)
    /// r07: the routable features in display order: [{id, name, description}] for "chat", "transcribe", "translate",
    /// "speak", "image", "video", "sound".
    Q_PROPERTY(QVariantList capabilityList READ capabilityList CONSTANT)
    /// r07: what each feature uses right now, one row per capabilityList entry: [{capability, name, model, modelName,
    /// provider, providerName, usable, reason, local, automatic}]. `model` "" with `local` true for transcribe = whisper.cpp
    /// on this computer; translate without a choice of its own follows the assistant.
    Q_PROPERTY(QVariantList routing READ routing NOTIFY modelsChanged)
    /// r07: the wire protocols of custom providers: [{id, name, capabilities, description}] ("openai", "anthropic",
    /// "gemini").
    Q_PROPERTY(QVariantList customProtocols READ customProtocols CONSTANT)
    /// The installable local models with their fit on this machine. Computed when first read and again only when it
    /// can have changed (a download or start finished, refreshLocalModels()): it probes the GPUs and stats the weights,
    /// which must not run on the GUI thread every second (review wave 4, P2-3).
    Q_PROPERTY(QVariantList localModels READ localModels NOTIFY localModelsChanged)
    Q_PROPERTY(QString localStatus READ localStatus NOTIFY localChanged)
    Q_PROPERTY(bool localBusy READ localBusy NOTIFY localChanged)
    /// [{id, name, provider, providerName, price, badges, verified, notes, usable, reason, userAdded}]
    Q_PROPERTY(QVariantList chatModels READ chatModels NOTIFY modelsChanged)
    Q_PROPERTY(QVariantList transcriptionModels READ transcriptionModels NOTIFY modelsChanged)
    /// AI-12: the voice models (Dub, Narration), same fields.
    Q_PROPERTY(QVariantList speechModels READ speechModels NOTIFY modelsChanged)
    /// Selected model ids; "" for transcription means "on this computer" (local whisper.cpp, TranscriptionService).
    /// r07: chatModel reads the model the assistant uses now (routing) and writes the assistant's default
    /// (setDefaultModel("chat", id)); transcriptionModel is defaultModel("transcribe") ("" = automatic: this computer
    /// first).
    Q_PROPERTY(QString chatModel READ chatModel WRITE setChatModel NOTIFY modelsChanged)
    Q_PROPERTY(QString transcriptionModel READ transcriptionModel WRITE setTranscriptionModel NOTIFY modelsChanged)
    Q_PROPERTY(bool offlineMode READ offlineMode WRITE setOfflineMode NOTIFY privacyChanged)
    /// 0.2: the CPU cap of the AI helpers on this computer (transcription, analysis, segmentation, the local
    /// assistant), percent of the machine: 100 = no limit, 75 (the default), 50, 25. They always run below normal
    /// priority. Persisted (ai/helperCpuLimit); applies to helpers started afterwards (ai::setHelperCpuPolicy).
    Q_PROPERTY(int helperCpuLimit READ helperCpuLimit WRITE setHelperCpuLimit NOTIFY helperCpuLimitChanged FINAL)
    /// The open project's "Offline / no-cloud" lock (kept for the session until the project file can store it).
    Q_PROPERTY(bool projectLock READ projectLock WRITE setProjectLock NOTIFY privacyChanged)
    /// [{provider, providerName, capability, capabilityName, grantedAt}]
    Q_PROPERTY(QVariantList consents READ consents NOTIFY privacyChanged)
    /// [{provider, providerName, capped, capDollars, cap, spent, remaining, fraction}] for the current month
    Q_PROPERTY(QVariantList spend READ spend NOTIFY spendChanged)
    /// The newest redacted request-log entries, newest first: [{time, kind, what, status, cost, error}]
    Q_PROPERTY(QVariantList requestLog READ requestLog NOTIFY requestLogChanged)
    Q_PROPERTY(QString keyStoreName READ keyStoreName CONSTANT)
    Q_PROPERTY(bool keyStorePersistent READ keyStorePersistent CONSTANT)
    /// Compatibility property: always false. Persisted keys require the OS keychain (AI-10).
    Q_PROPERTY(bool plaintextKeysOptIn READ plaintextKeysOptIn WRITE setPlaintextKeysOptIn NOTIFY privacyChanged)
    /// Keys still in the plaintext file of the removed opt-in (AppConfigLocation/ai-keys.json); 0 = none (review
    /// wave 4, P2-5). Preferences > AI offers to move them into the key store and delete the file.
    Q_PROPERTY(int legacyPlaintextKeys READ legacyPlaintextKeys NOTIFY privacyChanged)
    /// The AI status pill: "Offline", "Local" or the chat provider's name.
    Q_PROPERTY(QString statusLabel READ statusLabel NOTIFY statusChanged)
    Q_PROPERTY(QString registryUpdated READ registryUpdated CONSTANT)
    /// Registry entries not verified against vendor documentation ("model:voxtral-mini-latest", ...).
    Q_PROPERTY(QStringList unverifiedEntries READ unverifiedEntries CONSTANT)

public:
    explicit AiController(QObject* parent = nullptr);
    ~AiController() override;

    /// QML singleton factory: also binds the project no-cloud lock to the engine's ProjectController.
    static AiController* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    /// The project whose no-cloud lock (ProjectState::aiNoCloud) applies: read from its live session before every
    /// provider call, and written there by setProjectLock(). Without one the lock is kept in memory only.
    void setProjectController(wabcut::app::ProjectController* controller);

    QVariantList localModels() const;
    /// Probes again and re-reads what is installed (the Local page calls it when it opens).
    Q_INVOKABLE void refreshLocalModels();
    QString localStatus() const;
    bool localBusy() const;
    Q_INVOKABLE void downloadLocal(const QString& modelId);
    Q_INVOKABLE void startLocal(const QString& modelId);
    Q_INVOKABLE void stopLocal();
    void localPressure(bool busy);
    QVariantList providers() const;
    QVariantList chatModels() const;
    QVariantList transcriptionModels() const;
    QVariantList speechModels() const;
    QString chatModel() const;
    void setChatModel(const QString& id);
    QString transcriptionModel() const;
    void setTranscriptionModel(const QString& id);
    bool offlineMode() const;
    void setOfflineMode(bool offline);
    int helperCpuLimit() const;
    void setHelperCpuLimit(int percent);
    bool projectLock() const;
    void setProjectLock(bool locked);
    QVariantList consents() const;
    QVariantList spend() const;
    QVariantList requestLog() const;
    QString keyStoreName() const;
    bool keyStorePersistent() const;
    bool plaintextKeysOptIn() const;
    int legacyPlaintextKeys() const;
    /// Moves the legacy plaintext keys into the key store, then overwrites and deletes the file. Returns what happened.
    Q_INVOKABLE QString importPlaintextKeys();
    /// Overwrites and deletes the legacy plaintext key file without importing it.
    Q_INVOKABLE bool deletePlaintextKeys();
    /// The legacy plaintext key file's path.
    std::filesystem::path legacyKeyFile() const;
    void setPlaintextKeysOptIn(bool optIn);
    QString statusLabel() const;
    QString registryUpdated() const;
    QStringList unverifiedEntries() const;

    /// Checks `key` with the provider's cheap authenticated call, then stores it (unless the check fails). With
    /// `skipCheck` it is stored without a network call. Emits keyCheckFinished. The key is never logged or echoed.
    Q_INVOKABLE void saveKey(const QString& providerId, const QString& key, bool skipCheck = false);
    /// Re-checks the stored key (or, for local servers, that the server answers). Emits keyCheckFinished.
    Q_INVOKABLE void testKey(const QString& providerId);
    Q_INVOKABLE void removeKey(const QString& providerId);
    /// For local / OpenAI-compatible providers; "" restores the default. False (with keyCheckFinished) on a bad URL.
    Q_INVOKABLE bool setBaseUrl(const QString& providerId, const QString& url);
    /// Adds a model id to a provider whose models the user defines (Ollama tags, OpenRouter models, ...). r07: for
    /// `capability` (a feature id, "chat" by default); on a custom provider the model joins its spec.
    Q_INVOKABLE bool addModel(const QString& providerId, const QString& modelId,
                              const QString& capability = QStringLiteral("chat"));
    Q_INVOKABLE bool removeModel(const QString& modelId);

    // --- r07: per-feature routing ---------------------------------------------------------------------------------
    QVariantList capabilityList() const;
    QVariantList routing() const;
    /// The models that can serve `capability` (a feature id): the chatModels row fields plus `local` and `capability`,
    /// local ones first.
    Q_INVOKABLE QVariantList modelsFor(const QString& capability) const;
    /// The user's default for a feature; "" = automatic (local first where it exists, then the first usable).
    /// Persisted. False for an unknown feature or a model that cannot serve it.
    Q_INVOKABLE bool setDefaultModel(const QString& capability, const QString& modelId);
    Q_INVOKABLE QString defaultModel(const QString& capability) const;
    /// C++ callers (feature bridges): the model id a feature uses now ("" when none is usable; for transcribe ""
    /// also means this computer), as `routing` shows it.
    QString routedModel(const QString& capability) const;

    // --- r07: custom providers ------------------------------------------------------------------------------------
    QVariantList customProtocols() const;
    /// Creates (no `id`) or updates (`id` of a custom provider) a custom provider from {id?, name, protocol, baseUrl,
    /// auth: "bearer" | "header" | "none", authHeader, headers: {name: value}, models: [{id, capability}],
    /// paths: {capability: "/path"}}. Returns {ok, id, error}. The key goes through saveKey(id, key, skipCheck).
    Q_INVOKABLE QVariantMap saveCustomProvider(const QVariantMap& spec);
    /// The spec of a custom provider for editing (never the key); {} for other ids.
    Q_INVOKABLE QVariantMap customProvider(const QString& id) const;
    /// Removes a custom provider with its key, models and consents; defaults that pointed at it become automatic.
    Q_INVOKABLE bool removeCustomProvider(const QString& id);
    /// Lists the provider's models (GET {baseUrl}/models) off the GUI thread; answers with remoteModelsListed.
    Q_INVOKABLE void listRemoteModels(const QString& providerId);
    /// r07 (QA-08): checks the custom provider `spec` describes (what saveCustomProvider takes) with `key` (empty: the
    /// saved provider's key when spec.id names one) WITHOUT saving or changing anything: its key check off the GUI
    /// thread. Returns {ok, request, error}: ok false with `error` when it cannot be checked (an invalid spec, no key);
    /// else the answer comes as customProviderChecked(request, ...).
    Q_INVOKABLE QVariantMap checkCustomProvider(const QVariantMap& spec, const QString& key);
    /// r07 (QA-08): listRemoteModels() for an unsaved spec (nothing is saved); the answer comes as
    /// remoteModelsListed(request, ...). Returns {ok, request, error} like checkCustomProvider.
    Q_INVOKABLE QVariantMap listCustomProviderModels(const QVariantMap& spec, const QString& key);
    /// Monthly cap in dollars; 0 or less removes the cap.
    Q_INVOKABLE void setMonthlyCap(const QString& providerId, double dollars);
    /// What the first-use consent prompt shows: {provider, providerName, capabilityName, destination, payload,
    /// termsUrl, dataPolicy, cost, needed}. `capability` is "chat", "transcribe", ...
    Q_INVOKABLE QVariantMap consentPrompt(const QString& modelId, const QString& capability, const QString& payload) const;
    Q_INVOKABLE void grantConsent(const QString& providerId, const QString& capability);
    Q_INVOKABLE void revokeConsent(const QString& providerId, const QString& capability);
    Q_INVOKABLE void revokeAllConsents();
    Q_INVOKABLE void clearRequestLog();

    /// r07, the local model library (LocalModelsController): adds, replaces or removes a downloaded GGUF model of the
    /// local assistant (provider "local-llm") so the model pickers and routing list it. Not kept in "userModels": the
    /// library's own index brings them back at start. False for a built-in model or another provider.
    bool registerLocalModel(wabcut::ai::ModelSpec spec);
    bool unregisterLocalModel(const QString& modelId);
    /// r07 (QA-10): the models registered by the local model library now.
    QStringList libraryModelIds() const;

    /// r07 (QA-07): whether a model runs on this computer (its provider's address, as the user set it, is on this
    /// computer): offline mode and the project's no-cloud lock never stop it. False for an unknown model.
    bool runsLocally(const std::string& modelId) const;
    /// r07 (QA-02): a copy of the live model registry (catalogue, models the user added, custom providers) for a job
    /// thread. GUI thread.
    std::shared_ptr<const wabcut::ai::ModelRegistry> registrySnapshot() const;

    /// The platform for C++ callers (agent, transcript panel, wabcut-cli integration). GUI thread.
    wabcut::ai::AiPlatform& platform();
    /// The network stack (proxies, TLS, the redacted request log) for C++ callers that download, such as smart
    /// search's model. Created on the GUI thread on first use; the transport itself may then be used from any thread.
    wabcut::ai::IHttpTransport& transport();

signals:
    /// Emitted first thing in the destructor, while the platform and the network stack still exist: objects whose
    /// worker threads use them (GenerateController, TranscriptBridge) stop and join those threads here. QML singletons
    /// have no destruction order, so this is the only safe point (review wave 4, P2-4).
    void shuttingDown();
    /// The runtime's status or busy state changed (emitted on changes only).
    void localChanged();
    void localModelsChanged();
    void providersChanged();
    void modelsChanged();
    void privacyChanged();
    void helperCpuLimitChanged();
    void spendChanged();
    void requestLogChanged();
    void statusChanged();
    /// `ok` false with a short user-facing reason ("The provider rejected the key.").
    void keyCheckFinished(const QString& providerId, bool ok, const QString& message);
    /// r07: the answer to listRemoteModels(): `models` = [{id, capability}] (capability a best guess from the id).
    void remoteModelsListed(const QString& providerId, bool ok, const QVariantList& models, const QString& message);
    /// r07 (QA-08): the answer to checkCustomProvider(): `ok` when the key works (or the server needs none).
    void customProviderChecked(const QString& request, bool ok, const QString& message);

private:
    struct State;
    void load();
    /// r07: recomputes the effective preference lists (explicit defaults, else the automatic order) after anything
    /// that changes what is usable.
    void applyRouting();
    void loadCustomProviders();
    void saveCustomProviders() const;
    QString unusableReason(const wabcut::ai::ModelSpec& model, wabcut::ai::Capability capability) const;
    bool hasKeyFor(const wabcut::ai::ProviderSpec& provider) const;
    QVariantMap modelRow(const wabcut::ai::ModelSpec& model, const QString& feature) const;
    QVariantMap routingRow(const QString& feature) const;
    std::expected<std::pair<std::string, wabcut::ai::Secret>, wabcut::ai::AiError> prepareLocal(const std::string&, std::stop_token);
    void saveSettings() const;
    /// `checked` false: stored without a call (skipCheck, or a provider without a key check; r07).
    void finishKeyCheck(const QString& providerId, const QString& key, bool store,
                        const std::expected<void, wabcut::ai::AiError>& result, bool checked = true);
    QVariantList modelsWith(wabcut::ai::Capability capability) const;
    /// QA-08: the provider `spec` describes, validated like saveCustomProvider's but not registered, and the key to try
    /// (`key`, else the saved provider's). False with `error` when it cannot be checked.
    bool draftCustomProvider(const QVariantMap& spec, const QString& key, wabcut::ai::ProviderSpec& provider, std::string& secret,
                             QString& error) const;
    /// GET {baseUrl}/models (or the protocol's list) off the GUI thread; answers remoteModelsListed(replyId, ...).
    void startModelListing(const QString& replyId, const wabcut::ai::ProviderSpec& provider, const std::string& baseUrl,
                           std::string_view key);

    std::unique_ptr<State> d;
};

/// QML registration through create() (the class stays default-constructible for C++ and tests).
struct AiControllerForeign {
    Q_GADGET
    QML_FOREIGN(AiController)
    QML_NAMED_ELEMENT(AiController)
    QML_SINGLETON

public:
    static AiController* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return AiController::create(qmlEngine, jsEngine); }
};
