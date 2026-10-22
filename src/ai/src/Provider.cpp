#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Redact.h>

#include <atomic>
#include <chrono>
#include <mutex>

namespace wabcut::ai {

// --- ProviderRegistry -------------------------------------------------------------------------------------------

ProviderRegistry::ProviderRegistry(const ModelRegistry& models) : m_models(models) {}

void ProviderRegistry::registerChat(std::string api, AdapterFactory<IChatModel> factory) {
    std::lock_guard lock(m_mutex);
    m_chat.insert_or_assign(std::move(api), std::move(factory));
}

void ProviderRegistry::registerTranscriber(std::string api, AdapterFactory<ITranscriber> factory) {
    std::lock_guard lock(m_mutex);
    m_transcribers.insert_or_assign(std::move(api), std::move(factory));
}

void ProviderRegistry::registerEmbedder(std::string api, AdapterFactory<IEmbedder> factory) {
    std::lock_guard lock(m_mutex);
    m_embedders.insert_or_assign(std::move(api), std::move(factory));
}

void ProviderRegistry::registerSpeechSynth(std::string api, AdapterFactory<ISpeechSynth> factory) {
    std::lock_guard lock(m_mutex);
    m_speech.insert_or_assign(std::move(api), std::move(factory));
}

void ProviderRegistry::registerMediaGenerator(std::string api, AdapterFactory<IMediaGenerator> factory) {
    std::lock_guard lock(m_mutex);
    m_generators.insert_or_assign(std::move(api), std::move(factory));
}

bool ProviderRegistry::hasAdapter(std::string_view api, Capability capability) const {
    std::lock_guard lock(m_mutex);
    switch (capability) {
    case Capability::Chat:
        return m_chat.contains(api);
    case Capability::Transcribe:
        return m_transcribers.contains(api);
    case Capability::Embed:
        return m_embedders.contains(api);
    case Capability::Speak:
        return m_speech.contains(api);
    case Capability::GenerateMedia:
        return m_generators.contains(api);
    case Capability::Translate:
        return m_chat.contains(api); // translation runs on a chat model (LlmTranslator)
    case Capability::CloneVoice:
        return m_speech.contains(api); // cloning is part of the speech adapter
    }
    return false;
}

std::string ProviderRegistry::baseUrl(const ProviderSpec& provider) const {
    std::lock_guard lock(m_mutex);
    if (auto it = m_baseUrls.find(provider.id); it != m_baseUrls.end()) {
        return it->second;
    }
    return provider.baseUrl;
}

std::expected<void, AiError> ProviderRegistry::setBaseUrl(std::string_view providerId, std::string url) {
    const ProviderSpec* provider = m_models.provider(providerId);
    if (!provider || !provider->configurableBaseUrl) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "the base URL of '" + std::string(providerId) +
                                                                        "' cannot be changed"));
    }
    std::lock_guard lock(m_mutex);
    if (url.empty()) {
        m_baseUrls.erase(std::string(providerId));
        return {};
    }
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "base URLs must start with http:// or https://"));
    }
    while (url.size() > 8 && url.back() == '/') {
        url.pop_back();
    }
    m_baseUrls.insert_or_assign(std::string(providerId), std::move(url));
    return {};
}

void ProviderRegistry::setPreference(Capability capability, std::vector<std::string> modelIds) {
    std::lock_guard lock(m_mutex);
    m_preferences[capability] = std::move(modelIds);
}

std::vector<std::string> ProviderRegistry::preference(Capability capability) const {
    std::lock_guard lock(m_mutex);
    auto it = m_preferences.find(capability);
    return it == m_preferences.end() ? std::vector<std::string>() : it->second;
}

std::expected<ProviderRegistry::Candidate, AiError>
ProviderRegistry::candidate(std::string_view modelId, Capability capability, std::chrono::year_month_day today) const {
    auto model = m_models.usableModel(modelId, today);
    if (!model) {
        return std::unexpected(model.error());
    }
    const ModelSpec& m = **model;
    if (!m.caps.has(capability) && !(capability == Capability::Translate && m.caps.has(Capability::Chat))) {
        return std::unexpected(AiError(AiErrorCode::Unsupported, "'" + m.id + "' cannot " + std::string(toString(capability))));
    }
    if (!hasAdapter(m.api, capability)) {
        return std::unexpected(AiError(AiErrorCode::Unsupported, "no " + std::string(toString(capability)) +
                                                                     " adapter for the '" + m.api + "' API"));
    }
    const ProviderSpec* provider = m_models.provider(m.provider);
    if (!provider) {
        return std::unexpected(AiError(AiErrorCode::ModelMissing, "unknown provider '" + m.provider + "'"));
    }
    return Candidate{&m, provider, baseUrl(*provider)};
}

