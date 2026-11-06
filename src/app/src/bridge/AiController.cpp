#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/ai/llm/Tiers.h>
#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/HttpDownload.h>
#include "PlaybackBridge.h"
#include "ExportController.h"
#include <QTimer>
#include <atomic>
#include "AiController.h"

#include "LocalModelsController.h"
#include "ProjectController.h"

#include <wabcut/ai/Redact.h>
#include "CrashHandler.h"
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/TranscriptionService.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai_net/Network.h>

#include <QJsonDocument>
#include <cctype>
#include <initializer_list>
#include <map>
#include <set>

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
#include <set>
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
    if (!rule) { // r07: catalogue hosts without a published price (local models carry an explicit zero price)
        return QObject::tr("Price not listed");
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

// --- r07: features, custom providers --------------------------------------------------------------------------------

QString featureName(std::string_view feature) {
    if (feature == "chat") return QObject::tr("Assistant");
    if (feature == "transcribe") return QObject::tr("Transcription");
    if (feature == "translate") return QObject::tr("Translation");
    if (feature == "speak") return QObject::tr("Voice");
    if (feature == "image") return QObject::tr("Images");
    if (feature == "video") return QObject::tr("Video");
    if (feature == "sound") return QObject::tr("Sound effects");
    return qs(feature);
}

QString featureDescription(std::string_view feature) {
    if (feature == "chat") return QObject::tr("The assistant, rough cut, shorts, takes, autopilot and descriptions");
    if (feature == "transcribe") return QObject::tr("Transcripts and captions from speech");
    if (feature == "translate") return QObject::tr("Caption translation");
    if (feature == "speak") return QObject::tr("Dubbing, narration and voice previews");
    if (feature == "image") return QObject::tr("Generated images");
    if (feature == "video") return QObject::tr("Generated video clips");
    if (feature == "sound") return QObject::tr("Generated sound effects");
    return {};
}

bool isFeature(std::string_view feature) { return featureCapability(feature).has_value(); }

/// A catalogue base URL with a placeholder the user fills in ("https://{resource}.openai.azure.com/openai/v1").
bool needsAddress(std::string_view url) { return url.find('{') != std::string_view::npos; }

/// The adapter API and default path of a custom provider's model, per protocol and feature ("" = not offered).
struct CustomRoute {
    std::string_view api;
    std::string_view path;
};

CustomRoute customRoute(std::string_view protocol, std::string_view feature) {
    if (protocol == "openai") {
        if (feature == "chat" || feature == "translate") return {"chat-completions", "/chat/completions"};
        if (feature == "transcribe") return {"openai-transcriptions", "/audio/transcriptions"};
        if (feature == "speak") return {"openai-speech", "/audio/speech"};
        if (feature == "image") return {"openai-images", "/images/generations"};
        if (feature == "video") return {"openai-videos", "/videos"};
        // QA-01: OpenAI has no sound-effects endpoint; servers that make them (LocalAI) take ElevenLabs' request at
        // /v1/sound-generation, so that is the default path (after a base URL ending in /v1).
        if (feature == "sound") return {"sound-generation", "/sound-generation"};
    } else if (protocol == "anthropic") {
        if (feature == "chat" || feature == "translate") return {"anthropic-messages", "/v1/messages"};
    } else if (protocol == "gemini") {
        if (feature == "chat" || feature == "translate") return {"gemini-interactions", "/v1beta/interactions"};
    }
    return {};
}

/// What a custom (or user-added OpenAI-compatible) generation model of `feature` makes.
void markGeneration(ModelSpec& m, std::string_view feature) {
    if (feature == "image") {
        m.generation.image = true;
    } else if (feature == "video") {
        m.generation.video = true;
    } else if (feature == "sound") { // QA-01: ElevenLabs-style lengths (0 = the server picks)
        m.generation.soundEffect = true;
        m.generation.minSeconds = 0.5;
        m.generation.maxSeconds = 30.0;
    }
}

/// QA-05: catalogue APIs whose hosts speak the OpenAI API, so their other paths (/audio/..., /images/..., /videos,
/// /sound-generation) can serve models the user adds for other features.
bool openAiCompatibleApi(std::string_view api) { return api == "chat-completions" || api == "openai-responses"; }

QString protocolName(std::string_view protocol) {
    if (protocol == "openai") return QObject::tr("OpenAI-compatible");
    if (protocol == "anthropic") return QObject::tr("Anthropic-compatible");
    if (protocol == "gemini") return QObject::tr("Gemini-compatible");
    return qs(protocol);
}

/// The key header a custom provider uses with auth "header" when the user names none.
std::string_view defaultAuthHeader(std::string_view protocol) {
    if (protocol == "anthropic") return "x-api-key";
    if (protocol == "gemini") return "x-goog-api-key";
    return "api-key";
}

std::string trimmed(std::string_view text) {
    constexpr std::string_view kSpace = " \t\r\n";
    const auto first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kSpace);
    return std::string(text.substr(first, last - first + 1));
}

bool hasControl(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
}

bool validHeaderName(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '-' || c == '_')) {
            return false;
        }
    }
    for (const std::string_view banned : {"host", "content-length", "content-type", "transfer-encoding", "connection",
                                          "cookie", "te", "upgrade", "expect", "range", "accept", "accept-encoding"}) {
        if (equalsIgnoreCase(name, banned)) {
            return false;
        }
    }
    return true;
}

/// Headers that carry credentials: a custom provider's key goes in the key store, never into an extra header (which
/// is persisted in plain settings).
bool secretHeader(std::string_view name) {
    for (const std::string_view secret : {"authorization", "proxy-authorization", "x-api-key", "xi-api-key", "x-goog-api-key",
                                          "api-key", "ocp-apim-subscription-key", "x-key", "set-cookie"}) {
        if (equalsIgnoreCase(name, secret)) {
            return true;
        }
    }
    return false;
}

/// A custom provider built from its spec: the registry entries and the normalised spec that is persisted.
struct CustomProvider {
    ProviderSpec provider;
    std::vector<ModelSpec> models;
    Json spec;
};

