#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/ai/llm/Tiers.h>
#include <wabcut/ai/HttpDownload.h>
#include "PlaybackBridge.h"
#include "ExportController.h"
#include <QTimer>
#include <atomic>
#include "AiController.h"

#include "ProjectController.h"

#include <wabcut/ai/Redact.h>
#include "CrashHandler.h"
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai_net/Network.h>

#if WABCUT_WITH_QTKEYCHAIN
#include <qt6keychain/keychain.h>
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QMetaObject>
#include <QPointer>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <stop_token>

using namespace wabcut::ai;

namespace {

constexpr char kGroup[] = "ai";

QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
std::string str(const QString& s) { return s.toStdString(); }

#if WABCUT_WITH_QTKEYCHAIN
/// API keys in the OS keychain through QtKeychain 0.17 (Windows Credential Manager entry "provider:<id>@Wabcut",
/// macOS Keychain, Secret Service). QtKeychain queues its jobs on one thread and needs that thread's event loop, so
/// every call runs on the GUI thread: calls from job threads are marshalled there, and the GUI thread waits for the
/// job in a local event loop that ignores user input.
class KeychainStore final : public SecretStore {
public:
    explicit KeychainStore(QObject* owner) : m_owner(owner) {}

    std::expected<std::optional<Secret>, AiError> read(std::string_view providerId) override {
        std::expected<std::optional<Secret>, AiError> result = std::optional<Secret>();
        onOwnerThread([&] {
            QKeychain::ReadPasswordJob job(QStringLiteral("Wabcut"));
            job.setAutoDelete(false);
            job.setKey(entry(providerId));
            run(job);
            if (job.error() == QKeychain::EntryNotFound) {
                result = std::optional<Secret>();
            } else if (job.error() != QKeychain::NoError) {
                result = std::unexpected(error(job));
            } else {
                QByteArray bytes = job.textData().toUtf8();
                result = std::optional<Secret>(Secret(bytes.toStdString()));
                bytes.fill('\0');
            }
        });
        return result;
    }

    std::expected<void, AiError> write(std::string_view providerId, const Secret& secret) override {
        std::expected<void, AiError> result;
        onOwnerThread([&] {
            QKeychain::WritePasswordJob job(QStringLiteral("Wabcut"));
            job.setAutoDelete(false);
            job.setKey(entry(providerId));
            job.setTextData(QString::fromUtf8(secret.reveal().data(), static_cast<qsizetype>(secret.reveal().size())));
            run(job);
            if (job.error() != QKeychain::NoError) {
                result = std::unexpected(error(job));
            }
        });
        return result;
    }

    std::expected<void, AiError> remove(std::string_view providerId) override {
        std::expected<void, AiError> result;
        onOwnerThread([&] {
            QKeychain::DeletePasswordJob job(QStringLiteral("Wabcut"));
            job.setAutoDelete(false);
            job.setKey(entry(providerId));
            run(job);
            if (job.error() != QKeychain::NoError && job.error() != QKeychain::EntryNotFound) {
                result = std::unexpected(error(job));
            }
        });
        return result;
    }

    std::string backendName() const override {
#if defined(Q_OS_WIN)
        return "Windows Credential Manager";
#elif defined(Q_OS_MACOS)
        return "macOS Keychain";
#else
        return "Secret Service";
#endif
    }
    bool persistent() const override { return true; }

private:
    static QString entry(std::string_view providerId) { return QStringLiteral("provider:") + qs(providerId); }

    static void run(QKeychain::Job& job) {
        QEventLoop loop;
        QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        job.start();
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }

    static AiError error(const QKeychain::Job& job) {
        const AiErrorCode code = job.error() == QKeychain::AccessDenied || job.error() == QKeychain::AccessDeniedByUser
                                     ? AiErrorCode::Unauthorized
                                     : AiErrorCode::ProviderError;
        return AiError(code, "keychain: " + job.errorString().toStdString());
    }

    void onOwnerThread(const std::function<void()>& fn) {
        if (QThread::currentThread() == m_owner->thread()) {
            fn();
        } else {
            QMetaObject::invokeMethod(m_owner, fn, Qt::BlockingQueuedConnection);
        }
    }

    QObject* m_owner;
};
#endif

/// Forwards to the network stack, creating it on first use (no network thread until AI is used).
class LazyTransport final : public IHttpTransport {
public:
    explicit LazyTransport(std::function<IHttpTransport&()> get) : m_get(std::move(get)) {}
    std::expected<HttpResponse, AiError> send(const HttpRequest& request, const HttpStreamHandlers& handlers,
                                              std::stop_token stop) override {
        return m_get().send(request, handlers, stop);
    }

private:
    std::function<IHttpTransport&()> m_get;
};

QString formatPrice(const ModelSpec& model, std::chrono::year_month_day today) {
    const PriceRule* rule = priceRuleFor(model, 0, today);
    if (!rule) {
        return QObject::tr("Free");
    }
    if (rule->perThousandChars > 0) { // AI-12: ElevenLabs voices are billed per character
        return QObject::tr("%1 per 1,000 characters").arg(qs(formatUsd(rule->perThousandChars)));
    }
    if (rule->perAudioMinute > 0) {
        return QObject::tr("%1 per hour").arg(qs(formatUsd(rule->perAudioMinute * 60)));
    }
    if (rule->inputPerMTok > 0 || rule->outputPerMTok > 0) {
        return QObject::tr("%1 in · %2 out per 1M tokens").arg(qs(formatUsd(rule->inputPerMTok)), qs(formatUsd(rule->outputPerMTok)));
    }
    return QObject::tr("Free");
}

QString capabilityName(Capability c) {
    switch (c) {
    case Capability::Chat:
        return QObject::tr("Assistant");
    case Capability::Transcribe:
        return QObject::tr("Transcription");
    case Capability::Embed:
        return QObject::tr("Search embeddings");
    case Capability::Speak:
        return QObject::tr("Voice-over");
    case Capability::Translate:
        return QObject::tr("Translation");
    case Capability::GenerateMedia:
        return QObject::tr("Generative media");
    case Capability::CloneVoice:
        return QObject::tr("Voice cloning");
    }
    return {};
}

} // namespace