std::expected<std::vector<ProviderRegistry::Candidate>, AiError>
ProviderRegistry::resolve(Capability capability, const ResolveContext& context) const {
    std::vector<Candidate> usable;
    std::optional<AiError> firstError;
    for (const auto& id : preference(capability)) {
        auto c = candidate(id, capability, context.today);
        if (c && context.consent && isCloud(*c->provider, c->baseUrl) && !context.consent->cloudAllowed()) {
            c = std::unexpected(AiError(AiErrorCode::Offline, "cloud AI is off"));
        }
        if (c && c->provider->keyRequired && context.hasKey && !context.hasKey(*c->provider)) {
            c = std::unexpected(AiError(AiErrorCode::NoCredentials, "no key for " + c->provider->name));
        }
        if (c) {
            usable.push_back(*c);
        } else if (!firstError) {
            firstError = c.error();
        }
    }
    if (usable.empty()) {
        return std::unexpected(firstError.value_or(
            AiError(AiErrorCode::ModelMissing, "no model chosen for " + std::string(toString(capability)))));
    }
    return usable;
}

template <typename Interface>
std::expected<std::unique_ptr<Interface>, AiError>
ProviderRegistry::create(const std::map<std::string, AdapterFactory<Interface>, std::less<>>& factories,
                         const Candidate& candidate, Secret key, IHttpTransport& transport) const {
    if (!candidate.model || !candidate.provider) {
        return std::unexpected(AiError(AiErrorCode::ModelMissing, "no model"));
    }
    AdapterFactory<Interface> factory;
    {
        std::lock_guard lock(m_mutex);
        auto it = factories.find(candidate.model->api);
        if (it == factories.end()) {
            return std::unexpected(AiError(AiErrorCode::Unsupported, "no adapter for the '" + candidate.model->api + "' API"));
        }
        factory = it->second;
    }
    auto adapter = factory(ProviderContext{*candidate.provider, *candidate.model, candidate.baseUrl, std::move(key), transport});
    if (!adapter) {
        return std::unexpected(AiError(AiErrorCode::Unsupported, "the adapter refused '" + candidate.model->id + "'"));
    }
    return adapter;
}

std::expected<std::unique_ptr<IChatModel>, AiError> ProviderRegistry::createChat(const Candidate& candidate, Secret key,
                                                                                 IHttpTransport& transport) const {
    return create(m_chat, candidate, std::move(key), transport);
}

std::expected<std::unique_ptr<ITranscriber>, AiError>
ProviderRegistry::createTranscriber(const Candidate& candidate, Secret key, IHttpTransport& transport) const {
    return create(m_transcribers, candidate, std::move(key), transport);
}

std::expected<std::unique_ptr<IEmbedder>, AiError>
ProviderRegistry::createEmbedder(const Candidate& candidate, Secret key, IHttpTransport& transport) const {
    return create(m_embedders, candidate, std::move(key), transport);
}

std::expected<std::unique_ptr<ISpeechSynth>, AiError>
ProviderRegistry::createSpeechSynth(const Candidate& candidate, Secret key, IHttpTransport& transport) const {
    return create(m_speech, candidate, std::move(key), transport);
}

std::expected<std::unique_ptr<IMediaGenerator>, AiError>
ProviderRegistry::createMediaGenerator(const Candidate& candidate, Secret key, IHttpTransport& transport) const {
    return create(m_generators, candidate, std::move(key), transport);
}

// --- Guarded capability objects ---------------------------------------------------------------------------------

namespace {

std::int64_t elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

RequestLogEntry callEntry(const ProviderSpec& provider, const ModelSpec& model, Capability capability) {
    RequestLogEntry e;
    e.kind = RequestLogEntry::Kind::Call;
    e.time = nowIso8601();
    e.provider = provider.id;
    e.model = model.id;
    e.capability = std::string(toString(capability));
    return e;
}

/// What reached the provider during one guarded call. Settlement depends on it: a call that never left this
/// machine, or that the provider refused (an HTTP error status before any output), releases its reservation; a call
/// the provider answered or may have processed is settled with its usage, or a conservative estimate.
struct DispatchState {
    std::atomic<bool> attempted{false};  ///< the adapter sent a request
    std::atomic<bool> bodySent{false};   ///< request bytes left this machine (upload progress)
    std::atomic<bool> answered{false};   ///< a 2xx response head arrived: the provider is working on it
    std::atomic<bool> refused{false};    ///< the last response was an error status (nothing billed)
    /// Set by guardedCall for the call's duration: may this call still send (consent, lock, same project)?
    std::function<std::expected<void, AiError>()> authorize;

    void reset() {
        attempted = false;
        bodySent = false;
        answered = false;
        refused = false;
    }
};

/// The adapter's transport for a guarded call object: forwards everything and records the dispatch state.
class TrackingTransport final : public IHttpTransport {
public:
    TrackingTransport(IHttpTransport& inner, DispatchState& state) : m_inner(inner), m_state(state) {}