/// Validates a custom provider spec (from the UI or from settings: untrusted either way) and builds its registry
/// entries. Errors are user-facing sentences.
std::expected<CustomProvider, QString> buildCustomProvider(const Json& j, const std::string& id,
                                                           const ProviderRegistry& adapters) {
    if (!j.is_object()) {
        return std::unexpected(QObject::tr("The provider description is not valid."));
    }
    const auto text = [&](const char* key) {
        const auto it = j.find(key);
        return it != j.end() && it->is_string() ? trimmed((it->get<std::string>())) : std::string();
    };
    CustomProvider out;
    ProviderSpec& p = out.provider;
    p.id = id;
    p.name = text("name");
    if (p.name.empty()) {
        return std::unexpected(QObject::tr("Give the provider a name."));
    }
    if (p.name.size() > 80 || hasControl(p.name)) {
        return std::unexpected(QObject::tr("Use a shorter name on one line."));
    }
    p.protocol = text("protocol");
    if (p.protocol.empty()) {
        p.protocol = "openai";
    }
    if (customRoute(p.protocol, "chat").api.empty()) {
        return std::unexpected(QObject::tr("Choose the kind of API the provider speaks."));
    }
    std::string url = text("baseUrl");
    while (url.size() > 8 && url.back() == '/') {
        url.pop_back();
    }
    const bool http = url.starts_with("http://") || url.starts_with("https://");
    const std::string_view authority = http ? std::string_view(url).substr(url.find("//") + 2) : std::string_view();
    const std::string_view host = authority.substr(0, authority.find('/'));
    if (!http || url.size() > 2048 || host.empty() || hasControl(url) || url.find_first_of(" {}\\\"<>") != std::string::npos) {
        return std::unexpected(QObject::tr("Enter the address as http://host:port/path or https://..."));
    }
    if (host.find('@') != std::string_view::npos) {
        return std::unexpected(QObject::tr("Leave the user name and password out of the address: put the key in the key field."));
    }
    p.baseUrl = url;
    p.local = isLoopbackUrl(url); // a server on this computer counts as local: no consent, allowed offline
    p.configurableBaseUrl = false; // the address is edited in the provider itself
    const std::string auth = text("auth").empty() ? std::string("bearer") : text("auth");
    if (auth == "bearer") {
        p.auth = AuthScheme::Bearer;
    } else if (auth == "none") {
        p.auth = AuthScheme::None;
    } else if (auth == "header") {
        p.auth = AuthScheme::Header;
        p.authHeader = text("authHeader").empty() ? std::string(defaultAuthHeader(p.protocol)) : text("authHeader");
        if (!validHeaderName(p.authHeader)) {
            return std::unexpected(QObject::tr("The key header name can use letters, digits, - and _ only."));
        }
    } else {
        return std::unexpected(QObject::tr("Choose how the key is sent."));
    }
    p.keyRequired = p.auth != AuthScheme::None;
    if (const auto it = j.find("headers"); it != j.end() && !it->is_null()) {
        if (!it->is_object() || it->size() > 20) {
            return std::unexpected(QObject::tr("Use at most 20 extra headers."));
        }
        for (const auto& [name, value] : it->items()) {
            if (!validHeaderName(name)) {
                return std::unexpected(QObject::tr("The header name \"%1\" is not allowed.").arg(qs(name.substr(0, 64))));
            }
            if (secretHeader(name)) {
                return std::unexpected(QObject::tr("Put the key in the key field, not in the \"%1\" header: headers are saved in plain settings.").arg(qs(name)));
            }
            if (!value.is_string() || value.get<std::string>().size() > 1024 || hasControl(value.get<std::string>())) {
                return std::unexpected(QObject::tr("The value of the header \"%1\" must be one line of text.").arg(qs(name)));
            }
            p.extraHeaders.push_back({name, value.get<std::string>()});
        }
    }
    if (p.protocol == "anthropic" && !findHeader(p.extraHeaders, "anthropic-version")) {
        p.extraHeaders.push_back({"anthropic-version", "2023-06-01"});
    }
    std::map<std::string, std::string> paths;
    if (const auto it = j.find("paths"); it != j.end() && !it->is_null()) {
        if (!it->is_object()) {
            return std::unexpected(QObject::tr("The API paths are not valid."));
        }
        for (const auto& [feature, value] : it->items()) {
            if (!isFeature(feature) || !value.is_string()) {
                return std::unexpected(QObject::tr("The API paths are not valid."));
            }
            const std::string path = trimmed(value.get<std::string>());
            if (path.empty()) {
                continue;
            }
            if (!path.starts_with('/') || path.starts_with("//") || path.size() > 512 || hasControl(path) ||
                path.find_first_of(" {}\\\"<>") != std::string::npos) {
                return std::unexpected(QObject::tr("API paths start with / and stay on the provider's address (\"%1\").").arg(qs(path.substr(0, 64))));
            }
            paths[feature] = path;
        }
    }
    p.api = std::string(customRoute(p.protocol, "chat").api);
    p.keyCheck.method = "GET";
    p.keyCheck.path = p.protocol == "anthropic" ? "/v1/models?limit=1" : p.protocol == "gemini" ? "/v1beta/models?pageSize=1" : "/models";
    p.description = QObject::tr("Your own %1 provider").arg(protocolName(p.protocol)).toStdString();
    p.dataPolicy = p.local ? QObject::tr("Runs on this computer; nothing is uploaded.").toStdString()
                           : QObject::tr("Sent to %1. Wabcut does not know this provider's data policy: check it with whoever runs the server.")
                                 .arg(qs(host))
                                 .toStdString();
    p.verified = false;
    p.source = "user";
    p.custom = true;

    Json models = Json::array();
    std::map<std::string, std::size_t> byId; // a model listed for chat and translate is one entry
    if (const auto it = j.find("models"); it != j.end() && !it->is_null()) {
        if (!it->is_array() || it->size() > 500) {
            return std::unexpected(QObject::tr("Use at most 500 models."));
        }
        for (const auto& entry : *it) {
            std::string modelId;
            std::string feature = "chat";
            if (entry.is_string()) {
                modelId = trimmed((entry.get<std::string>()));
            } else if (entry.is_object()) {
                if (const auto m = entry.find("id"); m != entry.end() && m->is_string()) {
                    modelId = trimmed((m->get<std::string>()));
                }
                if (const auto c = entry.find("capability"); c != entry.end() && c->is_string() && !c->get<std::string>().empty()) {
                    feature = c->get<std::string>();
                }
            }
            if (modelId.empty()) {
                continue;
            }
            if (modelId.size() > 200 || hasControl(modelId)) {
                return std::unexpected(QObject::tr("The model id \"%1\" is not valid.").arg(qs(modelId.substr(0, 64))));
            }
            const CustomRoute route = customRoute(p.protocol, feature);
            if (route.api.empty() || !isFeature(feature)) {
                return std::unexpected(QObject::tr("%1 providers cannot do %2.").arg(protocolName(p.protocol), featureName(feature)));
            }
            const auto capability = *featureCapability(feature);
            if (!adapters.hasAdapter(route.api, capability == Capability::Translate ? Capability::Chat : capability)) {
                return std::unexpected(QObject::tr("%1 through %2 is not supported in this version yet.").arg(featureName(feature), protocolName(p.protocol)));
            }
            const bool chatLike = feature == "chat" || feature == "translate";
            if (const auto found = byId.find(modelId); found != byId.end()) {
                ModelSpec& existing = out.models[found->second];
                const bool existingChat = existing.caps.has(Capability::Chat);
                if (!(chatLike && existingChat)) {
                    return std::unexpected(QObject::tr("The model \"%1\" is listed twice.").arg(qs(modelId)));
                }
                continue; // chat and translate: one chat model serves both
            }
            ModelSpec m;
            m.id = modelId;
            m.provider = p.id;
            m.name = modelId;
            m.api = std::string(route.api);
            m.endpoint = paths.contains(feature) ? paths[feature] : std::string(route.path);
            if (chatLike) {
                m.caps.capabilities = {Capability::Chat, Capability::Translate};
                m.caps.toolsOnChat = true;
                m.caps.forcedToolChoice = false; // many compatible servers reject a forced tool_choice
            } else {
                m.caps.capabilities = {capability};
                m.caps.streaming = false;
                markGeneration(m, feature);
            }
            m.prices = {PriceRule{}}; // unknown prices: nothing is booked, so spend caps do not cover custom providers
            m.license = "unknown";
            m.source = "user";
            m.notes = "Added by you.";
            byId.emplace(modelId, out.models.size());
            out.models.push_back(std::move(m));
            models.push_back({{"id", modelId}, {"capability", feature}});
        }
    }
    Json headers = Json::object();
    for (const auto& h : p.extraHeaders) {
        if (!(p.protocol == "anthropic" && equalsIgnoreCase(h.name, "anthropic-version") && h.value == "2023-06-01" &&
              !(j.contains("headers") && j["headers"].is_object() && j["headers"].contains(h.name)))) {
            headers[h.name] = h.value;
        }
    }
    out.spec = {{"id", p.id},         {"name", p.name}, {"protocol", p.protocol}, {"baseUrl", p.baseUrl},
                {"auth", auth},       {"authHeader", p.auth == AuthScheme::Header ? p.authHeader : std::string()},
                {"headers", headers}, {"models", models}, {"paths", paths}};
    return out;
}

/// Model ids are unique across the registry: a custom model whose name another provider already uses is registered as
/// "<provider>:<name>" and keeps the name as its apiId (what the adapter sends).
void scopeModelIds(std::vector<ModelSpec>& models, const ModelRegistry& registry, const std::string& providerId) {
    for (ModelSpec& m : models) {
        if (const ModelSpec* other = registry.model(m.id); other && other->provider != providerId) {
            m.apiId = m.id;
            m.id = providerId + ":" + m.id;
        }
    }
}

