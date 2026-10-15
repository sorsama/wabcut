#pragma once

// Providers (docs/ai.md, "Provider contract").
//
// ProviderRegistry maps wire formats ("anthropic-messages", "openai-responses", "chat-completions",
// "gemini-interactions", "deepgram", "elevenlabs-stt", "assemblyai", ...) to adapter factories, keeps the user's
// model preference per capability and resolves it with fallback: the first preferred model that exists, is not
// deprecated, has an adapter, has its key (if it needs one) and is allowed by the offline/no-cloud locks.
//
// AiPlatform is the front door for callers (AiController, wabcut-cli): it hands out capability objects whose calls
// enforce the locks and consent, reserve the pre-flight cost estimate against the monthly spend cap, settle the
// actual cost from the response's usage, and record each call in the redacted request log.

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Capability.h>
#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/Generation.h>
#include <wabcut/ai/Http.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/Transport.h>

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

/// What an adapter factory gets: the entries it serves, the endpoint, the key and the transport.
struct ProviderContext {
    const ProviderSpec& provider;
    const ModelSpec& model;
    std::string baseUrl; ///< the provider's, or the user's override for configurable providers
    Secret key;          ///< empty for providers without keys
    IHttpTransport& transport;
};

template <typename Interface> using AdapterFactory = std::function<std::unique_ptr<Interface>(ProviderContext)>;

class ProviderRegistry {
public:
    explicit ProviderRegistry(const ModelRegistry& models);
    ProviderRegistry(const ProviderRegistry&) = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;

    const ModelRegistry& models() const { return m_models; }

    void registerChat(std::string api, AdapterFactory<IChatModel> factory);
    void registerTranscriber(std::string api, AdapterFactory<ITranscriber> factory);
    void registerEmbedder(std::string api, AdapterFactory<IEmbedder> factory);
    void registerSpeechSynth(std::string api, AdapterFactory<ISpeechSynth> factory);
    void registerMediaGenerator(std::string api, AdapterFactory<IMediaGenerator> factory);
    bool hasAdapter(std::string_view api, Capability capability) const;

    /// The base URL used for a provider: the user's override (configurable providers only) or the registry's.
    std::string baseUrl(const ProviderSpec& provider) const;
    /// Overrides a configurable provider's base URL (empty restores the default). InvalidRequest for providers
    /// whose URL is fixed, or for URLs that are not http(s).
    std::expected<void, AiError> setBaseUrl(std::string_view providerId, std::string url);

    /// The user's ordered choice for a capability: model ids, first = preferred, the rest = fallbacks.
    void setPreference(Capability capability, std::vector<std::string> modelIds);
    std::vector<std::string> preference(Capability capability) const;

    struct Candidate {
        const ModelSpec* model = nullptr;
        const ProviderSpec* provider = nullptr;
        std::string baseUrl;
    };

    struct ResolveContext {
        std::chrono::year_month_day today{};
        const ConsentLedger* consent = nullptr;            ///< for the offline / no-cloud locks
        std::function<bool(const ProviderSpec&)> hasKey;   ///< whether a key is available
    };

    /// Usable candidates for `capability` in preference order. When none is usable the error is why the most
    /// preferred one is not (ModelMissing, Deprecated, Unsupported, NoCredentials, Offline); with no preference
    /// set, ModelMissing.
    std::expected<std::vector<Candidate>, AiError> resolve(Capability capability, const ResolveContext& context) const;

    /// The candidate for one model id (no preference involved). Errors: ModelMissing, Deprecated, Unsupported.
    std::expected<Candidate, AiError> candidate(std::string_view modelId, Capability capability,
                                                std::chrono::year_month_day today) const;

    std::expected<std::unique_ptr<IChatModel>, AiError> createChat(const Candidate& candidate, Secret key,
                                                                   IHttpTransport& transport) const;
    std::expected<std::unique_ptr<ITranscriber>, AiError> createTranscriber(const Candidate& candidate, Secret key,
                                                                            IHttpTransport& transport) const;
    std::expected<std::unique_ptr<IEmbedder>, AiError> createEmbedder(const Candidate& candidate, Secret key,
                                                                      IHttpTransport& transport) const;
    std::expected<std::unique_ptr<ISpeechSynth>, AiError> createSpeechSynth(const Candidate& candidate, Secret key,
                                                                            IHttpTransport& transport) const;
    std::expected<std::unique_ptr<IMediaGenerator>, AiError>
    createMediaGenerator(const Candidate& candidate, Secret key, IHttpTransport& transport) const;

private:
    template <typename Interface>
    std::expected<std::unique_ptr<Interface>, AiError>
    create(const std::map<std::string, AdapterFactory<Interface>, std::less<>>& factories, const Candidate& candidate,
           Secret key, IHttpTransport& transport) const;

    const ModelRegistry& m_models;
    mutable std::mutex m_mutex;
    std::map<std::string, AdapterFactory<IChatModel>, std::less<>> m_chat;
    std::map<std::string, AdapterFactory<ITranscriber>, std::less<>> m_transcribers;
    std::map<std::string, AdapterFactory<IEmbedder>, std::less<>> m_embedders;
    std::map<std::string, AdapterFactory<ISpeechSynth>, std::less<>> m_speech;
    std::map<std::string, AdapterFactory<IMediaGenerator>, std::less<>> m_generators;
    std::map<std::string, std::string, std::less<>> m_baseUrls;
    std::map<Capability, std::vector<std::string>> m_preferences;
};