    std::expected<HttpResponse, AiError> send(const HttpRequest& request, const HttpStreamHandlers& handlers,
                                              std::stop_token stop) override {
        // Every dispatch is authorized again (review wave 3, P1-5): a multi-request operation (Gemini's upload init,
        // upload and polls, a retry) stops when the lock comes on or another project is opened, before the next
        // request leaves. Retries below this transport check the same through HttpRequest::beforeDispatch.
        HttpRequest guarded = request;
        if (m_state.authorize) {
            if (auto allowed = m_state.authorize(); !allowed) {
                return std::unexpected(allowed.error()); // nothing was sent
            }
            guarded.beforeDispatch.check = m_state.authorize;
        }
        m_state.attempted = true;
        HttpStreamHandlers tracked = handlers;
        tracked.onHead = [&](const HttpResponseHead& head) {
            m_state.answered = true;
            return handlers.onHead ? handlers.onHead(head) : true;
        };
        tracked.onUploadProgress = [&](std::uint64_t sent, std::uint64_t total) {
            if (sent > 0) {
                m_state.bodySent = true;
            }
            if (handlers.onUploadProgress) {
                handlers.onUploadProgress(sent, total);
            }
        };
        auto response = m_inner.send(guarded, tracked, stop);
        if (response) {
            m_state.bodySent = true; // a response means the request arrived
            if (response->ok()) {
                m_state.answered = true;
                m_state.refused = false;
            } else {
                m_state.refused = !m_state.answered.load();
            }
        }
        return response;
    }

private:
    IHttpTransport& m_inner;
    DispatchState& m_state;
};

/// What a chat call streamed before it ended: the last cumulative usage, and how much output arrived.
struct StreamedUsage {
    std::mutex mutex;
    std::optional<Usage> usage;
    std::size_t outputBytes = 0;