struct AiController::State {
    ModelRegistry models = ModelRegistry::builtin();
    std::unique_ptr<ProviderRegistry> providers;
    std::unique_ptr<SecretStore> store;
    std::unique_ptr<KeyRing> keys;
    ConsentLedger consent;
    SpendGuard spend;
    RequestLog log{500};
    std::mutex networkMutex;
    std::unique_ptr<net::NetworkStack> network;
    std::unique_ptr<OfflineGateTransport> offlineTransport;
    std::unique_ptr<LazyTransport> transport;
    std::unique_ptr<AiPlatform> platform;
    QThreadPool pool;          // key checks; waited for before the network stack goes away
    QPointer<wabcut::app::ProjectController> project; // whose ProjectState::aiNoCloud is the project lock
    bool lastProjectLock = false;
    // The active project's identity for ConsentLedger (review wave 3, P1-5): bumped whenever the session is reset
    // (another project opened, created or recovered), so a cloud operation cannot outlive its project.
    std::shared_ptr<std::atomic<std::uint64_t>> projectGeneration = std::make_shared<std::atomic<std::uint64_t>>(1);
    wabcut::EditSession::ListenerId resetListener = 0;
    bool listening = false;
    std::stop_source stopJobs; // cancels them at shutdown
    bool loaded = false;
    std::unique_ptr<llm::LocalRuntime> local;
    std::mutex localMutex;
    std::atomic<bool> localWork{false};
    QString localMessage;
    QVariantList localModels;         // cached (P2-3); empty = not computed yet
    llm::RuntimeStatus lastLocalStatus; // what the 1 s tick last reported
    std::filesystem::path runtimePath;
    std::string runtimeId;
};

AiController::AiController(QObject* parent) : QObject(parent), d(std::make_unique<State>()) {
    d->providers = std::make_unique<ProviderRegistry>(d->models);
    net::registerCloudAdapters(*d->providers);

    [[maybe_unused]] const bool memoryOnly = qEnvironmentVariable("WABCUT_AI_KEYSTORE") == QLatin1String("memory");
#if WABCUT_WITH_QTKEYCHAIN
    if (!memoryOnly && QKeychain::isAvailable()) {
        d->store = std::make_unique<KeychainStore>(this);
    }
#endif
    if (!d->store) {
        d->store = std::make_unique<MemorySecretStore>();
    }
    // The key ring registers every key with the process-wide redactor: the crash log redacts them too (S5-P2-10).
    wabcut::crash::setSecretRedaction([](std::string_view line) { return Redactor::global().redactSecrets(line, "<redacted>"); });
    d->keys = std::make_unique<KeyRing>(*d->store);
    d->transport = std::make_unique<LazyTransport>([this]() -> IHttpTransport& { return transport(); });
    d->platform = std::make_unique<AiPlatform>(*d->providers, *d->keys, d->consent, d->spend, d->log, *d->transport);

    d->local = std::make_unique<llm::LocalRuntime>(*d->transport);
    d->providers->registerChat("managed-llama", [this](ProviderContext c) {
        const auto id = c.model.id;
        return net::makeManagedChat(std::move(c), [this, id](std::stop_token token) { return prepareLocal(id, token); },
                                    [this](bool busy) { d->local->setBusy(busy); });
    });
    auto* localTimer = new QTimer(this);
    localTimer->setInterval(1000);
    connect(localTimer, &QTimer::timeout, this, [this] {
        // The tick notices idle unloads and exits; the UI hears about it only when something changed (P2-3).
        d->local->tick();
        const auto status = d->local->status();
        if (status.state != d->lastLocalStatus.state || status.message != d->lastLocalStatus.message || status.model != d->lastLocalStatus.model) {
            d->lastLocalStatus = status;
            emit localChanged();
        }
    });
    localTimer->start();

    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!logDir.isEmpty()) {
        d->log.setFile(std::filesystem::path(QDir(logDir).filePath(QStringLiteral("ai/requests.jsonl")).toStdU16String()));
    }
    load();

    // Engine objects call back from job threads: hop to the GUI thread before touching QML-visible state.
    QPointer<AiController> self(this);
    d->spend.setOnChanged([self] {
        QMetaObject::invokeMethod(self, [self] {
            if (self) {
                self->saveSettings();
                emit self->spendChanged();
            }
        }, Qt::QueuedConnection);
    });
    d->consent.setOnChanged([self] {
        QMetaObject::invokeMethod(self, [self] {
            if (self) {
                self->saveSettings();
                emit self->privacyChanged();
                emit self->statusChanged();
                emit self->modelsChanged();
            }
        }, Qt::QueuedConnection);
    });
    d->log.setOnAppended([self](const RequestLogEntry&) {
        QMetaObject::invokeMethod(self, [self] {
            if (self) {
                emit self->requestLogChanged();
            }
        }, Qt::QueuedConnection);
    });
    d->loaded = true;
}

AiController* AiController::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* ai = new AiController;
    if (qmlEngine) {
        ai->setProjectController(qmlEngine->singletonInstance<wabcut::app::ProjectController*>("Wabcut", "ProjectController"));
    }
    if (qmlEngine) {
        auto* playback = qmlEngine->singletonInstance<wabcut::app::PlaybackBridge*>("Wabcut", "Playback");
        auto* exporting = qmlEngine->singletonInstance<wabcut::app::ExportController*>("Wabcut", "ExportController");
        const auto pressure = [ai, playback, exporting] {
            ai->localPressure((playback && playback->playing()) || (exporting && exporting->busy()));
        };
        if (playback) QObject::connect(playback, &wabcut::app::PlaybackBridge::transportChanged, ai, pressure);
        if (exporting) QObject::connect(exporting, &wabcut::app::ExportController::busyChanged, ai, pressure);
        pressure();
    }
    return ai;
}