/// Per-call options of AiPlatform.
struct CallOptions {
    bool allowOnce = false; ///< the user answered the consent prompt with "Allow once"
    /// A hard limit for this one call (generation): refused with SpendCapExceeded before anything is sent when the
    /// cost estimate is above it. The monthly cap applies as well.
    std::optional<MicroUsd> maxCost;
    /// Who asked: "" = the user, in the app's own features; "extension:<id>" = an extension. A remembered "Always
    /// allow" covers only the user's own features: an extension's call needs allowOnce, the user's answer to its own
    /// prompt (review wave 5, S5-P2-9). Hosts that give extensions AI services set it.
    std::string caller;
};

/// What the UI needs before a call: the resolved model and provider, consent state and cost estimate.
struct Preflight {
    ProviderRegistry::Candidate candidate;
    ConsentState consent = ConsentState::NeedsPrompt;
    CostEstimate cost;
    std::optional<MicroUsd> remainingBudget; ///< cap - spent - reserved this month, when capped
};

class AiPlatform {
public:
    AiPlatform(ProviderRegistry& providers, KeyRing& keys, ConsentLedger& consent, SpendGuard& spend,
               RequestLog& log, IHttpTransport& transport);

    ProviderRegistry& providers() { return m_providers; }
    KeyRing& keys() { return m_keys; }
    ConsentLedger& consent() { return m_consent; }
    SpendGuard& spend() { return m_spend; }
    RequestLog& log() { return m_log; }
    IHttpTransport& transport() { return m_transport; }

    /// Overrides "today" (tests); by default todayUtc().
    void setClock(std::function<std::chrono::year_month_day()> today);
    std::chrono::year_month_day today() const;

    std::expected<Preflight, AiError> preflightChat(std::string_view modelId, const ChatRequest& request);
    std::expected<Preflight, AiError> preflightTranscription(std::string_view modelId,
                                                             std::int64_t audioMilliseconds);
    /// The cost of a generation request (AI-10) and whether it may run: also checks the request against the model.
    std::expected<Preflight, AiError> preflightGeneration(std::string_view modelId, const MediaGenerationRequest& request);

    /// Capability objects whose calls are guarded (locks, consent, spend cap, request log). The checks run on
    /// every call, so a lock switched on later still applies. Creation errors: ModelMissing, Deprecated,
    /// Unsupported, NoCredentials.
    std::expected<std::unique_ptr<IChatModel>, AiError> chat(std::string_view modelId, CallOptions options = {});
    std::expected<std::unique_ptr<ITranscriber>, AiError> transcriber(std::string_view modelId,
                                                                      CallOptions options = {});
    /// Text-to-speech (AI-12): consent for Speak, the estimate (estimateSpeechCost) reserved against the monthly
    /// cap and refused above `options.maxCost` when one is set; settled at the reported usage, at the estimate when
    /// the provider failed after receiving the text.
    std::expected<std::unique_ptr<ISpeechSynth>, AiError> speech(std::string_view modelId, CallOptions options = {});

    /// Runs a whole generation job (create, poll, fetch into `directory`) as ONE guarded call (AI-10): the locks and
    /// consent are checked before every request (submit, each poll, the download), the estimate is reserved against
    /// the monthly cap (and refused above `options.maxCost`), and the job is settled once: at its price when it
    /// succeeded, at its estimate when it failed or was cancelled after the provider accepted it (unless the provider
    /// documents the failure as unbilled), released when it never reached the provider. Cancelling asks the provider
    /// to stop the job where its API can. Errors: everything AiErrorCode has; Unsupported / InvalidRequest from
    /// checkGenerationRequest(); SpendCapExceeded.
    std::expected<GeneratedMedia, AiError> generate(std::string_view modelId, const MediaGenerationRequest& request,
                                                    const std::filesystem::path& directory,
                                                    const GenerationEvents& events, std::stop_token stop,
                                                    CallOptions options = {});
    /// How generate() waits between polls (tests: a no-op) and how long it polls before giving up.
    void setPolling(Sleeper sleeper, std::chrono::milliseconds interval = std::chrono::seconds(5),
                    std::chrono::milliseconds maxTime = std::chrono::minutes(30));

    /// The resolved, usable chat/transcription model ids for the user's preference (first = what runs).
    std::expected<std::vector<ProviderRegistry::Candidate>, AiError> resolve(Capability capability);

private:
    std::expected<Secret, AiError> keyFor(const ProviderSpec& provider);

    ProviderRegistry& m_providers;
    KeyRing& m_keys;
    ConsentLedger& m_consent;
    SpendGuard& m_spend;
    RequestLog& m_log;
    IHttpTransport& m_transport;
    std::function<std::chrono::year_month_day()> m_today;
    Sleeper m_sleeper = interruptibleSleep;
    std::chrono::milliseconds m_pollInterval{std::chrono::seconds(5)};
    std::chrono::milliseconds m_maxPollTime{std::chrono::minutes(30)};
};

} // namespace wabcut::ai