    void reset() {
        std::lock_guard lock(mutex);
        usage.reset();
        outputBytes = 0;
    }
};

/// RAII settlement of a reservation: whatever happens in the call (an error, a cancel, an exception), the reservation
/// is settled or released exactly once, never left reserved.
class Settlement {
public:
    Settlement(SpendGuard& spend, SpendGuard::Reservation reservation, std::function<MicroUsd()> unknownOutcome)
        : m_spend(spend), m_reservation(std::move(reservation)), m_unknownOutcome(std::move(unknownOutcome)) {}
    Settlement(const Settlement&) = delete;
    Settlement& operator=(const Settlement&) = delete;
    ~Settlement() {
        if (!m_done) { // an exception escaped the call: settle what may have been billed
            MicroUsd cost = 0;
            try {
                cost = m_unknownOutcome();
            } catch (...) {
                cost = m_reservation.amount; // keep the whole reservation rather than lose track of spend
            }
            m_spend.settle(m_reservation, cost);
        }
    }
    void settle(MicroUsd cost) {
        if (!m_done) {
            m_spend.settle(m_reservation, cost);
            m_done = true;
        }
    }
    void release() {
        if (!m_done) {
            m_spend.release(m_reservation);
            m_done = true;
        }
    }

private:
    SpendGuard& m_spend;
    SpendGuard::Reservation m_reservation;
    std::function<MicroUsd()> m_unknownOutcome;
    bool m_done = false;
};

/// How a failed (or interrupted) call is accounted for.
struct FailureCost {
    /// The cost of usage the provider reported before the failure; nullopt when it reported none.
    std::function<std::optional<MicroUsd>()> reported;
    /// A conservative estimate for a call the provider answered (it produced output) without reporting usage.
    std::function<MicroUsd()> answered;
    /// A conservative estimate for a request that reached the provider but got no answer (it may have processed it).
    std::function<MicroUsd()> unanswered;
};

/// The cost to settle for a call that did not succeed, or nullopt to release the reservation: only a call proven
/// not to have been processed is free (never sent, or refused with an error status before any output).
std::optional<MicroUsd> failedCallCost(const DispatchState& state, const FailureCost& cost) {
    if (const auto reported = cost.reported()) {
        return *reported;
    }
    if (state.answered) {
        return cost.answered();
    }
    if (state.bodySent && !state.refused) {
        return cost.unanswered();
    }
    return std::nullopt; // not sent, or refused before any output
}

/// Shared pre/post steps of a guarded call: authorize, reserve, run, settle, log.
template <typename Result, typename Run>
std::expected<Result, AiError> guardedCall(AiPlatform& platform, const ProviderSpec& provider,
                                           const std::string& baseUrl, const ModelSpec& model, Capability capability,
                                           const CallOptions& options, MicroUsd ceiling, DispatchState& dispatch,
                                           const FailureCost& failureCost, Run run, Usage (*usageOf)(const Result&)) {
    RequestLogEntry entry = callEntry(provider, model, capability);
    auto fail = [&](AiError error) -> std::expected<Result, AiError> {
        entry.error = std::string(toString(error.code));
        entry.detail = Redactor::global().redact(error.message);
        platform.log().add(entry);
        return std::unexpected(std::move(error));
    };
    if (auto allowed = platform.consent().authorize(provider, baseUrl, capability, options.allowOnce, options.caller); !allowed) {
        return fail(allowed.error());
    }
    const auto today = platform.today();
    auto reservation = platform.spend().reserve(provider.id, ceiling, today);
    if (!reservation) {
        return fail(reservation.error());
    }
    dispatch.reset();
    // Bound to the project the call started on: every dispatch checks consent, the lock and the project again.
    ConsentLedger& consent = platform.consent();
    const std::uint64_t project = consent.projectIdentity();
    dispatch.authorize = [&consent, &provider, baseUrl, capability, allowOnce = options.allowOnce, caller = options.caller,
                          project]() -> std::expected<void, AiError> {
        if (consent.projectIdentity() != project) {
            return std::unexpected(AiError(AiErrorCode::Cancelled,
                                           "another project was opened during the request: nothing more was sent"));
        }
        return consent.authorize(provider, baseUrl, capability, allowOnce, caller);
    };
    struct ClearAuthorize {
        DispatchState& state;
        ~ClearAuthorize() { state.authorize = nullptr; }
    } clearAuthorize{dispatch};
    Settlement settlement(platform.spend(), *reservation, [&] {
        return failedCallCost(dispatch, failureCost).value_or(0);
    });
    const auto start = std::chrono::steady_clock::now();
    auto result = run();
    entry.durationMs = elapsedMs(start);
    if (!result) {
        if (const auto cost = failedCallCost(dispatch, failureCost)) {
            entry.cost = *cost;
            settlement.settle(*cost); // the provider may bill partial work: it stays in the month's spend
        } else {
            settlement.release(); // proven not processed
        }
        return fail(result.error());
    }
    entry.usage = usageOf(*result);
    entry.cost = costOf(model, entry.usage, today);
    settlement.settle(entry.cost);
    platform.log().add(entry);
    return result;
}

Usage chatUsage(const ChatResponse& r) { return r.usage; }
Usage transcriptionUsage(const TranscribeResult& r) { return r.usage; }
Usage speechUsage(const SpeechResult& r) { return r.usage; }
Usage noUsage(const ClonedVoice&) { return {}; }
Usage generatedUsage(const GeneratedMedia& r) { return r.usage; }

AiError generationFailure(const GenerationStatus& status) {
    std::string message = status.failure.empty() ? std::string("the provider could not generate the media")
                                                 : Redactor::global().redact(status.failure);
    AiErrorCode code = AiErrorCode::ProviderError;
    if (status.failureCode == "content_policy" || status.failureCode == "safety" || status.failureCode == "invalid") {
        code = AiErrorCode::InvalidRequest; // the prompt or image was refused: retrying the same request will not help
    } else if (status.failureCode == "quota") {
        code = AiErrorCode::QuotaExceeded;
    }
    AiError error(code, std::move(message));
    error.providerCode = status.failureCode;
    return error;
}

class GuardedChat final : public IChatModel {
public:
    GuardedChat(std::unique_ptr<DispatchState> dispatch, std::unique_ptr<TrackingTransport> transport,
                std::unique_ptr<IChatModel> inner, ProviderSpec provider, std::string baseUrl, AiPlatform& platform,
                CallOptions options)
        : m_dispatch(std::move(dispatch)), m_transport(std::move(transport)), m_inner(std::move(inner)),
          m_provider(std::move(provider)), m_baseUrl(std::move(baseUrl)), m_platform(platform), m_options(options) {}

