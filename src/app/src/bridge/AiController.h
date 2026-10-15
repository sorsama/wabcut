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
    ///   dataPolicy, baseUrl, configurableBaseUrl, verified, capabilities}]
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged)
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
    Q_PROPERTY(QString chatModel READ chatModel WRITE setChatModel NOTIFY modelsChanged)
    Q_PROPERTY(QString transcriptionModel READ transcriptionModel WRITE setTranscriptionModel NOTIFY modelsChanged)
    Q_PROPERTY(bool offlineMode READ offlineMode WRITE setOfflineMode NOTIFY privacyChanged)
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
    /// Adds a model id to a provider whose models the user defines (Ollama tags, OpenRouter models, ...).
    Q_INVOKABLE bool addModel(const QString& providerId, const QString& modelId);
    Q_INVOKABLE bool removeModel(const QString& modelId);
    /// Monthly cap in dollars; 0 or less removes the cap.
    Q_INVOKABLE void setMonthlyCap(const QString& providerId, double dollars);
    /// What the first-use consent prompt shows: {provider, providerName, capabilityName, destination, payload,
    /// termsUrl, dataPolicy, cost, needed}. `capability` is "chat", "transcribe", ...
    Q_INVOKABLE QVariantMap consentPrompt(const QString& modelId, const QString& capability, const QString& payload) const;
    Q_INVOKABLE void grantConsent(const QString& providerId, const QString& capability);
    Q_INVOKABLE void revokeConsent(const QString& providerId, const QString& capability);
    Q_INVOKABLE void revokeAllConsents();
    Q_INVOKABLE void clearRequestLog();

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
    void spendChanged();
    void requestLogChanged();
    void statusChanged();
    /// `ok` false with a short user-facing reason ("The provider rejected the key.").
    void keyCheckFinished(const QString& providerId, bool ok, const QString& message);

private:
    struct State;
    void load();
    std::expected<std::pair<std::string, wabcut::ai::Secret>, wabcut::ai::AiError> prepareLocal(const std::string&, std::stop_token);
    void saveSettings() const;
    void finishKeyCheck(const QString& providerId, const QString& key, bool store,
                        const std::expected<void, wabcut::ai::AiError>& result);
    QVariantList modelsWith(wabcut::ai::Capability capability) const;

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