void AiController::setProjectController(wabcut::app::ProjectController* controller) {
    if (d->project == controller) {
        return;
    }
    if (d->project) {
        disconnect(d->project, nullptr, this, nullptr);
        if (d->listening) {
            d->project->session().removeListener(d->resetListener);
        }
    }
    d->listening = false;
    d->project = controller;
    if (controller) {
        // Read at every consent check, from any thread (snapshot() is thread-safe): the ACTIVE project decides, so a
        // reopened, recovered or switched-to project carries its own lock and consent records never override it.
        wabcut::EditSession* session = &controller->session();
        d->consent.setProjectLockSource([session] { return session->snapshot()->aiNoCloud; });
        const auto generation = d->projectGeneration;
        generation->fetch_add(1);
        d->consent.setProjectIdentitySource([generation] { return generation->load(); });
        d->resetListener = session->addListener([generation](const wabcut::ChangeEvent& event) {
            if (event.kind == wabcut::ChangeKind::Reset) {
                generation->fetch_add(1); // another project: operations of the previous one stop
            }
        });
        d->listening = true;
        connect(controller, &wabcut::app::ProjectController::sessionClosing, this,
                [this, session, generation] {
                    generation->fetch_add(1);
                    d->consent.setProjectLockSource({});
                    d->consent.setProjectIdentitySource({});
                    if (d->listening) {
                        session->removeListener(d->resetListener);
                        d->listening = false;
                    }
                },
                Qt::DirectConnection);
        connect(controller, &wabcut::app::ProjectController::revisionChanged, this, [this] {
            if (projectLock() != d->lastProjectLock) { // a project opened, undone, redone or switched
                d->lastProjectLock = projectLock();
                emit privacyChanged();
                emit statusChanged();
                emit modelsChanged();
            }
        });
    } else {
        d->consent.setProjectLockSource({});
        d->consent.setProjectIdentitySource({});
    }
    d->lastProjectLock = projectLock();
    emit privacyChanged();
    emit statusChanged();
}

AiController::~AiController() {
    emit shuttingDown(); // workers that use the platform finish before any of it is destroyed (P2-4)
    d->consent.setProjectLockSource({});
    d->consent.setProjectIdentitySource({});
    if (d->project && d->listening) {
        d->project->session().removeListener(d->resetListener);
    }
    d->stopJobs.request_stop();
    d->local->stop();
    d->pool.waitForDone();
    d->spend.setOnChanged({});
    d->consent.setOnChanged({});
    d->log.setOnAppended({});
}

IHttpTransport& AiController::transport() {
    std::lock_guard lock(d->networkMutex);
    if (!d->network) {
        d->network = std::make_unique<net::NetworkStack>(d->log);
        d->offlineTransport = std::make_unique<OfflineGateTransport>(d->network->transport(), d->consent);
    }
    return *d->offlineTransport;
}

AiPlatform& AiController::platform() { return *d->platform; }

// --- Persistence --------------------------------------------------------------------------------------------------

void AiController::load() {
    QSettings settings;
    settings.beginGroup(kGroup);
    const QStringList userModels = settings.value(QStringLiteral("userModels")).toStringList();
    for (const QString& entry : userModels) {
        const auto parts = entry.split(QLatin1Char('\t'));
        if (parts.size() == 2) {
            ModelSpec m;
            m.provider = str(parts[0]);
            m.id = str(parts[1]);
            (void)d->models.addModel(std::move(m));
        }
    }
    settings.beginGroup(QStringLiteral("baseUrls"));
    for (const QString& provider : settings.childKeys()) {
        (void)d->providers->setBaseUrl(str(provider), str(settings.value(provider).toString()));
    }
    settings.endGroup();
    auto preference = [&](const char* key, std::vector<std::string> fallback) {
        const QStringList ids = settings.value(QLatin1String(key)).toStringList();
        if (!settings.contains(QLatin1String(key))) {
            return fallback;
        }
        std::vector<std::string> out;
        for (const QString& id : ids) {
            if (!id.isEmpty()) {
                out.push_back(str(id));
            }
        }
        return out;
    };
    // AI-15: until the user picks one, the local assistant tier this computer runs best (Tiers.h).
    d->providers->setPreference(Capability::Chat, preference("chatPreference", {std::string(llm::defaultModel(llm::probeSystem()))}));
    d->providers->setPreference(Capability::Transcribe, preference("transcriptionPreference", {}));
    if (const auto consent = Json::parse(str(settings.value(QStringLiteral("consent")).toString()), nullptr, false);
        consent.is_object()) {
        (void)d->consent.loadJson(consent);
    }
    if (const auto spend = Json::parse(str(settings.value(QStringLiteral("spend")).toString()), nullptr, false);
        spend.is_object()) {
        (void)d->spend.loadJson(spend);
    }
    settings.endGroup();
}

void AiController::saveSettings() const {
    if (!d->loaded) {
        return;
    }
    QSettings settings;
    settings.beginGroup(kGroup);
    auto ids = [](const std::vector<std::string>& v) {
        QStringList out;
        for (const auto& id : v) {
            out << qs(id);
        }
        return out;
    };
    settings.setValue(QStringLiteral("chatPreference"), ids(d->providers->preference(Capability::Chat)));
    settings.setValue(QStringLiteral("transcriptionPreference"), ids(d->providers->preference(Capability::Transcribe)));
    settings.setValue(QStringLiteral("consent"), qs(d->consent.toJson().dump()));
    settings.setValue(QStringLiteral("spend"), qs(d->spend.toJson().dump()));
    QStringList userModels;
    for (const auto& m : d->models.models()) {
        if (d->models.isUserModel(m.id)) {
            userModels << qs(m.provider) + QLatin1Char('\t') + qs(m.id);
        }
    }
    settings.setValue(QStringLiteral("userModels"), userModels);
    settings.remove(QStringLiteral("baseUrls"));
    for (const auto& p : d->models.providers()) {
        const std::string url = d->providers->baseUrl(p);
        if (p.configurableBaseUrl && url != p.baseUrl) {
            settings.setValue(QStringLiteral("baseUrls/") + qs(p.id), qs(url));
        }
    }
    settings.endGroup();
}

// --- Read-only views ----------------------------------------------------------------------------------------------