    const ModelSpec& spec() const override { return m_inner->spec(); }

    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                              std::stop_token stop) override {
        const auto today = m_platform.today();
        const CostEstimate cost = estimateChatCost(spec(), request, today);
        m_streamed.reset();
        // Usage and output are retained as they stream, so a call that fails after them is still accounted for.
        const ChatEventHandler tracked = [&](const ChatEvent& event) {
            {
                std::lock_guard lock(m_streamed.mutex);
                if (const auto* u = std::get_if<UsageUpdate>(&event)) {
                    m_streamed.usage = u->usage;
                } else if (const auto* t = std::get_if<TextDelta>(&event)) {
                    m_streamed.outputBytes += t->text.size();
                } else if (const auto* r = std::get_if<ReasoningDelta>(&event)) {
                    m_streamed.outputBytes += r->text.size();
                } else if (const auto* a = std::get_if<ToolCallArgumentsDelta>(&event)) {
                    m_streamed.outputBytes += a->text.size();
                }
            }
            if (onEvent) {
                onEvent(event);
            }
        };
        FailureCost failure;
        failure.reported = [&]() -> std::optional<MicroUsd> {
            std::lock_guard lock(m_streamed.mutex);
            if (!m_streamed.usage) {
                return std::nullopt;
            }
            // Output streamed after the last usage report counts too (about 4 bytes per token, rounded up).
            Usage u = *m_streamed.usage;
            u.outputTokens = std::max<std::int64_t>(u.outputTokens, static_cast<std::int64_t>((m_streamed.outputBytes + 3) / 4));
            return costOf(spec(), u, today);
        };
        failure.answered = [&] {
            std::lock_guard lock(m_streamed.mutex);
            Usage u;
            u.inputTokens = cost.inputTokens;
            u.outputTokens = static_cast<std::int64_t>((m_streamed.outputBytes + 3) / 4);
            return costOf(spec(), u, today);
        };
        failure.unanswered = [&] {
            Usage u;
            u.inputTokens = cost.inputTokens; // the prompt may have been processed; no output arrived
            return costOf(spec(), u, today);
        };
        return guardedCall<ChatResponse>(
            m_platform, m_provider, m_baseUrl, spec(), Capability::Chat, m_options, cost.ceiling, *m_dispatch, failure,
            [&] { return m_inner->chat(request, tracked, stop); }, &chatUsage);
    }

private:
    // Declared before the adapter: the adapter holds a reference to the transport, which must outlive it.
    std::unique_ptr<DispatchState> m_dispatch;
    std::unique_ptr<TrackingTransport> m_transport;
    std::unique_ptr<IChatModel> m_inner;
    ProviderSpec m_provider;
    std::string m_baseUrl;
    AiPlatform& m_platform;
    CallOptions m_options;
    StreamedUsage m_streamed;
};

class GuardedTranscriber final : public ITranscriber {
public:
    GuardedTranscriber(std::unique_ptr<DispatchState> dispatch, std::unique_ptr<TrackingTransport> transport,
                       std::unique_ptr<ITranscriber> inner, ProviderSpec provider, std::string baseUrl,
                       AiPlatform& platform, CallOptions options)
        : m_dispatch(std::move(dispatch)), m_transport(std::move(transport)), m_inner(std::move(inner)),
          m_provider(std::move(provider)), m_baseUrl(std::move(baseUrl)), m_platform(platform), m_options(options) {}

    const ModelSpec& spec() const override { return m_inner->spec(); }

    std::expected<TranscribeResult, AiError> transcribe(const TranscribeRequest& request,
                                                           const ProgressFn& progress, std::stop_token stop) override {
        if (request.audioMilliseconds <= 0 && m_platform.spend().monthlyCap(m_provider.id)) {
            // A cap cannot be enforced without the duration.
            return std::unexpected(
                AiError(AiErrorCode::InvalidRequest, "the audio duration is needed to respect the spend cap"));
        }
        const CostEstimate cost = estimateTranscriptionCost(spec(), request.audioMilliseconds, m_platform.today());
        // Transcription bills the audio once it was uploaded: a failure after that settles the expected cost.
        FailureCost failure;
        failure.reported = [] { return std::optional<MicroUsd>(); };
        failure.answered = [&] { return cost.expected; };
        failure.unanswered = [&] { return cost.expected; };
        return guardedCall<TranscribeResult>(
            m_platform, m_provider, m_baseUrl, spec(), Capability::Transcribe, m_options, cost.ceiling, *m_dispatch,
            failure, [&] { return m_inner->transcribe(request, progress, stop); }, &transcriptionUsage);
    }

private:
    std::unique_ptr<DispatchState> m_dispatch;
    std::unique_ptr<TrackingTransport> m_transport;
    std::unique_ptr<ITranscriber> m_inner;
    ProviderSpec m_provider;
    std::string m_baseUrl;
    AiPlatform& m_platform;
    CallOptions m_options;
};

class GuardedSpeech final : public ISpeechSynth {
public:
    GuardedSpeech(std::unique_ptr<DispatchState> dispatch, std::unique_ptr<TrackingTransport> transport,
                  std::unique_ptr<ISpeechSynth> inner, ProviderSpec provider, std::string baseUrl, AiPlatform& platform,
                  CallOptions options)
        : m_dispatch(std::move(dispatch)), m_transport(std::move(transport)), m_inner(std::move(inner)),
          m_provider(std::move(provider)), m_baseUrl(std::move(baseUrl)), m_platform(platform), m_options(options) {}

    const ModelSpec& spec() const override { return m_inner->spec(); }