/// A best guess at what a listed model does, from its id (GET /models says nothing about it).
QString guessFeature(QString id) {
    id = id.toLower();
    const auto any = [&](std::initializer_list<const char*> words) {
        return std::any_of(words.begin(), words.end(), [&](const char* w) { return id.contains(QLatin1String(w)); });
    };
    if (any({"whisper", "transcribe", "stt", "voxtral", "scribe", "nova-"})) return QStringLiteral("transcribe");
    if (any({"sfx", "sound-effect", "sound_effect", "sound-generation", "audioldm", "stable-audio", "audiogen"})) return QStringLiteral("sound");
    if (any({"tts", "speech", "voice", "sonic", "aura"})) return QStringLiteral("speak");
    if (any({"sora", "video", "veo", "kling", "wan2", "wan-", "hunyuanvideo", "ltx", "seedance", "hailuo"})) return QStringLiteral("video");
    if (any({"dall-e", "gpt-image", "image", "flux", "sdxl", "stable-diffusion", "imagen", "ideogram", "recraft", "seedream"})) return QStringLiteral("image");
    if (any({"embed", "rerank", "moderation", "guard"})) return QString(); // not routable
    return QStringLiteral("chat");
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
    // r07: per-feature defaults ("" or missing = automatic), the local assistant tier this computer runs best (probed
    // once at load), the custom providers' normalised specs (persisted; never a key), and which managed local models
    // are downloaded (refreshed by applyRouting()).
    std::map<std::string, std::string, std::less<>> defaults;
    std::string localDefault;
    std::map<std::string, Json, std::less<>> customSpecs;
    std::set<std::string, std::less<>> installedLocal;
    std::set<std::string> libraryModels; // r07: registered by the local model library, not saved as "userModels"
    int draftSerial = 0;                 // QA-08: request ids of checks and listings of unsaved custom providers
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
                self->applyRouting(); // offline mode / consent change what the automatic choices can use
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
                applyRouting();
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
    applyRouting(); // r07: the project's no-cloud lock changes what "automatic" can use
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
    // 0.2: the AI helpers' CPU cap, for every helper this process starts.
    {
        HelperCpuPolicy policy = helperCpuPolicy();
        policy.cpuRatePercent = std::clamp(settings.value(QStringLiteral("helperCpuLimit"), kDefaultHelperCpuPercent).toInt(), 10, 100);
        policy.lowPriority = true;
        setHelperCpuPolicy(policy);
    }
    settings.endGroup();
    loadCustomProviders(); // r07: before the user models, which may belong to one
    settings.beginGroup(kGroup);
    const QStringList userModels = settings.value(QStringLiteral("userModels")).toStringList();
    for (const QString& entry : userModels) {
        const auto parts = entry.split(QLatin1Char('\t'));
        if (parts.size() == 2 || parts.size() == 3) { // r07: provider \t model [\t feature]
            (void)addModel(parts[0], parts[1], parts.size() == 3 ? parts[2] : QStringLiteral("chat"));
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
    // AI-15: until the user picks one, the local assistant tier this computer runs best (Tiers.h). r07: that is now the
    // first local choice of the automatic order (applyRouting()).
    d->localDefault = std::string(llm::defaultModel(llm::probeSystem()));
    // r07: the local model library's storage folder, downloaded models and launch settings (LocalModelsController.h).
    // QA-03: before the saved defaults are read, which may name a downloaded model.
    wabcut::app::loadLocalModelLibrary(*this);
    if (settings.value(QStringLiteral("routingVersion")).toInt() >= 2) {
        settings.beginGroup(QStringLiteral("defaults"));
        for (const std::string_view feature : featureIds()) {
            const QString id = settings.value(qs(feature)).toString();
            if (!id.isEmpty() && d->models.model(str(id)) && servesFeature(*d->models.model(str(id)), feature)) {
                d->defaults[std::string(feature)] = str(id);
            }
        }
        settings.endGroup();
    } else {
        // Before r07 the assistant's preference was saved even when nobody picked it: the local default tier means
        // "automatic"; anything else was the user's choice.
        const auto chat = preference("chatPreference", {});
        if (!chat.empty() && chat.front() != d->localDefault && d->models.model(chat.front())) {
            d->defaults["chat"] = chat.front();
        }
        const auto transcribe = preference("transcriptionPreference", {});
        if (!transcribe.empty() && d->models.model(transcribe.front())) {
            d->defaults["transcribe"] = transcribe.front();
        }
    }
    if (const auto consent = Json::parse(str(settings.value(QStringLiteral("consent")).toString()), nullptr, false);
        consent.is_object()) {
        (void)d->consent.loadJson(consent);
    }
    if (const auto spend = Json::parse(str(settings.value(QStringLiteral("spend")).toString()), nullptr, false);
        spend.is_object()) {
        (void)d->spend.loadJson(spend);
    }
    settings.endGroup();
    applyRouting();
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
    // Older versions read these two (the effective lists); r07 reads "defaults/<feature>" ("" = automatic).
    settings.setValue(QStringLiteral("chatPreference"), ids(d->providers->preference(Capability::Chat)));
    settings.setValue(QStringLiteral("transcriptionPreference"),
                      d->defaults.contains("transcribe") ? QStringList{qs(d->defaults.at("transcribe"))} : QStringList());
    settings.setValue(QStringLiteral("routingVersion"), 2);
    settings.remove(QStringLiteral("defaults"));
    for (const std::string_view feature : featureIds()) {
        const auto it = d->defaults.find(feature);
        settings.setValue(QStringLiteral("defaults/") + qs(feature), it == d->defaults.end() ? QString() : qs(it->second));
    }
    settings.setValue(QStringLiteral("consent"), qs(d->consent.toJson().dump()));
    settings.setValue(QStringLiteral("spend"), qs(d->spend.toJson().dump()));
    QStringList userModels;
    for (const auto& m : d->models.models()) {
        if (d->models.isUserModel(m.id) && !d->libraryModels.contains(m.id)) {
            const auto features = featuresOf(m);
            const QString feature = features.empty() || features.front() == "chat" ? QString() : qs(features.front());
            // The provider's own name (apiId for "<provider>:<name>" entries): addModel() scopes it again on load.
            userModels << qs(m.provider) + QLatin1Char('\t') + qs(apiModelId(m)) + (feature.isEmpty() ? QString() : QLatin1Char('\t') + feature);
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
        const std::string baseUrl = d->providers->baseUrl(p);
        const bool onThisComputer = !isCloud(p, baseUrl);
        // r07: what it can do = its API's own features, the catalogue's and its models'; supported = some adapter.
        std::set<std::string, std::less<>> features;
        bool supported = false;
        for (const auto& f : featuresOfApi(p.api)) {
            features.insert(f);
            supported = supported || d->providers->hasAdapter(p.api, Capability::Chat);
        }
        for (const auto& f : p.features) {
            features.insert(f);
        }
        const auto today = d->platform->today();
        QVariantList models;
        const auto modelsOfProvider = d->models.modelsOf(p.id);
        for (const ModelSpec* model : modelsOfProvider) {
            QStringList modelFeatures;
            for (const auto& f : featuresOf(*model)) {
                features.insert(f);
                modelFeatures << qs(f);
                const auto capability = featureCapability(f);
                supported = supported || (capability && d->providers->candidate(model->id, *capability, today).has_value());
            }
            models << QVariantMap{{QStringLiteral("id"), qs(model->id)},
                                  {QStringLiteral("name"), qs(model->name.empty() ? model->id : model->name)},
                                  {QStringLiteral("capabilities"), modelFeatures},
                                  {QStringLiteral("userAdded"), d->models.isUserModel(model->id) || p.custom}};
        }
        QStringList featureList;
        QStringList featureNames;
        QStringList addable; // QA-05: what addModel() takes for this provider (its own kinds, or every OpenAI path)
        for (const std::string_view f : featureIds()) {
            if (features.contains(f)) {
                featureList << qs(f);
                featureNames << featureName(f);
            }
            const std::string_view protocol = p.custom ? std::string_view(p.protocol) : openAiCompatibleApi(p.api) ? "openai" : "";
            const CustomRoute route = protocol.empty() ? CustomRoute{} : customRoute(protocol, f);
            const auto capability = featureCapability(f);
            if (features.contains(f) ||
                (!route.api.empty() && d->providers->hasAdapter(route.api, *capability == Capability::Translate ? Capability::Chat : *capability))) {
                addable << qs(f);
            }
        }
        const QString lastError = settings.value(meta + QStringLiteral("error")).toString();
        QString status;
        QString text;
        if (!supported) {
            status = QStringLiteral("notSupported");
            text = tr("Not supported yet");
        } else if (needsAddress(baseUrl)) {
            status = QStringLiteral("needsKey");
            text = tr("Set its address first");
        } else if (onThisComputer) {
            status = QStringLiteral("local");
            text = tr("On this computer");
        } else if (!p.keyRequired) {
            status = QStringLiteral("ready");
            text = tr("No key needed");
        } else if (!stored && !fromEnv) {
            status = QStringLiteral("needsKey");
            text = tr("No key");
        } else if (!lastError.isEmpty()) {
            status = QStringLiteral("error");
            text = lastError;
        } else if (fromEnv) {
            status = QStringLiteral("ready");
            text = tr("Key from %1").arg(qs(p.envVar));
        } else {
            status = QStringLiteral("ready");
            text = tr("Key ••••%1 · %2").arg(last4, keyStoreName());
        }
        m[QStringLiteral("status")] = status;
        m[QStringLiteral("statusText")] = text;
        m[QStringLiteral("lastError")] = lastError;
        m[QStringLiteral("consoleUrl")] = qs(p.consoleUrl);
        m[QStringLiteral("termsUrl")] = qs(p.termsUrl);
        m[QStringLiteral("dataPolicy")] = qs(p.dataPolicy);
        m[QStringLiteral("baseUrl")] = qs(baseUrl);
        m[QStringLiteral("configurableBaseUrl")] = p.configurableBaseUrl;
        m[QStringLiteral("verified")] = p.verified;
        m[QStringLiteral("capabilities")] = featureList;
        m[QStringLiteral("capabilityNames")] = featureNames;
        m[QStringLiteral("addableCapabilities")] = addable;
        m[QStringLiteral("category")] = p.custom ? QStringLiteral("custom") : onThisComputer || p.local ? QStringLiteral("local") : QStringLiteral("cloud");
        m[QStringLiteral("custom")] = p.custom;
        m[QStringLiteral("description")] = qs(p.description);
        m[QStringLiteral("website")] = qs(p.website);
        m[QStringLiteral("api")] = qs(p.api);
        m[QStringLiteral("protocol")] = qs(p.protocol);
        m[QStringLiteral("modelCount")] = static_cast<int>(modelsOfProvider.size());
        m[QStringLiteral("models")] = models;
        out << m;
    }
    return out;
}

QVariantList AiController::modelsWith(Capability capability) const {
    const QString feature = capability == Capability::Chat ? QStringLiteral("chat")
                            : capability == Capability::Transcribe ? QStringLiteral("transcribe")
                            : capability == Capability::Speak ? QStringLiteral("speak")
                            : capability == Capability::Translate ? QStringLiteral("translate")
                                                                  : QString();
    QVariantList out;
    for (const ModelSpec* model : d->models.modelsWith(capability)) {
        if (d->models.provider(model->provider)) {
            out << modelRow(*model, feature);
        }
    }
    return out;
}

bool AiController::hasKeyFor(const ProviderSpec& provider) const {
    if (!provider.keyRequired) {
        return true;
    }
    if (!provider.envVar.empty() && systemEnv(provider.envVar)) {
        return true;
    }
    return QSettings().value(QStringLiteral("%1/keys/%2/hasKey").arg(QLatin1String(kGroup), qs(provider.id))).toBool();
}

QString AiController::unusableReason(const ModelSpec& model, Capability capability) const {
    const ProviderSpec* provider = d->models.provider(model.provider);
    if (!provider) {
        return tr("Unavailable");
    }
    const std::string baseUrl = d->providers->baseUrl(*provider);
    if (auto c = d->providers->candidate(model.id, capability, d->platform->today()); !c) {
        return c.error().code == AiErrorCode::Unsupported ? tr("Not supported yet")
               : c.error().code == AiErrorCode::Deprecated ? tr("Retired")
                                                             : tr("Unavailable");
    }
    if (needsAddress(baseUrl)) {
        return tr("Set the %1 address first").arg(qs(provider->name));
    }
    if (provider->keyRequired && !hasKeyFor(*provider)) {
        return tr("Needs a %1 key").arg(qs(provider->name));
    }
    if (isCloud(*provider, baseUrl) && !d->consent.cloudAllowed()) {
        return tr("Offline");
    }
    return {};
}

QVariantMap AiController::modelRow(const ModelSpec& model, const QString& feature) const {
    const auto today = d->platform->today();
    const ProviderSpec* provider = d->models.provider(model.provider);
    const Capability capability = featureCapability(str(feature)).value_or(Capability::Chat);
    QVariantMap m;
    m[QStringLiteral("id")] = qs(model.id);
    m[QStringLiteral("name")] = qs(model.name.empty() ? model.id : model.name);
    m[QStringLiteral("provider")] = provider ? qs(provider->id) : QString();
    m[QStringLiteral("providerName")] = provider ? qs(provider->name) : QString();
    m[QStringLiteral("price")] = provider && provider->custom ? tr("Price not tracked") : formatPrice(model, today);
    QStringList badges;
    if (capability == Capability::Chat || capability == Capability::Translate) {
        if (model.caps.toolsOnChat || model.caps.toolsOnResponses) {
            badges << tr("Tools");
        }
        if (model.caps.vision) {
            badges << tr("Images");
        }
        if (model.caps.videoInput == VideoInput::Native) {
            badges << tr("Video");
        }
    } else if (capability == Capability::Transcribe) {
        if (model.caps.wordTimestamps) {
            badges << tr("Word timing");
        }
        if (model.caps.diarization) {
            badges << tr("Speakers");
        }
    } else if (capability == Capability::GenerateMedia) {
        if (model.generation.audio) {
            badges << tr("With sound");
        }
        if (model.generation.firstFrame) {
            badges << tr("From an image");
        }
    }
    if (model.retirement) {
        badges << tr("Until %1").arg(qs(formatDate(*model.retirement)));
    }
    m[QStringLiteral("badges")] = badges;
    m[QStringLiteral("verified")] = model.verified;
    m[QStringLiteral("notes")] = qs(model.notes);
    // r07: models of the local model library are managed there (Settings > AI > Local models), not as user models.
    const bool library = d->libraryModels.contains(model.id);
    m[QStringLiteral("userAdded")] = !library && (d->models.isUserModel(model.id) || (provider && provider->custom));
    m[QStringLiteral("library")] = library;
    QString reason = unusableReason(model, capability);
    const bool local = provider && !isCloud(*provider, d->providers->baseUrl(*provider));
    if (reason.isEmpty() && provider && provider->id == "local-llm" && !d->installedLocal.contains(model.id) &&
        findModel(model.id) != nullptr) {
        reason = tr("Download it in Local models first");
    }
    m[QStringLiteral("usable")] = reason.isEmpty();
    m[QStringLiteral("reason")] = reason;
    m[QStringLiteral("local")] = local;
    m[QStringLiteral("capability")] = feature;
    return m;
}

QVariantList AiController::chatModels() const { return modelsWith(Capability::Chat); }
QVariantList AiController::transcriptionModels() const { return modelsWith(Capability::Transcribe); }
QVariantList AiController::speechModels() const { return modelsWith(Capability::Speak); }

QString AiController::chatModel() const {
    const auto pref = d->providers->preference(Capability::Chat);
    return pref.empty() ? QString() : qs(pref.front());
}

void AiController::setChatModel(const QString& id) { (void)setDefaultModel(QStringLiteral("chat"), id); }

QString AiController::transcriptionModel() const { return defaultModel(QStringLiteral("transcribe")); }

void AiController::setTranscriptionModel(const QString& id) { (void)setDefaultModel(QStringLiteral("transcribe"), id); }

// --- r07: routing -------------------------------------------------------------------------------------------------

QVariantList AiController::capabilityList() const {
    QVariantList out;
    for (const std::string_view f : featureIds()) {
        out << QVariantMap{{QStringLiteral("id"), qs(f)}, {QStringLiteral("name"), featureName(f)},
                           {QStringLiteral("description"), featureDescription(f)}};
    }
    return out;
}

QVariantList AiController::modelsFor(const QString& capability) const {
    const std::string feature = str(capability);
    QVariantList local;
    QVariantList cloud;
    if (!isFeature(feature)) {
        return {};
    }
    for (const auto& model : d->models.models()) {
        if (!servesFeature(model, feature) || !d->models.provider(model.provider)) {
            continue;
        }
        QVariantMap row = modelRow(model, capability);
        (row.value(QStringLiteral("local")).toBool() ? local : cloud) << row;
    }
    return local + cloud;
}

QString AiController::defaultModel(const QString& capability) const {
    const auto it = d->defaults.find(str(capability));
    return it == d->defaults.end() ? QString() : qs(it->second);
}

bool AiController::setDefaultModel(const QString& capability, const QString& modelId) {
    const std::string feature = str(capability);
    if (!isFeature(feature)) {
        return false;
    }
    const std::string id = str(modelId.trimmed());
    if (!id.empty()) {
        const ModelSpec* model = d->models.model(id);
        if (!model || !servesFeature(*model, feature)) {
            return false;
        }
    }
    const auto it = d->defaults.find(feature);
    const std::string previous = it == d->defaults.end() ? std::string() : it->second;
    if (previous == id) {
        return true;
    }
    if (id.empty()) {
        d->defaults.erase(feature);
    } else {
        d->defaults[feature] = id;
    }
    applyRouting();
    saveSettings();
    emit modelsChanged();
    emit statusChanged();
    return true;
}

void AiController::applyRouting() {
    const auto today = d->platform->today();
    // Which managed local models are downloaded (stat + marker per file; models a library added at runtime count as
    // ready: they are registered once installed).
    d->installedLocal.clear();
    {
        const ModelStore store(ModelStore::defaultRoot());
        for (const auto* model : d->models.modelsOf("local-llm")) {
            const ModelInfo* info = findModel(model->id);
            if (!info || store.isInstalled(*info)) {
                d->installedLocal.insert(model->id);
            }
        }
    }
    // The automatic order of a feature, local first wherever a local option exists: local models that can run now,
    // then local models that still need a download (the tier this computer runs best first: AI-15's rule that the
    // assistant stays on this computer until the user picks a cloud model - saving a key never moves it), then cloud
    // models that are usable now, then the rest (so the error of an unusable choice names the first fix).
    const auto automatic = [&](std::string_view feature) {
        const Capability capability = *featureCapability(feature);
        std::vector<std::string> localReady;
        std::vector<std::string> cloudReady;
        std::vector<std::string> localLater;
        std::vector<std::string> cloudLater;
        for (const auto& model : d->models.models()) {
            if (!servesFeature(model, feature) || !d->providers->candidate(model.id, capability, today)) {
                continue;
            }
            const ProviderSpec* provider = d->models.provider(model.provider);
            if (!provider) {
                continue;
            }
            const bool local = !isCloud(*provider, d->providers->baseUrl(*provider));
            const bool ready = unusableReason(model, capability).isEmpty() &&
                               (provider->id != "local-llm" || d->installedLocal.contains(model.id));
            auto& bucket = local ? (ready ? localReady : localLater) : (ready ? cloudReady : cloudLater);
            if (model.id == d->localDefault) {
                bucket.insert(bucket.begin(), model.id);
            } else {
                bucket.push_back(model.id);
            }
        }
        std::vector<std::string> out = std::move(localReady);
        out.insert(out.end(), localLater.begin(), localLater.end());
        out.insert(out.end(), cloudReady.begin(), cloudReady.end());
        out.insert(out.end(), cloudLater.begin(), cloudLater.end());
        return out;
    };
    const auto chosen = [&](std::string_view feature) -> std::vector<std::string> {
        if (const auto it = d->defaults.find(feature); it != d->defaults.end() && d->models.model(it->second)) {
            return {it->second};
        }
        return automatic(feature);
    };
    d->providers->setPreference(Capability::Chat, chosen("chat"));
    // Transcription: whisper.cpp on this computer comes first in "automatic" (TranscribeService / the Transcript panel);
    // these are the cloud models after it.
    d->providers->setPreference(Capability::Transcribe, chosen("transcribe"));
    // Translation follows the assistant unless the user picked a model (or a translation service) for it.
    d->providers->setPreference(Capability::Translate, d->defaults.contains("translate") ? chosen("translate")
                                                                                          : d->providers->preference(Capability::Chat));
    d->providers->setPreference(Capability::Speak, chosen("speak"));
    std::vector<std::string> media;
    for (const char* feature : {"video", "image", "sound"}) {
        for (const auto& id : chosen(feature)) {
            if (std::find(media.begin(), media.end(), id) == media.end()) {
                media.push_back(id);
            }
        }
    }
    d->providers->setPreference(Capability::GenerateMedia, media);
}

QVariantMap AiController::routingRow(const QString& capability) const {
    const std::string feature = str(capability);
    QVariantMap row{{QStringLiteral("capability"), capability},
                    {QStringLiteral("name"), featureName(feature)},
                    {QStringLiteral("model"), QString()},
                    {QStringLiteral("modelName"), QString()},
                    {QStringLiteral("provider"), QString()},
                    {QStringLiteral("providerName"), QString()},
                    {QStringLiteral("usable"), false},
                    {QStringLiteral("reason"), QString()},
                    {QStringLiteral("local"), false},
                    {QStringLiteral("automatic"), !d->defaults.contains(feature)}};
    const auto fill = [&](const ModelSpec& model) {
        const QVariantMap m = modelRow(model, capability);
        row[QStringLiteral("model")] = m.value(QStringLiteral("id"));
        row[QStringLiteral("modelName")] = m.value(QStringLiteral("name"));
        row[QStringLiteral("provider")] = m.value(QStringLiteral("provider"));
        row[QStringLiteral("providerName")] = m.value(QStringLiteral("providerName"));
        row[QStringLiteral("usable")] = m.value(QStringLiteral("usable"));
        row[QStringLiteral("reason")] = m.value(QStringLiteral("reason"));
        row[QStringLiteral("local")] = m.value(QStringLiteral("local"));
    };
    std::error_code missing;
    if (feature == "transcribe" && !d->defaults.contains(feature) && std::filesystem::exists(defaultInferExecutable(), missing)) {
        row[QStringLiteral("modelName")] = tr("Whisper on this computer");
        row[QStringLiteral("provider")] = QStringLiteral("local");
        row[QStringLiteral("providerName")] = tr("This computer");
        row[QStringLiteral("usable")] = true;
        row[QStringLiteral("local")] = true;
        return row;
    }
    const auto capabilityOf = featureCapability(feature);
    if (!capabilityOf) {
        return row;
    }
    // What a feature's resolve() picks: the first model of the effective list it accepts (a local tier still to download
    // included: it shows as not usable with "Download it in Local models first"), else the first one with the reason it
    // cannot run.
    std::vector<std::string> list;
    if (*capabilityOf == Capability::GenerateMedia) {
        if (const auto it = d->defaults.find(feature); it != d->defaults.end()) {
            list = {it->second};
        } else {
            for (const auto& id : d->providers->preference(Capability::GenerateMedia)) {
                if (const ModelSpec* m = d->models.model(id); m && servesFeature(*m, feature)) {
                    list.push_back(id);
                }
            }
        }
    } else {
        list = d->providers->preference(*capabilityOf);
    }
    const ModelSpec* first = nullptr;
    for (const auto& id : list) {
        const ModelSpec* model = d->models.model(id);
        if (!model || !servesFeature(*model, feature)) {
            continue;
        }
        first = first ? first : model;
        if (unusableReason(*model, *capabilityOf).isEmpty()) {
            fill(*model);
            return row;
        }
    }
    if (first) {
        fill(*first);
    } else {
        row[QStringLiteral("reason")] = tr("Add a provider that offers %1").arg(featureName(feature).toLower());
    }
    return row;
}

QVariantList AiController::routing() const {
    QVariantList out;
    for (const std::string_view f : featureIds()) {
        out << routingRow(qs(f));
    }
    return out;
}

QString AiController::routedModel(const QString& capability) const {
    const QVariantMap row = routingRow(capability);
    return row.value(QStringLiteral("usable")).toBool() ? row.value(QStringLiteral("model")).toString() : QString();
}

bool AiController::offlineMode() const { return d->consent.offlineMode(); }

void AiController::setOfflineMode(bool offline) {
    if (offline == offlineMode()) {
        return;
    }
    d->consent.setOfflineMode(offline); // persisted and signalled through the ledger's callback
}

int AiController::helperCpuLimit() const { return helperCpuPolicy().cpuRatePercent; }

void AiController::setHelperCpuLimit(int percent) {
    percent = std::clamp(percent, 10, 100);
    if (percent == helperCpuLimit()) {
        return;
    }
    HelperCpuPolicy policy = helperCpuPolicy();
    policy.cpuRatePercent = percent;
    setHelperCpuPolicy(policy);
    QSettings().setValue(QStringLiteral("%1/helperCpuLimit").arg(QLatin1String(kGroup)), percent);
    emit helperCpuLimitChanged();
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
    applyRouting();
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
    if (skipCheck || provider->keyCheck.path.empty()) { // r07: some providers have no call that checks a key for free
        finishKeyCheck(providerId, trimmed, true, {}, false);
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
    if (provider->keyCheck.path.empty()) { // r07
        emit keyCheckFinished(providerId, false, tr("%1 has no free way to check a key: it is checked on first use.").arg(qs(provider->name)));
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
                                  const std::expected<void, AiError>& result, bool checked) {
    const ProviderSpec* provider = d->models.provider(str(providerId));
    if (!provider) {
        return;
    }
    const bool usable = result.has_value() || result.error().code == AiErrorCode::QuotaExceeded;
    QString message;
    if (result) {
        message = !checked                ? (provider->keyCheck.path.empty()
                                                ? tr("Key saved. %1 has no free way to check a key, so it was not checked.").arg(qs(provider->name))
                                                : tr("Key saved without checking it."))
                  : store ? tr("Key saved and checked.")
                          : tr("%1 answered: the key works.").arg(qs(provider->name));
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
        if (result && checked) {
            settings.setValue(meta + QStringLiteral("validatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        }
        settings.remove(meta + QStringLiteral("error")); // r07: the provider's status is "ready" again
        applyRouting();
        emit providersChanged();
        emit modelsChanged();
        emit statusChanged();
    } else if (!store && result.error().code == AiErrorCode::Unauthorized) {
        // r07: a stored key the provider now rejects shows as the provider's "error" status until it works again.
        QSettings().setValue(QStringLiteral("%1/keys/%2/error").arg(QLatin1String(kGroup), providerId), message);
        emit providersChanged();
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
    applyRouting();
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
    applyRouting();
    saveSettings();
    emit providersChanged();
    emit modelsChanged();
    emit statusChanged();
    return true;
}

bool AiController::addModel(const QString& providerId, const QString& modelId, const QString& capability) {
    const std::string feature = capability.isEmpty() ? std::string("chat") : str(capability);
    const std::string id = str(modelId.trimmed());
    const ProviderSpec* provider = d->models.provider(str(providerId));
    if (!provider || !isFeature(feature) || id.empty() || id.size() > 200 || hasControl(id)) {
        return false;
    }
    if (provider->custom) { // r07: a custom provider's models live in its spec (validated and persisted with it)
        const auto spec = d->customSpecs.find(provider->id);
        if (spec == d->customSpecs.end()) {
            return false;
        }
        Json next = spec->second;
        next["models"].push_back({{"id", id}, {"capability", feature}});
        QVariantMap request = QJsonDocument::fromJson(QByteArray::fromStdString(next.dump())).toVariant().toMap();
        return saveCustomProvider(request).value(QStringLiteral("ok")).toBool();
    }
    ModelSpec m;
    m.provider = provider->id;
    m.id = id;
    if (const ModelSpec* other = d->models.model(id); other && other->provider != provider->id) {
        m.apiId = id; // another provider serves a model of that name: "<provider>:<name>" in the registry
        m.id = provider->id + ":" + id;
    }
    if (feature != "chat" && feature != "translate") {
        // Another kind of model of a built-in provider: it speaks the API of the provider's models of that kind.
        const ModelSpec* like = nullptr;
        for (const ModelSpec* other : d->models.modelsOf(provider->id)) {
            if (servesFeature(*other, feature)) {
                like = other;
                break;
            }
        }
        if (like) {
            m.api = like->api;
            m.endpoint = like->endpoint;
            m.caps.capabilities = like->caps.capabilities;
            m.caps.wordTimestamps = like->caps.wordTimestamps;
            m.caps.diarization = like->caps.diarization;
            m.caps.streaming = like->caps.streaming;
            m.generation = like->generation;
        } else {
            // QA-05: none of that kind yet (a gateway or a server of the user's: LiteLLM, LocalAI, vLLM, OpenRouter,
            // OpenAI's Sora): an OpenAI-compatible host takes the OpenAI path for it, as a custom provider would.
            const CustomRoute route = openAiCompatibleApi(provider->api) ? customRoute("openai", feature) : CustomRoute{};
            const Capability kind = *featureCapability(feature);
            if (route.api.empty() || !d->providers->hasAdapter(route.api, kind)) {
                return false;
            }
            m.api = std::string(route.api);
            m.endpoint = std::string(route.path);
            m.caps.capabilities = {kind};
            m.caps.streaming = false;
            markGeneration(m, feature);
            if (provider->local) {
                m.prices = {PriceRule{}}; // a server on the user's own machine bills nothing
            }
        }
    }
    if (!d->models.addModel(std::move(m))) {
        return false;
    }
    if (d->loaded) {
        applyRouting();
        saveSettings();
        emit providersChanged();
        emit modelsChanged();
    }
    return true;
}

bool AiController::registerLocalModel(ModelSpec spec) {
    const std::string id = spec.id;
    if (spec.provider != "local-llm" || id.empty() || (d->models.model(id) && !d->models.isUserModel(id))) {
        return false;
    }
    (void)d->models.removeUserModel(id); // replaced (its settings changed), or a stale "userModels" entry
    if (!d->models.addModel(std::move(spec))) {
        return false;
    }
    d->libraryModels.insert(id);
    applyRouting(); // the automatic order now knows the downloaded model
    emit modelsChanged();
    return true;
}

bool AiController::unregisterLocalModel(const QString& modelId) {
    const std::string id = str(modelId);
    if (d->libraryModels.erase(id) == 0) {
        return false;
    }
    (void)d->models.removeUserModel(id);
    if (d->local->status().model == id) {
        stopLocal();
    }
    applyRouting();
    emit modelsChanged();
    return true;
}

QStringList AiController::libraryModelIds() const {
    QStringList out;
    for (const auto& id : d->libraryModels) {
        out << qs(id);
    }
    return out;
}

bool AiController::runsLocally(const std::string& modelId) const {
    const ModelSpec* model = d->models.model(modelId);
    const ProviderSpec* provider = model ? d->models.provider(model->provider) : nullptr;
    return provider != nullptr && !isCloud(*provider, d->providers->baseUrl(*provider));
}

std::shared_ptr<const ModelRegistry> AiController::registrySnapshot() const { return std::make_shared<const ModelRegistry>(d->models); }

bool AiController::removeModel(const QString& modelId) {
    const ModelSpec* model = d->models.model(str(modelId));
    if (model && d->models.isCustomProvider(model->provider)) { // r07: out of the custom provider's spec
        const auto spec = d->customSpecs.find(model->provider);
        if (spec == d->customSpecs.end()) {
            return false;
        }
        Json next = spec->second;
        Json kept = Json::array();
        for (const auto& entry : next["models"]) {
            const bool same = entry.is_object() && entry.contains("id") && entry["id"].is_string() &&
                              entry["id"].get<std::string>() == apiModelId(*model);
            if (!same) {
                kept.push_back(entry);
            }
        }
        next["models"] = kept;
        QVariantMap request = QJsonDocument::fromJson(QByteArray::fromStdString(next.dump())).toVariant().toMap();
        return saveCustomProvider(request).value(QStringLiteral("ok")).toBool();
    }
    if (!d->models.removeUserModel(str(modelId))) {
        return false;
    }
    for (auto it = d->defaults.begin(); it != d->defaults.end();) { // a default on it becomes automatic
        it = it->second == str(modelId) ? d->defaults.erase(it) : std::next(it);
    }
    applyRouting();
    saveSettings();
    emit providersChanged();
    emit modelsChanged();
    emit statusChanged();
    return true;
}

// --- r07: custom providers ----------------------------------------------------------------------------------------

QVariantList AiController::customProtocols() const {
    QVariantList out;
    for (const std::string_view protocol : {std::string_view("openai"), std::string_view("anthropic"), std::string_view("gemini")}) {
        QStringList features;
        for (const std::string_view f : featureIds()) {
            const CustomRoute route = customRoute(protocol, f);
            const auto capability = featureCapability(f);
            if (!route.api.empty() && d->providers->hasAdapter(route.api, *capability == Capability::Translate ? Capability::Chat : *capability)) {
                features << qs(f);
            }
        }
        QString description;
        if (protocol == "openai") {
            description = tr("Servers that speak the OpenAI API: vLLM, LiteLLM, LocalAI, Jan, most hosted model APIs and gateways.");
        } else if (protocol == "anthropic") {
            description = tr("Servers and gateways that speak the Anthropic Messages API.");
        } else {
            description = tr("Servers and gateways that speak the Gemini API.");
        }
        out << QVariantMap{{QStringLiteral("id"), qs(protocol)}, {QStringLiteral("name"), protocolName(protocol)},
                           {QStringLiteral("capabilities"), features}, {QStringLiteral("description"), description}};
    }
    return out;
}

void AiController::loadCustomProviders() {
    QSettings settings;
    const auto parsed = Json::parse(str(settings.value(QStringLiteral("%1/customProviders").arg(QLatin1String(kGroup))).toString()),
                                    nullptr, false);
    if (!parsed.is_array()) {
        return;
    }
    for (const auto& entry : parsed) {
        // Settings are untrusted input too: each entry is validated like one from the dialog; bad ones are dropped.
        if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string()) {
            continue;
        }
        const std::string id = entry["id"].get<std::string>();
        if (!id.starts_with("custom-") || id.size() > 64 || d->models.provider(id)) {
            continue;
        }
        auto built = buildCustomProvider(entry, id, *d->providers);
        if (!built) {
            continue;
        }
        scopeModelIds(built->models, d->models, id);
        Json spec = built->spec;
        if (d->models.addCustomProvider(std::move(built->provider), std::move(built->models))) {
            d->customSpecs[id] = std::move(spec);
        }
    }
}

void AiController::saveCustomProviders() const {
    Json all = Json::array();
    for (const auto& [id, spec] : d->customSpecs) {
        all.push_back(spec);
    }
    QSettings().setValue(QStringLiteral("%1/customProviders").arg(QLatin1String(kGroup)), qs(all.dump()));
}

QVariantMap AiController::saveCustomProvider(const QVariantMap& spec) {
    const auto failed = [](const QString& error) {
        return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("id"), QString()}, {QStringLiteral("error"), error}};
    };
    const Json j = Json::parse(QJsonDocument::fromVariant(spec).toJson(QJsonDocument::Compact).toStdString(), nullptr, false);
    if (!j.is_object()) {
        return failed(tr("The provider description is not valid."));
    }
    std::string id = j.contains("id") && j["id"].is_string() ? j["id"].get<std::string>() : std::string();
    if (!id.empty()) {
        if (!d->customSpecs.contains(id)) {
            return failed(tr("That custom provider no longer exists."));
        }
    } else {
        // A new id from the name: "custom-" + lowercase letters and digits, unique among all providers.
        std::string slug;
        for (const char c : str(spec.value(QStringLiteral("name")).toString().trimmed().toLower())) {
            const auto u = static_cast<unsigned char>(c);
            if (std::isalnum(u) && u < 0x80) {
                slug.push_back(c);
            } else if (!slug.empty() && slug.back() != '-') {
                slug.push_back('-');
            }
            if (slug.size() >= 40) {
                break;
            }
        }
        while (!slug.empty() && slug.back() == '-') {
            slug.pop_back();
        }
        const std::string base = "custom-" + (slug.empty() ? std::string("provider") : slug);
        id = base;
        for (int n = 2; d->models.provider(id); ++n) {
            id = base + "-" + std::to_string(n);
        }
    }
    auto built = buildCustomProvider(j, id, *d->providers);
    if (!built) {
        return failed(built.error());
    }
    scopeModelIds(built->models, d->models, id); // a name another provider uses becomes "<id>:<name>" (apiId = name)
    Json normalised = built->spec;
    if (auto added = d->models.addCustomProvider(std::move(built->provider), std::move(built->models)); !added) {
        return failed(qs(added.error().message));
    }
    d->customSpecs[id] = std::move(normalised);
    for (auto it = d->defaults.begin(); it != d->defaults.end();) { // defaults on a model it no longer has: automatic
        const ModelSpec* m = d->models.model(it->second);
        it = !m ? d->defaults.erase(it) : std::next(it);
    }
    saveCustomProviders();
    applyRouting();
    saveSettings();
    emit providersChanged();
    emit modelsChanged();
    emit statusChanged();
    return QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("id"), qs(id)}, {QStringLiteral("error"), QString()}};
}

QVariantMap AiController::customProvider(const QString& id) const {
    const auto it = d->customSpecs.find(str(id));
    if (it == d->customSpecs.end()) {
        return {};
    }
    QVariantMap out = QJsonDocument::fromJson(QByteArray::fromStdString(it->second.dump())).toVariant().toMap();
    if (const ProviderSpec* p = d->models.provider(it->first)) {
        out[QStringLiteral("hasKey")] = hasKeyFor(*p) && p->keyRequired;
        out[QStringLiteral("local")] = p->local;
    }
    return out;
}

bool AiController::removeCustomProvider(const QString& id) {
    const ProviderSpec* provider = d->models.provider(str(id));
    if (!provider || !provider->custom || !d->customSpecs.contains(str(id))) {
        return false;
    }
    const ProviderSpec copy = *provider;
    (void)d->keys->remove(copy); // its key leaves the key store with it
    QSettings().remove(QStringLiteral("%1/keys/%2").arg(QLatin1String(kGroup), id));
    for (const Capability c : allCapabilities()) {
        d->consent.revoke(copy.id, c);
    }
    d->consent.revoke(copy.id, Capability::CloneVoice);
    d->spend.setMonthlyCap(copy.id, std::nullopt);
    d->models.removeCustomProvider(copy.id);
    d->customSpecs.erase(copy.id);
    for (auto it = d->defaults.begin(); it != d->defaults.end();) { // its models were someone's default: automatic
        it = !d->models.model(it->second) ? d->defaults.erase(it) : std::next(it);
    }
    saveCustomProviders();
    applyRouting();
    saveSettings();
    emit providersChanged();
    emit modelsChanged();
    emit statusChanged();
    emit privacyChanged();
    emit spendChanged();
    return true;
}

void AiController::listRemoteModels(const QString& providerId) {
    const ProviderSpec* provider = d->models.provider(str(providerId));
    if (!provider) {
        emit remoteModelsListed(providerId, false, {}, tr("Unknown provider."));
        return;
    }
    const std::string baseUrl = d->providers->baseUrl(*provider);
    if (needsAddress(baseUrl)) {
        emit remoteModelsListed(providerId, false, {}, tr("Set the %1 address first.").arg(qs(provider->name)));
        return;
    }
    auto key = d->keys->get(*provider);
    if (!key && provider->keyRequired) {
        emit remoteModelsListed(providerId, false, {}, tr("Add the %1 key first.").arg(qs(provider->name)));
        return;
    }
    startModelListing(providerId, *provider, baseUrl, key ? key->reveal() : std::string_view());
}

bool AiController::draftCustomProvider(const QVariantMap& spec, const QString& key, ProviderSpec& provider, std::string& secret,
                                       QString& error) const {
    const Json j = Json::parse(QJsonDocument::fromVariant(spec).toJson(QJsonDocument::Compact).toStdString(), nullptr, false);
    if (!j.is_object()) {
        error = tr("The provider description is not valid.");
        return false;
    }
    // Editing a saved provider: its id (and so its stored key); a new one gets a placeholder id it never keeps.
    const std::string savedId = j.contains("id") && j["id"].is_string() ? j["id"].get<std::string>() : std::string();
    const bool saved = !savedId.empty() && d->customSpecs.contains(savedId);
    auto built = buildCustomProvider(j, saved ? savedId : std::string("custom-draft"), *d->providers);
    if (!built) {
        error = built.error();
        return false;
    }
    provider = std::move(built->provider);
    secret = str(key.trimmed());
    if (secret.empty() && saved) {
        if (const ProviderSpec* existing = d->models.provider(savedId)) {
            if (auto stored = d->keys->get(*existing)) {
                secret = std::string(stored->reveal());
            }
        }
    }
    if (secret.empty() && provider.keyRequired) {
        error = tr("Paste the key first, or choose No key under Advanced.");
        return false;
    }
    return true;
}

QVariantMap AiController::checkCustomProvider(const QVariantMap& spec, const QString& key) {
    ProviderSpec provider;
    std::string secret;
    QString error;
    if (!draftCustomProvider(spec, key, provider, secret, error)) {
        return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("request"), QString()}, {QStringLiteral("error"), error}};
    }
    const QString request = QStringLiteral("draft-check-%1").arg(++d->draftSerial);
    IHttpTransport& t = transport();
    QPointer<AiController> self(this);
    const std::stop_token stop = d->stopJobs.get_token();
    // Nothing is registered or stored: the draft and its key live in this job only (the request log redacts the key).
    auto job = std::make_shared<std::pair<ProviderSpec, Secret>>(std::move(provider), Secret(std::move(secret)));
    d->pool.start([self, &t, job, request, stop] {
        bool ok = false;
        QString message;
        try {
            const ProviderSpec& p = job->first;
            const QString name = qs(p.name);
            const auto result = validateKey(t, p, p.baseUrl, job->second, stop);
            ok = result.has_value() || result.error().code == AiErrorCode::QuotaExceeded;
            if (result) {
                message = p.keyRequired ? tr("%1 answered: the key works.").arg(name) : tr("%1 answered.").arg(name);
            } else {
                switch (result.error().code) {
                case AiErrorCode::Unauthorized:
                    message = tr("%1 rejected the key.").arg(name);
                    break;
                case AiErrorCode::QuotaExceeded:
                    message = tr("The key works, but the account has no credit left.");
                    break;
                case AiErrorCode::RateLimited:
                    message = tr("%1 is busy. Try again in a moment.").arg(name);
                    break;
                case AiErrorCode::Offline:
                    message = tr("Offline mode is on: nothing was sent.");
                    break;
                case AiErrorCode::Network:
                    message = p.local ? tr("No server answered at %1.").arg(qs(p.baseUrl)) : tr("Could not reach %1.").arg(name);
                    break;
                default:
                    message = tr("The check failed: %1").arg(qs(Redactor::global().redact(result.error().message)));
                    break;
                }
            }
        } catch (...) { // QThreadPool does not catch (review wave 4, P2-6)
            message = tr("The check failed.");
        }
        QMetaObject::invokeMethod(self, [self, request, ok, message] {
            if (self) {
                emit self->customProviderChecked(request, ok, message);
            }
        }, Qt::QueuedConnection);
    });
    return QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("request"), request}, {QStringLiteral("error"), QString()}};
}