QVariantList AiController::providers() const {
    QSettings settings;
    QVariantList out;
    for (const auto& p : d->models.providers()) {
        QVariantMap m;
        m[QStringLiteral("id")] = qs(p.id);
        m[QStringLiteral("name")] = qs(p.name);
        m[QStringLiteral("local")] = p.local;
        m[QStringLiteral("keyRequired")] = p.keyRequired;
        const QString meta = QStringLiteral("%1/keys/%2/").arg(QLatin1String(kGroup), qs(p.id));
        const bool stored = settings.value(meta + QStringLiteral("hasKey"), false).toBool();
        const bool fromEnv = !p.envVar.empty() && systemEnv(p.envVar).has_value();
        const QString last4 = settings.value(meta + QStringLiteral("last4")).toString();
        m[QStringLiteral("hasKey")] = stored || fromEnv;
        m[QStringLiteral("keySource")] = fromEnv ? QStringLiteral("environment") : stored ? QStringLiteral("store") : QString();
        m[QStringLiteral("masked")] = stored ? QStringLiteral("••••") + last4 : QString();
        m[QStringLiteral("validatedAt")] = settings.value(meta + QStringLiteral("validatedAt")).toString();
        QString status;
        QString text;
        if (!p.keyRequired) {
            status = QStringLiteral("local");
            text = p.local ? tr("On this computer") : tr("No key needed");
        } else if (fromEnv) {
            status = QStringLiteral("key");
            text = tr("Key from %1").arg(qs(p.envVar));
        } else if (stored) {
            status = QStringLiteral("key");
            text = tr("Key ••••%1 · %2").arg(last4, keyStoreName());
        } else {
            status = QStringLiteral("none");
            text = tr("No key");
        }
        m[QStringLiteral("status")] = status;
        m[QStringLiteral("statusText")] = text;
        m[QStringLiteral("consoleUrl")] = qs(p.consoleUrl);
        m[QStringLiteral("termsUrl")] = qs(p.termsUrl);
        m[QStringLiteral("dataPolicy")] = qs(p.dataPolicy);
        m[QStringLiteral("baseUrl")] = qs(d->providers->baseUrl(p));
        m[QStringLiteral("configurableBaseUrl")] = p.configurableBaseUrl;
        m[QStringLiteral("verified")] = p.verified;
        QStringList caps;
        bool chat = false;
        bool transcribe = false;
        for (const ModelSpec* model : d->models.modelsOf(p.id)) {
            chat = chat || model->caps.has(Capability::Chat);
            transcribe = transcribe || model->caps.has(Capability::Transcribe);
        }
        if (chat || p.api == "chat-completions") {
            caps << tr("Assistant");
        }
        if (transcribe) {
            caps << tr("Transcription");
        }
        m[QStringLiteral("capabilities")] = caps.join(QStringLiteral(" · "));
        out << m;
    }
    return out;
}

QVariantList AiController::modelsWith(Capability capability) const {
    const auto today = d->platform->today();
    QSettings settings;
    QVariantList out;
    for (const ModelSpec* model : d->models.modelsWith(capability)) {
        const ProviderSpec* provider = d->models.provider(model->provider);
        if (!provider) {
            continue;
        }
        QVariantMap m;
        m[QStringLiteral("id")] = qs(model->id);
        m[QStringLiteral("name")] = qs(model->name);
        m[QStringLiteral("provider")] = qs(provider->id);
        m[QStringLiteral("providerName")] = qs(provider->name);
        m[QStringLiteral("price")] = formatPrice(*model, today);
        QStringList badges;
        if (capability == Capability::Chat) {
            if (model->caps.toolsOnChat || model->caps.toolsOnResponses) {
                badges << tr("Tools");
            }
            if (model->caps.vision) {
                badges << tr("Images");
            }
            if (model->caps.videoInput == VideoInput::Native) {
                badges << tr("Video");
            }
        } else {
            if (model->caps.wordTimestamps) {
                badges << tr("Word timing");
            }
            if (model->caps.diarization) {
                badges << tr("Speakers");
            }
        }
        if (model->retirement) {
            badges << tr("Until %1").arg(qs(formatDate(*model->retirement)));
        }
        m[QStringLiteral("badges")] = badges;
        m[QStringLiteral("verified")] = model->verified;
        m[QStringLiteral("notes")] = qs(model->notes);
        m[QStringLiteral("userAdded")] = d->models.isUserModel(model->id);
        QString reason;
        if (auto c = d->providers->candidate(model->id, capability, today); !c) {
            reason = c.error().code == AiErrorCode::Unsupported ? tr("Not supported yet") : tr("Unavailable");
        } else if (provider->keyRequired && !settings.value(QStringLiteral("%1/keys/%2/hasKey").arg(QLatin1String(kGroup), qs(provider->id))).toBool() &&
                   !(provider->envVar.size() && systemEnv(provider->envVar))) {
            reason = tr("Needs a %1 key").arg(qs(provider->name));
        } else if (isCloud(*provider, d->providers->baseUrl(*provider)) && !d->consent.cloudAllowed()) {
            reason = tr("Offline");
        }
        m[QStringLiteral("usable")] = reason.isEmpty();
        m[QStringLiteral("reason")] = reason;
        out << m;
    }
    return out;
}

QVariantList AiController::chatModels() const { return modelsWith(Capability::Chat); }
QVariantList AiController::transcriptionModels() const { return modelsWith(Capability::Transcribe); }
QVariantList AiController::speechModels() const { return modelsWith(Capability::Speak); }

QString AiController::chatModel() const {
    const auto pref = d->providers->preference(Capability::Chat);
    return pref.empty() ? QString() : qs(pref.front());
}

void AiController::setChatModel(const QString& id) {
    if (id == chatModel()) {
        return;
    }
    d->providers->setPreference(Capability::Chat, id.isEmpty() ? std::vector<std::string>() : std::vector<std::string>{str(id)});
    saveSettings();
    emit modelsChanged();
    emit statusChanged();
}

QString AiController::transcriptionModel() const {
    const auto pref = d->providers->preference(Capability::Transcribe);
    return pref.empty() ? QString() : qs(pref.front());
}

void AiController::setTranscriptionModel(const QString& id) {
    if (id == transcriptionModel()) {
        return;
    }
    d->providers->setPreference(Capability::Transcribe,
                                id.isEmpty() ? std::vector<std::string>() : std::vector<std::string>{str(id)});
    saveSettings();
    emit modelsChanged();
}

bool AiController::offlineMode() const { return d->consent.offlineMode(); }