    std::expected<SpeechResult, AiError> synthesize(const SpeechRequest& request, std::stop_token stop) override {
        const auto characters = std::count_if(request.text.begin(), request.text.end(),
                                              [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
        const CostEstimate cost = estimateSpeechCost(spec(), static_cast<std::int64_t>(characters), m_platform.today());
        if (!cost.priced && m_platform.spend().monthlyCap(m_provider.id)) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "the voice model has no price, so the spend cap cannot be kept"));
        }
        if (m_options.maxCost && cost.ceiling > *m_options.maxCost) {
            return std::unexpected(AiError(AiErrorCode::SpendCapExceeded, "this line could cost more than the cap of the run"));
        }
        // The text is billed once the provider has it: a failure after that settles the estimate.
        FailureCost failure;
        failure.reported = [] { return std::optional<MicroUsd>(); };
        failure.answered = [&] { return cost.expected; };
        failure.unanswered = [&] { return cost.expected; };
        return guardedCall<SpeechResult>(m_platform, m_provider, m_baseUrl, spec(), Capability::Speak, m_options, cost.ceiling,
                                         *m_dispatch, failure, [&] { return m_inner->synthesize(request, stop); }, &speechUsage);
    }

    // Cloning uploads a recording of a person: a consent of its own (CloneVoice), which a remembered "Always allow" for
    // speech does not cover (review wave 5, S5-P2-9; the caller has the rights acknowledgement too). Not billed per call.
    std::expected<ClonedVoice, AiError> cloneVoice(std::span<const std::filesystem::path> samples, std::string_view name,
                                                   std::stop_token stop) override {
        return guardedCall<ClonedVoice>(m_platform, m_provider, m_baseUrl, spec(), Capability::CloneVoice, m_options, 0, *m_dispatch, free(),
                                        [&] { return m_inner->cloneVoice(samples, name, stop); }, &noUsage);
    }

    // Deleting a cloned voice only removes the speaker's data from the provider (review wave 5, A5-P2-14): neither the
    // consent ledger, the no-cloud lock nor another project being open refuses it. Only the offline mode keeps it on
    // this computer (nothing at all is sent then); the caller tells the user to delete the voice by hand. Logged.
    std::expected<void, AiError> deleteVoice(std::string_view voiceId, std::stop_token stop) override {
        RequestLogEntry entry = callEntry(m_provider, spec(), Capability::Speak);
        std::expected<void, AiError> deleted;
        if (m_platform.consent().offlineMode()) {
            deleted = std::unexpected(AiError(AiErrorCode::Offline, "offline mode is on: nothing was sent"));
        } else {
            m_dispatch->reset(); // no authorize hook outside guardedCall: every request of the deletion goes
            const auto start = std::chrono::steady_clock::now();
            deleted = m_inner->deleteVoice(voiceId, stop);
            entry.durationMs = elapsedMs(start);
        }
        if (!deleted) {
            entry.error = std::string(toString(deleted.error().code));
            entry.detail = Redactor::global().redact(deleted.error().message);
        }
        m_platform.log().add(entry);
        return deleted;
    }

private:
    static FailureCost free() {
        FailureCost cost;
        cost.reported = [] { return std::optional<MicroUsd>(); };
        cost.answered = [] { return MicroUsd{0}; };
        cost.unanswered = [] { return MicroUsd{0}; };
        return cost;
    }

    std::unique_ptr<DispatchState> m_dispatch;
    std::unique_ptr<TrackingTransport> m_transport;
    std::unique_ptr<ISpeechSynth> m_inner;
    ProviderSpec m_provider;
    std::string m_baseUrl;
    AiPlatform& m_platform;
    CallOptions m_options;
};

} // namespace

// --- AiPlatform -------------------------------------------------------------------------------------------------

AiPlatform::AiPlatform(ProviderRegistry& providers, KeyRing& keys, ConsentLedger& consent, SpendGuard& spend,
                       RequestLog& log, IHttpTransport& transport)
    : m_providers(providers), m_keys(keys), m_consent(consent), m_spend(spend), m_log(log), m_transport(transport),
      m_today(todayUtc) {}

void AiPlatform::setClock(std::function<std::chrono::year_month_day()> today) {
    m_today = today ? std::move(today) : std::function<std::chrono::year_month_day()>(todayUtc);
}

std::chrono::year_month_day AiPlatform::today() const { return m_today(); }

std::expected<Secret, AiError> AiPlatform::keyFor(const ProviderSpec& provider) {
    if (!provider.keyRequired) {
        auto key = m_keys.get(provider); // optional key, e.g. a llama-server started with --api-key
        return key ? std::move(*key) : Secret();
    }
    return m_keys.get(provider);
}

std::expected<Preflight, AiError> AiPlatform::preflightChat(std::string_view modelId, const ChatRequest& request) {
    auto candidate = m_providers.candidate(modelId, Capability::Chat, today());
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    Preflight p;
    p.candidate = *candidate;
    p.consent = m_consent.state(*candidate->provider, candidate->baseUrl, Capability::Chat);
    p.cost = estimateChatCost(*candidate->model, request, today());
    if (auto cap = m_spend.monthlyCap(candidate->provider->id)) {
        const auto t = today();
        p.remainingBudget = *cap - m_spend.spent(candidate->provider->id, {t.year(), t.month()}) -
                            m_spend.reserved(candidate->provider->id);
    }
    return p;
}