QVariantMap AiController::listCustomProviderModels(const QVariantMap& spec, const QString& key) {
    ProviderSpec provider;
    std::string secret;
    QString error;
    if (!draftCustomProvider(spec, key, provider, secret, error)) {
        return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("request"), QString()}, {QStringLiteral("error"), error}};
    }
    const QString request = QStringLiteral("draft-list-%1").arg(++d->draftSerial);
    startModelListing(request, provider, provider.baseUrl, secret);
    std::fill(secret.begin(), secret.end(), '\0');
    return QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("request"), request}, {QStringLiteral("error"), QString()}};
}

void AiController::startModelListing(const QString& providerId, const ProviderSpec& provider, const std::string& baseUrl,
                                     std::string_view key) {
    std::string path = "/models";
    if (provider.api == "anthropic-messages") {
        path = "/v1/models?limit=1000";
    } else if (provider.api == "gemini-interactions") {
        path = "/v1beta/models?pageSize=1000";
    }
    HttpRequest request;
    request.method = "GET";
    request.url = joinUrl(baseUrl, path);
    request.timeout = std::chrono::seconds(30);
    request.idleTimeout = std::chrono::seconds(20);
    applyAuth(request.headers, provider, key);
    for (const auto& h : provider.extraHeaders) {
        request.headers.push_back(h);
    }
    IHttpTransport& t = transport();
    QPointer<AiController> self(this);
    const std::stop_token stop = d->stopJobs.get_token();
    const QString name = qs(provider.name);
    const bool local = provider.local;
    // The request holds the key in a header: it lives in this job only and the transport's log redacts it.
    auto shared = std::make_shared<HttpRequest>(std::move(request));
    d->pool.start([self, &t, shared, providerId, name, local, stop] {
        bool ok = false;
        QString message;
        QVariantList models;
        try {
            const auto response = t.send(*shared, {}, stop);
            for (auto& h : shared->headers) { // the key is not needed any more
                std::fill(h.value.begin(), h.value.end(), '\0');
            }
            if (!response) {
                switch (response.error().code) {
                case AiErrorCode::Offline:
                    message = tr("Offline mode is on: nothing was sent.");
                    break;
                case AiErrorCode::Network:
                    message = local ? tr("No server answered.") : tr("Could not reach %1.").arg(name);
                    break;
                default:
                    message = tr("The list failed: %1").arg(qs(Redactor::global().redact(response.error().message)));
                    break;
                }
            } else if (!response->ok()) {
                const AiError error = classifyHttpError(response->status, response->headers, response->body);
                message = error.code == AiErrorCode::Unauthorized ? tr("%1 rejected the key.").arg(name)
                                                                  : tr("%1 answered with an error (%2).").arg(name).arg(response->status);
            } else {
                const auto j = Json::parse(response->body, nullptr, false);
                const Json* list = nullptr;
                if (j.is_object() && j.contains("data") && j["data"].is_array()) {
                    list = &j["data"];
                } else if (j.is_object() && j.contains("models") && j["models"].is_array()) {
                    list = &j["models"];
                } else if (j.is_array()) {
                    list = &j;
                }
                if (!list) {
                    message = tr("%1 did not answer with a model list.").arg(name);
                } else {
                    std::set<QString> seen;
                    for (const auto& entry : *list) {
                        QString id;
                        if (entry.is_string()) {
                            id = qs(entry.get<std::string>());
                        } else if (entry.is_object()) {
                            for (const char* key : {"id", "name"}) { // OpenAI-style "id"; Gemini "name"
                                if (const auto field = entry.find(key); id.isEmpty() && field != entry.end() && field->is_string()) {
                                    id = qs(field->get<std::string>());
                                }
                            }
                        }
                        if (id.startsWith(QLatin1String("models/"))) {
                            id = id.mid(7); // Gemini names
                        }
                        id = id.trimmed();
                        if (id.isEmpty() || id.size() > 200 || seen.contains(id) || models.size() >= 2000) {
                            continue;
                        }
                        const QString feature = guessFeature(id);
                        if (feature.isEmpty()) {
                            continue;
                        }
                        seen.insert(id);
                        models << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("capability"), feature}};
                    }
                    ok = true;
                    message = tr("%n model(s) found.", nullptr, static_cast<int>(models.size()));
                }
            }
        } catch (...) { // QThreadPool does not catch (review wave 4, P2-6)
            message = tr("The list failed.");
        }
        QMetaObject::invokeMethod(self, [self, providerId, ok, models, message] {
            if (self) {
                emit self->remoteModelsListed(providerId, ok, models, message);
            }
        }, Qt::QueuedConnection);
    });
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
        applyRouting(); // r07: a download changes what "automatic" can run on this computer
        emit localModelsChanged();
        emit modelsChanged();
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
    auto plan = llm::planLaunch(*model, llm::probeSystem());
    if (!plan) return std::unexpected(plan.error());
    ModelStore store(ModelStore::defaultRoot());
    auto weights = store.ensure(*model, nullptr);
    if (!weights) return std::unexpected(AiError{AiErrorCode::ModelMissing, "Download this model in AI preferences first"});
    if (auto v = store.verify(*model); !v) return std::unexpected(AiError{AiErrorCode::ModelMissing, v.error().message});
    // r07, library models: the other shards of a split GGUF sit beside the first; the vision projector goes to --mmproj.
    for (const std::string& part : model->parts) {
        const ModelInfo* p = findModel(part);
        if (!p || !store.isInstalled(*p))
            return std::unexpected(AiError{AiErrorCode::ModelMissing, "Download this model in AI preferences first"});
    }
    if (!model->projector.empty()) {
        if (const ModelInfo* p = findModel(model->projector); p && store.isInstalled(*p)) plan->projector = store.pathFor(*p);
    }
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