void AiController::setOfflineMode(bool offline) {
    if (offline == offlineMode()) {
        return;
    }
    d->consent.setOfflineMode(offline); // persisted and signalled through the ledger's callback
}

bool AiController::projectLock() const { return d->consent.projectLock(); }

void AiController::setProjectLock(bool locked) {
    if (locked == projectLock()) {
        return;
    }
    if (d->project) {
        // A project setting: saved with the project (and its recovery copies), one undo step.
        auto& session = d->project->session();
        const auto applied = session.apply(locked ? "Lock Project to No-Cloud AI" : "Allow Cloud AI for Project",
                                           [locked](const wabcut::ProjectState& s) -> std::expected<wabcut::ProjectState, wabcut::Error> {
                                               wabcut::ProjectState next = s;
                                               next.aiNoCloud = locked;
                                               return next;
                                           });
        if (!applied) {
            return;
        }
    } else {
        d->consent.setProjectLock(locked);
    }
    d->lastProjectLock = projectLock();
    emit privacyChanged();
    emit statusChanged();
    emit modelsChanged();
}

QVariantList AiController::consents() const {
    QVariantList out;
    for (const auto& r : d->consent.records()) {
        const ProviderSpec* p = d->models.provider(r.provider);
        QVariantMap m;
        m[QStringLiteral("provider")] = qs(r.provider);
        m[QStringLiteral("providerName")] = p ? qs(p->name) : qs(r.provider);
        m[QStringLiteral("capability")] = qs(toString(r.capability));
        m[QStringLiteral("capabilityName")] = capabilityName(r.capability);
        m[QStringLiteral("grantedAt")] = qs(r.grantedAt).left(10);
        out << m;
    }
    return out;
}

QVariantList AiController::spend() const {
    const auto today = d->platform->today();
    const std::chrono::year_month month{today.year(), today.month()};
    QVariantList out;
    for (const auto& p : d->models.providers()) {
        if (p.local || d->models.modelsOf(p.id).empty()) {
            continue;
        }
        const auto cap = d->spend.monthlyCap(p.id);
        const MicroUsd spent = d->spend.spent(p.id, month);
        QVariantMap m;
        m[QStringLiteral("provider")] = qs(p.id);
        m[QStringLiteral("providerName")] = qs(p.name);
        m[QStringLiteral("capped")] = cap.has_value();
        m[QStringLiteral("capDollars")] = cap ? static_cast<double>(*cap) / kMicroUsdPerUsd : 0.0;
        m[QStringLiteral("cap")] = cap ? qs(formatUsd(*cap)) : QString();
        m[QStringLiteral("spent")] = qs(formatUsd(spent));
        m[QStringLiteral("remaining")] = cap ? qs(formatUsd(std::max<MicroUsd>(0, *cap - spent))) : QString();
        m[QStringLiteral("fraction")] = cap && *cap > 0 ? std::min(1.0, static_cast<double>(spent) / static_cast<double>(*cap)) : 0.0;
        out << m;
    }
    return out;
}

QVariantList AiController::requestLog() const {
    const auto entries = d->log.entries();
    QVariantList out;
    const int count = static_cast<int>(std::min<std::size_t>(entries.size(), 100));
    for (int i = 0; i < count; ++i) {
        const auto& e = entries[entries.size() - 1 - static_cast<std::size_t>(i)];
        QVariantMap m;
        m[QStringLiteral("time")] = qs(e.time).mid(11, 8);
        m[QStringLiteral("kind")] = e.kind == RequestLogEntry::Kind::Http ? QStringLiteral("http") : QStringLiteral("call");
        if (e.kind == RequestLogEntry::Kind::Http) {
            m[QStringLiteral("what")] = qs(e.method) + QLatin1Char(' ') + qs(e.url);
            m[QStringLiteral("status")] = e.status > 0 ? QString::number(e.status) : QString();
            m[QStringLiteral("cost")] = QString();
        } else {
            m[QStringLiteral("what")] = qs(e.provider) + QStringLiteral(" · ") + qs(e.model) + QStringLiteral(" · ") + qs(e.capability);
            m[QStringLiteral("status")] = e.error.empty() ? tr("ok") : QString();
            m[QStringLiteral("cost")] = qs(formatUsd(e.cost));
        }
        m[QStringLiteral("error")] = qs(e.error);
        out << m;
    }
    return out;
}

QString AiController::keyStoreName() const {
    return d->store->persistent() ? qs(d->store->backendName()) : tr("this session only");
}
bool AiController::keyStorePersistent() const { return d->store->persistent(); }

bool AiController::plaintextKeysOptIn() const { return false; }
std::filesystem::path AiController::legacyKeyFile() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return std::filesystem::path(QDir(dir).filePath(QStringLiteral("ai-keys.json")).toStdU16String());
}
int AiController::legacyPlaintextKeys() const { return static_cast<int>(legacyKeyCount(legacyKeyFile())); }
QString AiController::importPlaintextKeys() {
    const auto result = importLegacyKeys(legacyKeyFile(), *d->keys, d->models.providers());
    QSettings settings;
    for (const auto& id : result.imported) {
        const ProviderSpec* provider = d->models.provider(id);
        if (!provider) {
            continue;
        }
        const auto key = d->keys->get(*provider);
        if (!key) {
            continue;
        }
        const QString meta = QStringLiteral("%1/keys/%2/").arg(QLatin1String(kGroup), qs(id));
        settings.setValue(meta + QStringLiteral("hasKey"), true);
        settings.setValue(meta + QStringLiteral("last4"), qs(key->last4()));
    }
    emit providersChanged();
    emit modelsChanged();
    emit statusChanged();
    emit privacyChanged();
    if (!result.error.empty()) {
        return tr("Some keys could not be moved: %1").arg(qs(result.error));
    }
    if (!result.keptBecause.empty()) { // nothing is lost: the user deletes the file when ready (A5-P2-17)
        return tr("Moved %n key(s) into %1. The plaintext file was kept: %2.", nullptr, static_cast<int>(result.imported.size()))
            .arg(keyStoreName(), qs(result.keptBecause));
    }
    QString message = tr("Moved %n key(s) into %1 and deleted the plaintext file.", nullptr, static_cast<int>(result.imported.size()))
                          .arg(keyStoreName());
    if (!keyStorePersistent()) {
        message += QLatin1Char(' ') + tr("No keychain is available, so they are kept for this session only.");
    }
    return message;
}
bool AiController::deletePlaintextKeys() {
    const bool removed = secureDeleteFile(legacyKeyFile());
    emit privacyChanged();
    return removed;
}
void AiController::setPlaintextKeysOptIn(bool) {
    QSettings().remove(QStringLiteral("%1/plaintextKeys").arg(QLatin1String(kGroup)));
    emit privacyChanged();
}