std::expected<Preflight, AiError> AiPlatform::preflightTranscription(std::string_view modelId,
                                                                     std::int64_t audioMilliseconds) {
    auto candidate = m_providers.candidate(modelId, Capability::Transcribe, today());
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    Preflight p;
    p.candidate = *candidate;
    p.consent = m_consent.state(*candidate->provider, candidate->baseUrl, Capability::Transcribe);
    p.cost = estimateTranscriptionCost(*candidate->model, audioMilliseconds, today());
    if (auto cap = m_spend.monthlyCap(candidate->provider->id)) {
        const auto t = today();
        p.remainingBudget = *cap - m_spend.spent(candidate->provider->id, {t.year(), t.month()}) -
                            m_spend.reserved(candidate->provider->id);
    }
    return p;
}

std::expected<Preflight, AiError> AiPlatform::preflightGeneration(std::string_view modelId,
                                                                  const MediaGenerationRequest& request) {
    auto candidate = m_providers.candidate(modelId, Capability::GenerateMedia, today());
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    if (auto valid = checkGenerationRequest(*candidate->model, request); !valid) {
        return std::unexpected(valid.error());
    }
    Preflight p;
    p.candidate = *candidate;
    p.consent = m_consent.state(*candidate->provider, candidate->baseUrl, Capability::GenerateMedia);
    p.cost = estimateGenerationCost(*candidate->model, request, today());
    if (auto cap = m_spend.monthlyCap(candidate->provider->id)) {
        const auto t = today();
        p.remainingBudget = *cap - m_spend.spent(candidate->provider->id, {t.year(), t.month()}) -
                            m_spend.reserved(candidate->provider->id);
    }
    return p;
}

void AiPlatform::setPolling(Sleeper sleeper, std::chrono::milliseconds interval, std::chrono::milliseconds maxTime) {
    m_sleeper = sleeper ? std::move(sleeper) : Sleeper(interruptibleSleep);
    m_pollInterval = interval;
    m_maxPollTime = maxTime;
}

std::expected<GeneratedMedia, AiError> AiPlatform::generate(std::string_view modelId,
                                                            const MediaGenerationRequest& request,
                                                            const std::filesystem::path& directory,
                                                            const GenerationEvents& events, std::stop_token stop,
                                                            CallOptions options) {
    auto candidate = m_providers.candidate(modelId, Capability::GenerateMedia, today());
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    const ModelSpec& model = *candidate->model;
    const ProviderSpec& provider = *candidate->provider;
    if (auto valid = checkGenerationRequest(model, request); !valid) {
        return std::unexpected(valid.error());
    }
    const CostEstimate cost = estimateGenerationCost(model, request, today());
    if (!cost.priced) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest,
                                       "the registry has no price for " + model.id +
                                           " at this size, so its spend cap cannot be enforced"));
    }
    if (!options.maxCost || *options.maxCost < 0) {
        return std::unexpected(AiError(AiErrorCode::SpendCapExceeded, "approve a hard limit for this generation first"));
    }
    if (cost.ceiling > *options.maxCost) {
        return std::unexpected(AiError(AiErrorCode::SpendCapExceeded, "the estimate " + formatUsd(cost.ceiling) +
                                                                          " is above this run's limit of " +
                                                                          formatUsd(*options.maxCost)));
    }
    auto key = keyFor(provider);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto dispatch = std::make_unique<DispatchState>();
    auto transport = std::make_unique<TrackingTransport>(m_transport, *dispatch);
    auto inner = m_providers.createMediaGenerator(*candidate, std::move(*key), *transport);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    IMediaGenerator& generator = **inner;

    // A job the provider accepted may be billed whatever happens next, unless it reports the failure as unbilled.
    bool unbilled = false;
    FailureCost failure;
    failure.reported = [] { return std::optional<MicroUsd>(); };
    failure.answered = [&] { return unbilled ? MicroUsd{0} : cost.expected; };
    failure.unanswered = [&] { return cost.expected; };

    const auto cancelJob = [&](const GenerationTicket& ticket) {
        if (model.generation.cancel) {
            (void)generator.cancel(ticket, std::stop_token{}); // best effort, outside the cancelled token
        }
    };
    const auto run = [&]() -> std::expected<GeneratedMedia, AiError> {
        auto ticket = generator.create(request, stop);
        if (!ticket) {
            return std::unexpected(ticket.error());
        }
        if (events.onSubmitted) {
            events.onSubmitted(*ticket);
        }
        const auto deadline = std::chrono::steady_clock::now() + m_maxPollTime;
        GenerationStatus status;
        for (;;) {
            if (stop.stop_requested()) {
                cancelJob(*ticket);
                return std::unexpected(AiError(AiErrorCode::Cancelled, "generation cancelled"));
            }
            auto polled = generator.poll(*ticket, stop);
            if (!polled) {
                if (polled.error().code == AiErrorCode::Cancelled) {
                    cancelJob(*ticket);
                }
                return std::unexpected(polled.error());
            }
            status = std::move(*polled);
            if (events.onStatus) {
                events.onStatus(status);
            }
            if (status.state == GenerationState::Succeeded) {
                break;
            }
            if (status.state == GenerationState::Failed) {
                unbilled = status.unbilled;
                return std::unexpected(generationFailure(status));
            }
            if (status.state == GenerationState::Cancelled) {
                return std::unexpected(AiError(AiErrorCode::Cancelled, "the provider cancelled the job"));
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                cancelJob(*ticket);
                return std::unexpected(AiError(AiErrorCode::Network, "the provider did not finish the job in time"));
            }
            if (!m_sleeper(status.pollAfter.value_or(m_pollInterval), stop)) {
                cancelJob(*ticket);
                return std::unexpected(AiError(AiErrorCode::Cancelled, "generation cancelled"));
            }
        }
        auto files = generator.fetch(status, directory, stop);
        if (!files) {
            return std::unexpected(files.error());
        }
        GeneratedMedia out;
        out.files = std::move(*files);
        out.ticket = std::move(*ticket);
        out.provider = provider.id;
        out.model = model.id;
        out.usage = generationUsage(model, request);
        if (status.usage.videoMilliseconds > 0 || status.usage.images > 0) { // what the provider says it billed
            out.usage.videoMilliseconds = status.usage.videoMilliseconds;
            out.usage.images = status.usage.images;
        }
        // 0.2 sound effects: the length that came back (the adapter measures the file), never more than the estimate
        // the user approved - a sound the model made shorter than asked for (or than the longest, when it picked the
        // length) is booked at what it is.
        if (request.kind == GeneratedMediaKind::SoundEffect && status.usage.audioMilliseconds > 0) {
            out.usage.audioMilliseconds = std::min(out.usage.audioMilliseconds, status.usage.audioMilliseconds);
        }
        out.createdAt = nowIso8601();
        return out;
    };
    auto result = guardedCall<GeneratedMedia>(*this, provider, candidate->baseUrl, model, Capability::GenerateMedia,
                                              options, cost.ceiling, *dispatch, failure, run, &generatedUsage);
    if (result) {
        result->cost = costOf(model, result->usage, today());
    }
    return result;
}

std::expected<std::unique_ptr<IChatModel>, AiError> AiPlatform::chat(std::string_view modelId, CallOptions options) {
    auto candidate = m_providers.candidate(modelId, Capability::Chat, today());
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    auto key = keyFor(*candidate->provider);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto dispatch = std::make_unique<DispatchState>();
    auto transport = std::make_unique<TrackingTransport>(m_transport, *dispatch);
    auto inner = m_providers.createChat(*candidate, std::move(*key), *transport);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return std::make_unique<GuardedChat>(std::move(dispatch), std::move(transport), std::move(*inner), *candidate->provider,
                                         candidate->baseUrl, *this, options);
}

std::expected<std::unique_ptr<ITranscriber>, AiError> AiPlatform::transcriber(std::string_view modelId,
                                                                              CallOptions options) {
    auto candidate = m_providers.candidate(modelId, Capability::Transcribe, today());
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    auto key = keyFor(*candidate->provider);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto dispatch = std::make_unique<DispatchState>();
    auto transport = std::make_unique<TrackingTransport>(m_transport, *dispatch);
    auto inner = m_providers.createTranscriber(*candidate, std::move(*key), *transport);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return std::make_unique<GuardedTranscriber>(std::move(dispatch), std::move(transport), std::move(*inner),
                                                *candidate->provider, candidate->baseUrl, *this, options);
}

std::expected<std::unique_ptr<ISpeechSynth>, AiError> AiPlatform::speech(std::string_view modelId, CallOptions options) {
    auto candidate = m_providers.candidate(modelId, Capability::Speak, today());
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    auto key = keyFor(*candidate->provider);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto dispatch = std::make_unique<DispatchState>();
    auto transport = std::make_unique<TrackingTransport>(m_transport, *dispatch);
    auto inner = m_providers.createSpeechSynth(*candidate, std::move(*key), *transport);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return std::make_unique<GuardedSpeech>(std::move(dispatch), std::move(transport), std::move(*inner), *candidate->provider,
                                           candidate->baseUrl, *this, options);
}

std::expected<std::vector<ProviderRegistry::Candidate>, AiError> AiPlatform::resolve(Capability capability) {
    ProviderRegistry::ResolveContext context;
    context.today = today();
    context.consent = &m_consent;
    context.hasKey = [this](const ProviderSpec& provider) { return m_keys.status(provider).hasKey; };
    return m_providers.resolve(capability, context);
}

} // namespace wabcut::ai