QString AiController::statusLabel() const {
    if (!d->consent.cloudAllowed()) {
        return tr("Offline");
    }
    const ModelSpec* model = d->models.model(str(chatModel()));
    const ProviderSpec* provider = model ? d->models.provider(model->provider) : nullptr;
    if (!provider) {
        return tr("Local");
    }
    return isCloud(*provider, d->providers->baseUrl(*provider)) ? qs(provider->name) : tr("Local");
}

QString AiController::registryUpdated() const { return qs(d->models.updated()); }

QStringList AiController::unverifiedEntries() const {
    QStringList out;
    for (const auto& e : ModelRegistry::builtin().unverifiedEntries()) {
        out << qs(e);
    }
    return out;
}

// --- Keys ---------------------------------------------------------------------------------------------------------

void AiController::saveKey(const QString& providerId, const QString& key, bool skipCheck) {
    const ProviderSpec* provider = d->models.provider(str(providerId));
    QString trimmed = key.trimmed();
    if (!provider) {
        emit keyCheckFinished(providerId, false, tr("Unknown provider."));
        return;
    }
    if (trimmed.isEmpty()) {
        emit keyCheckFinished(providerId, false, tr("Paste a key first."));
        return;
    }
    if (skipCheck) {
        finishKeyCheck(providerId, trimmed, true, {});
        return;
    }
    const ProviderSpec spec = *provider;
    const std::string baseUrl = d->providers->baseUrl(*provider);
    IHttpTransport& t = transport();
    QPointer<AiController> self(this);
    const std::stop_token stop = d->stopJobs.get_token();
    d->pool.start([self, &t, spec, baseUrl, providerId, trimmed, stop] {
        const auto result = validateKey(t, spec, baseUrl, Secret(trimmed.toStdString()), stop);
        QMetaObject::invokeMethod(self, [self, providerId, trimmed, result] {
            if (self) {
                self->finishKeyCheck(providerId, trimmed, true, result);
            }
        }, Qt::QueuedConnection);
    });
}

void AiController::testKey(const QString& providerId) {
    const ProviderSpec* provider = d->models.provider(str(providerId));
    if (!provider) {
        emit keyCheckFinished(providerId, false, tr("Unknown provider."));
        return;
    }
    auto key = d->keys->get(*provider);
    if (!key && provider->keyRequired) {
        emit keyCheckFinished(providerId, false, tr("No key is stored for %1.").arg(qs(provider->name)));
        return;
    }
    const ProviderSpec spec = *provider;
    const std::string baseUrl = d->providers->baseUrl(*provider);
    const QString value = key ? QString::fromStdString(std::string(key->reveal())) : QString();
    IHttpTransport& t = transport();
    QPointer<AiController> self(this);
    const std::stop_token stop = d->stopJobs.get_token();
    d->pool.start([self, &t, spec, baseUrl, providerId, value, stop] {
        const auto result = validateKey(t, spec, baseUrl, Secret(value.toStdString()), stop);
        QMetaObject::invokeMethod(self, [self, providerId, result] {
            if (self) {
                self->finishKeyCheck(providerId, QString(), false, result);
            }
        }, Qt::QueuedConnection);
    });
}

void AiController::finishKeyCheck(const QString& providerId, const QString& key, bool store,
                                  const std::expected<void, AiError>& result) {
    const ProviderSpec* provider = d->models.provider(str(providerId));
    if (!provider) {
        return;
    }
    const bool usable = result.has_value() || result.error().code == AiErrorCode::QuotaExceeded;
    QString message;
    if (result) {
        message = store ? tr("Key saved and checked.") : tr("%1 answered: the key works.").arg(qs(provider->name));
    } else {
        switch (result.error().code) {
        case AiErrorCode::Unauthorized:
            message = tr("%1 rejected the key.").arg(qs(provider->name));
            break;
        case AiErrorCode::QuotaExceeded:
            message = tr("The key works, but the account has no credit left.");
            break;
        case AiErrorCode::RateLimited:
            message = tr("%1 is busy. Try again in a moment.").arg(qs(provider->name));
            break;
        case AiErrorCode::Network:
            message = provider->local ? tr("No server answered at %1.").arg(qs(d->providers->baseUrl(*provider)))
                                      : tr("Could not reach %1. Check the connection, or save without checking.").arg(qs(provider->name));
            break;
        default:
            message = tr("The check failed: %1").arg(qs(Redactor::global().redact(result.error().message)));
            break;
        }
    }
    if (usable) {
        QSettings settings;
        const QString meta = QStringLiteral("%1/keys/%2/").arg(QLatin1String(kGroup), providerId);
        if (store) {
            if (auto saved = d->keys->save(*provider, Secret(key.toStdString())); !saved) {
                emit keyCheckFinished(providerId, false, tr("The key could not be stored: %1").arg(qs(saved.error().message)));
                return;
            }
            const QString last4 = key.size() > 8 ? key.right(4) : QString();
            settings.setValue(meta + QStringLiteral("hasKey"), true);
            settings.setValue(meta + QStringLiteral("last4"), last4);
        }
        if (result) {
            settings.setValue(meta + QStringLiteral("validatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        }
        emit providersChanged();
        emit modelsChanged();
        emit statusChanged();
    }
    emit keyCheckFinished(providerId, usable, message);
}

void AiController::removeKey(const QString& providerId) {
    const ProviderSpec* provider = d->models.provider(str(providerId));
    if (!provider) {
        return;
    }
    if (auto removed = d->keys->remove(*provider); !removed) {
        emit keyCheckFinished(providerId, false, tr("The key could not be removed: %1").arg(qs(removed.error().message)));
        return;
    }
    QSettings settings;
    settings.remove(QStringLiteral("%1/keys/%2").arg(QLatin1String(kGroup), providerId));
    emit providersChanged();
    emit modelsChanged();
    emit statusChanged();
}

// --- Providers and models -----------------------------------------------------------------------------------------

bool AiController::setBaseUrl(const QString& providerId, const QString& url) {
    if (auto r = d->providers->setBaseUrl(str(providerId), str(url.trimmed())); !r) {
        emit keyCheckFinished(providerId, false, tr("Use an http:// or https:// address."));
        return false;
    }
    saveSettings();
    emit providersChanged();
    emit modelsChanged();
    emit statusChanged();
    return true;
}

bool AiController::addModel(const QString& providerId, const QString& modelId) {
    ModelSpec m;
    m.provider = str(providerId);
    m.id = str(modelId.trimmed());
    if (!d->models.addModel(std::move(m))) {
        return false;
    }
    saveSettings();
    emit modelsChanged();
    return true;
}

bool AiController::removeModel(const QString& modelId) {
    if (!d->models.removeUserModel(str(modelId))) {
        return false;
    }
    saveSettings();
    emit modelsChanged();
    return true;
}

// --- Spend, consent, log ------------------------------------------------------------------------------------------

void AiController::setMonthlyCap(const QString& providerId, double dollars) {
    std::optional<MicroUsd> cap;
    if (std::isfinite(dollars) && dollars > 0.0) {
        cap = static_cast<MicroUsd>(std::llround(dollars * static_cast<double>(kMicroUsdPerUsd)));
    }
    d->spend.setMonthlyCap(str(providerId), cap); // persisted and signalled through the guard's callback
}

QVariantMap AiController::consentPrompt(const QString& modelId, const QString& capability, const QString& payload) const {
    QVariantMap m;
    const ModelSpec* model = d->models.model(str(modelId));
    const auto cap = capabilityFromString(str(capability));
    const ProviderSpec* provider = model ? d->models.provider(model->provider) : nullptr;
    if (!model || !cap || !provider) {
        return m;
    }
    const std::string baseUrl = d->providers->baseUrl(*provider);
    const ConsentPrompt prompt = makeConsentPrompt(*provider, baseUrl, *cap, str(payload), {});
    m[QStringLiteral("provider")] = qs(prompt.provider);
    m[QStringLiteral("providerName")] = qs(prompt.providerName);
    m[QStringLiteral("capability")] = capability;
    m[QStringLiteral("capabilityName")] = capabilityName(*cap);
    m[QStringLiteral("destination")] = qs(prompt.destination);
    m[QStringLiteral("payload")] = qs(prompt.payload);
    m[QStringLiteral("termsUrl")] = qs(prompt.termsUrl);
    m[QStringLiteral("dataPolicy")] = qs(prompt.dataPolicy);
    m[QStringLiteral("cost")] = formatPrice(*model, d->platform->today());
    m[QStringLiteral("needed")] = d->consent.state(*provider, baseUrl, *cap) == ConsentState::NeedsPrompt;
    return m;
}

void AiController::grantConsent(const QString& providerId, const QString& capability) {
    const ProviderSpec* provider = d->models.provider(str(providerId));
    const auto cap = capabilityFromString(str(capability));
    if (provider && cap) {
        d->consent.grantAlways(*provider, *cap, nowIso8601());
    }
}

void AiController::revokeConsent(const QString& providerId, const QString& capability) {
    if (const auto cap = capabilityFromString(str(capability))) {
        d->consent.revoke(str(providerId), *cap);
    }
}

void AiController::revokeAllConsents() { d->consent.revokeAll(); }

void AiController::clearRequestLog() {
    d->log.clear();
    emit requestLogChanged();
}

namespace {
/// The installable local models and how they fit this machine: probes the GPUs and stats the weights (not free).
QVariantList computeLocalModels() {
    QVariantList out;
    ModelStore store(ModelStore::defaultRoot());
    const auto probe = llm::probeSystem();
    // AI-15: each assistant tier's fit on this computer now, and the one it runs best (wabcut/ai/llm/Tiers.h).
    const llm::Recommendation recommendation = llm::recommend(probe);
    for (const auto& model : builtinModels()) {
        if (model.kind != ModelKind::Llm) continue;
        auto plan = llm::planLaunch(model, probe);
        QVariantMap row{{"id", qs(model.id)}, {"name", qs(model.description)}, {"license", qs(model.license)},
            {"card", qs(model.licenseCard)}, {"size", qs(llm::formatBytes(model.size))}, {"installed", store.isInstalled(model)},
            {"fits", plan.has_value()}, {"fit", plan ? qs(plan->gpu ? "GPU fits with playback reserve" : plan->reason) : qs(plan.error().message)},
            {"ram", qs(llm::formatBytes(model.size + (4ULL << 30)))}, {"vram", qs(llm::formatBytes(model.size + (3ULL << 30)))}};
        if (const llm::TierSpec* tier = llm::tierForModel(model.id)) {
            const llm::Admission admission = llm::admit(*tier, model, probe);
            row.insert(QStringLiteral("tier"), qs(std::string(tier->id)));
            row.insert(QStringLiteral("tierName"), qs(std::string(tier->name)));
            row.insert(QStringLiteral("context"), admission.context);
            row.insert(QStringLiteral("recommended"), recommendation.tier.has_value() && *recommendation.tier == tier->id);
            row.insert(QStringLiteral("ram"), qs(llm::formatBytes(admission.ramNeeded)));
            row.insert(QStringLiteral("vram"), admission.gpu ? qs(llm::formatBytes(admission.vramNeeded)) : QStringLiteral("CPU"));
        }
        out.push_back(row);
    }
    return out;
}
} // namespace

QVariantList AiController::localModels() const {
    if (d->localModels.isEmpty()) {
        d->localModels = computeLocalModels(); // first read: computed once, no signal from inside a getter
    }
    return d->localModels;
}

void AiController::refreshLocalModels() {
    QVariantList out = computeLocalModels();
    // The "fit" text quotes the free memory of this moment (AI-15's tiers), which moves all the time: only a change of
    // anything else (installed, fits, the recommended tier, ...) rebuilds the list (review wave 4, P2-3).
    const auto settled = [](const QVariantList& list) {
        QVariantList rows;
        for (const QVariant& row : list) {
            QVariantMap m = row.toMap();
            m.remove(QStringLiteral("fit"));
            rows.push_back(m);
        }
        return rows;
    };
    if (settled(out) != settled(d->localModels)) {
        d->localModels = std::move(out);
        emit localModelsChanged();
    }
}
QString AiController::localStatus() const {
    if (d->localWork) return d->localMessage;
    const auto message = d->local->status().message;
    return message.empty() ? d->localMessage : qs(message);
}
bool AiController::localBusy() const { return d->localWork; }
void AiController::localPressure(bool busy) { d->local->setPressure(busy); emit localChanged(); }
void AiController::stopLocal() { d->local->stop(); d->localMessage = tr("Local assistant stopped"); emit localChanged(); }
std::expected<std::pair<std::string, Secret>, AiError> AiController::prepareLocal(const std::string& id, std::stop_token stop) {
    std::lock_guard lock(d->localMutex);
    const auto status = d->local->status();
    if (status.state == llm::RuntimeStatus::State::Ready && status.model == id) {
        d->local->touch(); return std::pair{status.baseUrl, d->local->key()};
    }
    const auto* model = findModel(id);
    if (!model || model->kind != ModelKind::Llm) return std::unexpected(AiError{AiErrorCode::ModelMissing, "unknown local model"});
    const auto plan = llm::planLaunch(*model, llm::probeSystem());
    if (!plan) return std::unexpected(plan.error());
    ModelStore store(ModelStore::defaultRoot());
    auto weights = store.ensure(*model, nullptr);
    if (!weights) return std::unexpected(AiError{AiErrorCode::ModelMissing, "Download this model in AI preferences first"});
    if (auto v = store.verify(*model); !v) return std::unexpected(AiError{AiErrorCode::ModelMissing, v.error().message});
    if (d->runtimePath.empty() || d->runtimeId != plan->runtimeId) {
        const auto tar = std::filesystem::path(qEnvironmentVariable("SystemRoot").toStdWString()) / L"System32" / L"tar.exe";
        auto installed = llm::installRuntime(store, *plan, nullptr, tar, stop);
        if (!installed) return std::unexpected(installed.error());
        d->runtimePath = *installed; d->runtimeId = plan->runtimeId;
    }
    d->local->stop();
    auto started = d->local->start(d->runtimePath, *weights, *model, *plan, stop, {});
    if (!started) return std::unexpected(started.error());
    return std::pair{d->local->status().baseUrl, d->local->key()};
}
void AiController::startLocal(const QString& id) {
    if (d->localWork.exchange(true)) return;
    (void)transport();
    d->localMessage = tr("Loading local assistant"); emit localChanged();
    d->pool.start([this, id] {
        // QThreadPool does not catch: an exception would end the process (review wave 4, P2-6).
        std::expected<std::pair<std::string, Secret>, AiError> result = std::unexpected(AiError());
        try {
            result = prepareLocal(id.toStdString(), d->stopJobs.get_token());
        } catch (const std::exception& e) {
            result = std::unexpected(AiError(AiErrorCode::ProviderError, std::string("could not start: ") + e.what()));
        } catch (...) {
            result = std::unexpected(AiError(AiErrorCode::ProviderError, "could not start"));
        }
        const auto message = result ? tr("Local assistant ready") : qs(result.error().message);
        QMetaObject::invokeMethod(this, [this, id, message, ok = result.has_value()] {
            d->localWork = false; d->localMessage = message;
            if (!ok) d->local->stop();
            if (ok) {
                const auto* spec = d->models.model(id.toStdString());
                if (spec && spec->caps.toolsOnChat) setChatModel(id);
            }
            emit localChanged();
            refreshLocalModels();
        }, Qt::QueuedConnection);
    });
}
void AiController::downloadLocal(const QString& id) {
    if (offlineMode() || projectLock() || d->localWork.exchange(true)) return;
    const auto* model = findModel(id.toStdString());
    if (!model || model->kind != ModelKind::Llm) { d->localWork = false; return; }
    (void)transport();
    d->localMessage = tr("Downloading verified local model and engine"); emit localChanged();
    d->pool.start([this, model] {
        try {
        std::lock_guard lock(d->localMutex);
        ModelStore store(ModelStore::defaultRoot());
        // Downloads are user-triggered; beforeDispatch also catches an offline switch during download.
        class DownloadGuard final : public IHttpTransport {
        public:
            DownloadGuard(AiController& owner) : ai(owner) {}
            std::expected<HttpResponse, AiError> send(const HttpRequest& original, const HttpStreamHandlers& h, std::stop_token token) override {
                auto r = original;
                r.beforeDispatch.check = [this]() -> std::expected<void, AiError> {
                    if (ai.platform().consent().offlineMode() || ai.platform().consent().projectLock())
                        return std::unexpected(AiError{AiErrorCode::Offline, "offline mode blocks downloads"});
                    return {};
                };
                return ai.transport().send(r, h, token);
            }
            AiController& ai;
        } guard(*this);
        HttpDownloadTransport downloads(guard);
        auto result = store.ensure(*model, &downloads, {}, d->stopJobs.get_token());
        std::string message = result ? "Model downloaded" : result.error().message;
        if (result) {
            auto plan = llm::planLaunch(*model, llm::probeSystem());
            if (plan) {
                const auto* engine = findModel(plan->runtimeId);
                auto downloaded = store.ensure(*engine, &downloads, {}, d->stopJobs.get_token());
                if (!downloaded) message = downloaded.error().message;
            } else message = plan.error().message;
        }
        QMetaObject::invokeMethod(this, [this, message] { d->localMessage = qs(message); d->localWork = false; emit localChanged(); refreshLocalModels(); }, Qt::QueuedConnection);
        } catch (...) { // QThreadPool does not catch (review wave 4, P2-6)
            QMetaObject::invokeMethod(this, [this] { d->localMessage = tr("Download failed"); d->localWork = false; emit localChanged(); }, Qt::QueuedConnection);
        }
    });
}
