#include <wabcut/ai/TranscribeService.h>

#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Timestamps.h>
#include <wabcut/ai/TranscriptJson.h>
#include <wabcut/ai/VadChunker.h>
#include <wabcut/model/MediaLocator.h>

#include "TempWorkspace.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <fstream>
#include <list>
#include <thread>

namespace wabcut::ai {

namespace {

constexpr std::string_view kLocal = "local";
constexpr std::string_view kCloudPrefix = "cloud:";
constexpr std::int64_t kRate = kTranscriptionSampleRate;

std::unexpected<Error> fail(ErrorCode code, std::string message) { return std::unexpected(Error{code, std::move(message)}); }

/// An AI platform refusal or failure as an Error that still names the AI error ("ConsentRequired", ...).
Error fromAi(const AiError& e, std::string_view model) {
    ErrorCode code = ErrorCode::IoError;
    switch (e.code) {
    case AiErrorCode::Cancelled:
        code = ErrorCode::Cancelled;
        break;
    case AiErrorCode::ModelMissing:
        code = ErrorCode::NotFound;
        break;
    case AiErrorCode::NoCredentials:
    case AiErrorCode::ConsentRequired:
    case AiErrorCode::Offline:
    case AiErrorCode::SpendCapExceeded:
    case AiErrorCode::Unsupported:
    case AiErrorCode::Deprecated:
        code = ErrorCode::Unsupported;
        break;
    case AiErrorCode::InvalidRequest:
    case AiErrorCode::PayloadTooLarge:
        code = ErrorCode::InvalidArgument;
        break;
    default:
        break;
    }
    return Error{code, std::string(model) + ": " + std::string(toString(e.code)) + ": " + e.message};
}

struct Identity {
    std::uint64_t fingerprint = 0;
    int stream = -1;
    Ticks duration{};
};

TranscriptionRequest requestFor(const TranscribeSource& source, int stream) {
    TranscriptionRequest request;
    request.mediaPath = source.mediaPath;
    request.streamIndex = stream;
    return request;
}

std::expected<Identity, Error> identify(const TranscribeSource& source) {
    std::error_code ec;
    std::filesystem::path file;
    try {
        file = std::filesystem::path(reinterpret_cast<const char8_t*>(source.mediaPath.c_str()));
    } catch (const std::exception&) { // review P2-14: not UTF-8 (an ANSI command line, say)
        return fail(ErrorCode::InvalidArgument, "the media path is not valid UTF-8");
    }
    if (!std::filesystem::is_regular_file(file, ec)) {
        return fail(ErrorCode::NotFound, "no media file at " + source.mediaPath);
    }
    auto id = identifyMediaAudio(requestFor(source, source.containerStream));
    if (!id) {
        return std::unexpected(id.error());
    }
    return Identity{id->fingerprint, id->streamIndex, id->duration};
}

/// The source's audio as raw 16 kHz mono float32 into `pcm`; returns the samples written. Progress 0..1.
std::expected<std::int64_t, Error> extract(const TranscribeSource& source, const Identity& id, const std::filesystem::path& pcm,
                                           std::stop_token stop, const std::function<void(float)>& progress) {
    auto extracted = extractMediaAudio(requestFor(source, id.stream), AudioIdentity{id.fingerprint, id.stream, id.duration},
                                       pcm, std::move(stop), progress);
    if (!extracted) {
        return std::unexpected(extracted.error());
    }
    return extracted->samples;
}

/// Removes a file when it goes out of scope.
struct TempFile {
    std::filesystem::path path;
    ~TempFile() {
        std::error_code ec;
        if (!path.empty()) {
            std::filesystem::remove(path, ec);
        }
    }
};

/// Appends a chunk's transcript (already in source time) to `out`.
void append(Transcript& out, Transcript part) {
    if (out.language.empty()) {
        out.language = part.language;
    }
    if (out.provenance.engine.empty()) {
        out.provenance = part.provenance;
    }
    // Chunks are in order; keep times non-decreasing at a chunk seam (a provider may round a word a little early).
    const std::size_t base = out.words.size();
    const Ticks wordFloor = out.words.empty() ? Ticks{} : out.words.back().t1;
    for (TranscriptWord& w : part.words) {
        w.t0 = std::max(w.t0, wordFloor);
        w.t1 = std::max(w.t1, w.t0);
        out.words.push_back(std::move(w));
    }
    const Ticks segmentFloor = out.segments.empty() ? Ticks{} : out.segments.back().t1;
    for (TranscriptSegment& s : part.segments) {
        s.firstWord += base;
        s.t0 = std::max(s.t0, segmentFloor);
        s.t1 = std::max(s.t1, s.t0);
        out.segments.push_back(std::move(s));
    }
}

} // namespace

std::expected<std::int64_t, Error> writeWav16k(const std::filesystem::path& pcm, std::int64_t first, std::int64_t count,
                                               const std::filesystem::path& wav) {
    std::ifstream in(pcm, std::ios::binary);
    if (!in) {
        return fail(ErrorCode::IoError, "cannot read " + pathToUtf8(pcm));
    }
    in.seekg(static_cast<std::streamoff>(first * static_cast<std::int64_t>(sizeof(float))));
    std::ofstream file(wav, std::ios::binary | std::ios::trunc);
    if (!file) {
        return fail(ErrorCode::IoError, "cannot write " + pathToUtf8(wav));
    }
    const auto u32 = [&](std::uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
    file.write("RIFF", 4);
    u32(0);
    file.write("WAVEfmt ", 8);
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(16000);
    u32(32000);
    u16(2);
    u16(16);
    file.write("data", 4);
    u32(0);
    std::vector<float> buffer(16000);
    std::vector<std::int16_t> pcm16(buffer.size());
    std::int64_t written = 0;
    while (written < count) {
        const auto want = static_cast<std::size_t>(std::min<std::int64_t>(count - written, static_cast<std::int64_t>(buffer.size())));
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(want * sizeof(float)));
        const auto got = static_cast<std::size_t>(in.gcount()) / sizeof(float);
        for (std::size_t i = 0; i < got; ++i) {
            pcm16[i] = static_cast<std::int16_t>(std::lround(std::clamp(buffer[i], -1.0f, 1.0f) * 32767.0f));
        }
        file.write(reinterpret_cast<const char*>(pcm16.data()), static_cast<std::streamsize>(got * 2));
        written += static_cast<std::int64_t>(got);
        if (got < want) {
            break;
        }
    }
    const auto dataBytes = static_cast<std::uint32_t>(written * 2);
    file.seekp(4);
    u32(36 + dataBytes);
    file.seekp(40);
    u32(dataBytes);
    if (!file) {
        return fail(ErrorCode::IoError, "cannot write " + pathToUtf8(wav));
    }
    return written;
}

std::string cpuFallbackNote(std::string_view device) {
    std::string lower(device);
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower.empty() || lower.find("cpu") == std::string::npos) {
        return {}; // a GPU (or unknown: a cache hit does not say)
    }
    return "no Vulkan GPU was usable (no driver, an unsupported one, or a build without the Vulkan backend): "
           "transcribed on the CPU, which is several times slower. Install or update the graphics driver for GPU "
           "transcription.";
}

std::vector<SampleRange> energySpeech(const std::filesystem::path& pcm, std::int64_t samples) {
    constexpr std::int64_t kFrame = kRate / 100; // 10 ms
    std::ifstream in(pcm, std::ios::binary);
    std::vector<double> levels;
    std::vector<float> frame(static_cast<std::size_t>(kFrame));
    for (std::int64_t at = 0; at < samples && in; at += kFrame) {
        in.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size() * sizeof(float)));
        const auto got = static_cast<std::size_t>(in.gcount()) / sizeof(float);
        double sum = 0.0;
        for (std::size_t i = 0; i < got; ++i) {
            sum += static_cast<double>(frame[i]) * frame[i];
        }
        const double ms = got > 0 ? sum / static_cast<double>(got) : 0.0;
        levels.push_back(ms > 1e-20 ? 10.0 * std::log10(ms) : -200.0);
    }
    std::vector<double> audible;
    for (const double l : levels) {
        if (l > -100.0) {
            audible.push_back(l);
        }
    }
    if (audible.empty()) {
        return {};
    }
    const auto nth = audible.begin() + static_cast<std::ptrdiff_t>(audible.size() / 10);
    std::nth_element(audible.begin(), nth, audible.end());
    // Speech: 12 dB over the noise floor (between -50 and -30 dBFS), with 6 dB of hysteresis.
    const double on = std::clamp(*nth + 12.0, -50.0, -30.0);
    const double off = on - 6.0;
    std::vector<SampleRange> out;
    bool speaking = false;
    std::int64_t begin = 0;
    for (std::size_t i = 0; i <= levels.size(); ++i) {
        const double l = i < levels.size() ? levels[i] : -1e9;
        if (!speaking && l >= on) {
            speaking = true;
            begin = static_cast<std::int64_t>(i) * kFrame;
        } else if (speaking && l < off) {
            speaking = false;
            out.push_back({begin, std::min(static_cast<std::int64_t>(i) * kFrame, samples)});
        }
    }
    return out;
}

// ---- The service -------------------------------------------------------------------------------------------------

struct TranscribeService::Impl {
    TranscribeServiceConfig config;
    std::unique_ptr<TranscriptionService> local;
    mutable std::mutex mutex;
    std::map<std::string, std::pair<std::string, CustomTranscriber>, std::less<>> custom;
    struct Running {
        std::shared_ptr<media::detail::JobState<TranscribeOutcome>> state;
        std::thread thread;
    };
    std::list<Running> jobs;

    bool localAvailable() const {
        std::error_code ec;
        return std::filesystem::exists(config.local.inferExecutable, ec);
    }

    std::optional<CustomTranscriber> customFor(std::string_view id) const {
        std::lock_guard lock(mutex);
        if (const auto it = custom.find(id); it != custom.end()) {
            return it->second.second;
        }
        return std::nullopt;
    }

    std::expected<std::string, Error> resolve(std::string_view requested) const {
        if (requested.empty() || requested == "auto") {
            if (localAvailable()) {
                return std::string(kLocal);
            }
            if (config.platform != nullptr) {
                if (auto candidates = config.platform->resolve(Capability::Transcribe); candidates && !candidates->empty()) {
                    return std::string(kCloudPrefix) + candidates->front().model->id;
                }
            }
            return fail(ErrorCode::NotFound,
                        "no transcription provider: build with WABCUT_WITH_LOCAL_AI=ON for local transcription, or choose a "
                        "cloud transcription model and add its key (Preferences > AI; wabcut-cli: --provider cloud:<model>)");
        }
        if (requested == kLocal) {
            if (!localAvailable()) {
                return fail(ErrorCode::NotFound, "local transcription is not part of this build (WABCUT_WITH_LOCAL_AI)");
            }
            return std::string(requested);
        }
        if (requested.starts_with(kCloudPrefix)) {
            if (config.platform == nullptr) {
                return fail(ErrorCode::Unsupported, "cloud transcription is not available here");
            }
            return std::string(requested);
        }
        if (customFor(requested)) {
            return std::string(requested);
        }
        return fail(ErrorCode::NotFound, "unknown transcription provider \"" + std::string(requested) + "\"");
    }

    std::shared_ptr<const Transcript> fromCache(const Identity& id, const TranscribeSource& source,
                                                std::string* provider) const {
        if (config.cacheRoot.empty()) {
            return nullptr;
        }
        const AnalysisCache cache(config.cacheRoot);
        const auto entry = cache.newest(id.fingerprint, id.stream, "transcript");
        if (!entry) {
            return nullptr;
        }
        auto t = transcriptFromJson(entry->payload);
        if (!t) {
            return nullptr;
        }
        t->source.path = source.mediaPath;
        if (provider != nullptr) {
            // Ours say; the local engine records the device it ran on; anything else came from a cloud adapter.
            const auto& params = t->provenance.params;
            const auto p = params.find("provider");
            *provider = p != params.end()                                         ? p->second
                        : params.contains("device") || t->provenance.engine == "whisper.cpp" ? std::string(kLocal)
                                                                                  : std::string(kCloudPrefix) + t->provenance.modelId;
        }
        return std::make_shared<const Transcript>(std::move(*t));
    }

    void store(const Identity& id, const std::string& modelVersion, const std::map<std::string, std::string>& params,
               const Transcript& t) const {
        if (config.cacheRoot.empty()) {
            return;
        }
        AnalysisCache cache(config.cacheRoot);
        [[maybe_unused]] const auto stored =
            cache.put(AnalysisKey{id.fingerprint, id.stream, "transcript", modelVersion, hashParams(params)}, toJson(t));
    }

    std::expected<Transcript, Error> runCloud(const std::string& model, const TranscribeSource& source, const Identity& id,
                                              const TranscribeJobOptions& options, std::stop_token stop,
                                              const std::function<void(float)>& report) {
        CallOptions call;
        call.allowOnce = options.allowOnce;
        call.caller = options.caller;
        auto created = config.platform->transcriber(model, call);
        if (!created) {
            return std::unexpected(fromAi(created.error(), model));
        }
        ITranscriber& transcriber = **created;
        // Review P2-16: what the guards would refuse at the first request is refused before decoding anything:
        // consent and the offline / no-cloud locks, and a whole file that cannot fit the monthly cap.
        if (auto pre = config.platform->preflightTranscription(
                model, std::max<std::int64_t>(id.duration.value / (kTicksPerSecond / 1000), 0));
            pre && pre->candidate.provider != nullptr) {
            if (auto ok = config.platform->consent().authorize(*pre->candidate.provider, pre->candidate.baseUrl,
                                                               Capability::Transcribe, options.allowOnce, options.caller);
                !ok) {
                return std::unexpected(fromAi(ok.error(), model));
            }
            if (pre->remainingBudget && pre->cost.priced && pre->cost.ceiling > *pre->remainingBudget) {
                return std::unexpected(fromAi(AiError{AiErrorCode::SpendCapExceeded,
                                                      "the whole file would exceed this month's cap for " +
                                                          pre->candidate.provider->name},
                                              model));
            }
        }
        auto workspace = detail::TempWorkspace::create(config.workDir);
        if (!workspace) {
            return std::unexpected(workspace.error());
        }
        TempFile pcm{(*workspace)->path() / "audio.f32"};
        auto samples = extract(source, id, pcm.path, stop, [&](float p) { report(0.2f * p); });
        if (!samples) {
            return std::unexpected(samples.error());
        }
        // Review P1-3: redacted speech never leaves the machine.
        if (auto silenced = silencePcmRanges(pcm.path, *samples, source.redacted); !silenced) {
            return std::unexpected(silenced.error());
        }
        // One request at most cloudChunk long and within the model's documented limits.
        const ModelSpec& spec = transcriber.spec();
        std::int64_t limit = std::max<std::int64_t>(config.cloudChunk.value / (kTicksPerSecond / kRate), kRate);
        if (spec.maxAudioSeconds > 0) {
            limit = std::min(limit, spec.maxAudioSeconds * kRate);
        }
        if (spec.maxUploadBytes > 0) {
            limit = std::min(limit, std::max<std::int64_t>((spec.maxUploadBytes - 4096) / 2, kRate));
        }
        std::vector<SampleRange> chunks;
        if (*samples <= limit) {
            chunks.push_back({0, *samples});
        } else {
            std::vector<SampleRange> speech;
            if (localAvailable()) {
                if (auto vad = local->detectSpeech(pcm.path, *samples, stop, options.local.vadModel)) {
                    speech = std::move(*vad);
                }
            }
            if (speech.empty()) {
                speech = energySpeech(pcm.path, *samples);
            }
            ChunkOptions chunking;
            chunking.maxChunkSamples = limit;
            // Cut only at real pauses (a stretch of speech is never split unless it alone is over the limit), and
            // keep silences inside a request: they cost nothing there, so fewer, longer requests.
            chunking.mergeGapSamples = kRate * 3 / 10;
            chunking.maxGapSamples = limit;
            chunks = planChunks(speech, *samples, chunking);
            if (chunks.empty()) {
                chunks = fixedChunks(*samples, chunking);
            }
        }
        std::int64_t total = 0;
        for (const SampleRange& c : chunks) {
            total += c.length();
        }
        Transcript out;
        std::int64_t done = 0;
        for (const SampleRange& c : chunks) {
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "transcription cancelled");
            }
            TempFile wav{(*workspace)->path() / "upload.wav"};
            if (auto written = writeWav16k(pcm.path, c.begin, c.length(), wav.path); !written) {
                return std::unexpected(written.error());
            }
            TranscribeRequest request;
            request.audioFile = wav.path;
            request.mediaType = "audio/wav";
            request.language = options.language == "auto" ? std::string() : options.language;
            request.audioMilliseconds = c.length() * 1000 / kRate;
            request.source = TranscriptSource{id.fingerprint, id.stream, source.mediaPath};
            request.origin = transcriptionSamplesToTicks(c.begin).value_or(Ticks{});
            const double before = static_cast<double>(done);
            auto result = transcriber.transcribe(
                request,
                [&](float p) {
                    report(0.2f + 0.8f * static_cast<float>((before + p * static_cast<double>(c.length())) /
                                                            static_cast<double>(std::max<std::int64_t>(total, 1))));
                },
                stop);
            if (!result) {
                return std::unexpected(fromAi(result.error(), model));
            }
            append(out, std::move(result->transcript));
            done += c.length();
        }
        out.source = TranscriptSource{id.fingerprint, id.stream, source.mediaPath};
        out.provenance.params["provider"] = std::string(kCloudPrefix) + model;
        out.provenance.params["chunks"] = std::to_string(chunks.size());
        if (auto valid = out.validate(); !valid) {
            return fail(ErrorCode::DecoderError, model + " returned an invalid transcript: " + valid.error().message);
        }
        redactTranscript(out, source.redacted);
        std::map<std::string, std::string> params{{"model", model}, {"language", options.language}};
        if (!source.redacted.empty()) {
            params["redacted"] = rangesKey(source.redacted);
        }
        store(id, "cloud/" + model, params, out);
        return out;
    }

    std::expected<TranscribeOutcome, Error> run(const TranscribeSource& source, const TranscribeJobOptions& options,
                                                std::stop_token stop, const std::function<void(float)>& progress) {
        auto report = [&](float p) {
            if (progress) {
                progress(std::clamp(p, 0.0f, 1.0f));
            }
        };
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "transcription cancelled");
        }
        auto id = identify(source);
        if (!id) {
            return std::unexpected(id.error());
        }
        // A transcript made earlier (by any provider) needs no provider at all.
        if (options.useCache) {
            std::string madeBy;
            if (auto hit = fromCache(*id, source, &madeBy)) {
                report(1.0f);
                TranscribeOutcome cachedOutcome;
                cachedOutcome.transcript = *hit;
                redactTranscript(cachedOutcome.transcript, source.redacted); // made before a redaction, perhaps
                cachedOutcome.provider = std::move(madeBy);
                cachedOutcome.fromCache = true;
                return cachedOutcome;
            }
        }
        auto provider = resolve(options.provider);
        if (!provider) {
            return std::unexpected(provider.error());
        }
        TranscribeOutcome outcome;
        outcome.provider = *provider;
        if (*provider == kLocal) {
            TranscriptionRequest request;
            request.mediaPath = source.mediaPath;
            request.streamIndex = id->stream;
            request.options = options.local;
            request.options.language = options.language;
            request.options.useCache = true;               // the result is always stored ...
            request.options.refresh = !options.useCache;   // ... and only read back when allowed
            request.silence = source.redacted;             // redacted speech is never recognised
            request.onStats = [&](const TranscriptionStats& stats) {
                outcome.localStats = stats;
                outcome.device = stats.device;
            };
            auto t = local->run(request, stop, report);
            if (!t) {
                return std::unexpected(t.error());
            }
            outcome.transcript = std::move(*t);
            redactTranscript(outcome.transcript, source.redacted);
            outcome.note = options.local.useGpu ? cpuFallbackNote(outcome.device) : std::string{};
        } else if (provider->starts_with(kCloudPrefix)) {
            auto t = runCloud(provider->substr(kCloudPrefix.size()), source, *id, options, stop, report);
            if (!t) {
                return std::unexpected(t.error());
            }
            outcome.transcript = std::move(*t);
        } else {
            const auto engine = customFor(*provider);
            auto t = (*engine)(source, options, report, stop);
            if (!t) {
                return std::unexpected(t.error());
            }
            t->source = TranscriptSource{id->fingerprint, id->stream, source.mediaPath};
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "transcription cancelled");
            }
            t->provenance.params["provider"] = *provider;
            if (auto valid = t->validate(); !valid) {
                return fail(ErrorCode::DecoderError, *provider + " returned an invalid transcript: " + valid.error().message);
            }
            redactTranscript(*t, source.redacted);
            std::map<std::string, std::string> params{{"language", options.language}};
            if (!source.redacted.empty()) {
                params["redacted"] = rangesKey(source.redacted);
            }
            store(*id, "custom/" + *provider, params, *t);
            outcome.transcript = std::move(*t);
        }
        report(1.0f);
        return outcome;
    }
};

TranscribeService::TranscribeService(TranscribeServiceConfig config) : m_impl(std::make_unique<Impl>()) {
    if (config.local.inferExecutable.empty()) {
        config.local.inferExecutable = defaultInferExecutable();
    }
    if (config.workDir.empty()) {
        std::error_code ec;
        config.workDir = std::filesystem::temp_directory_path(ec) / "wabcut-transcribe";
    }
    config.local.cacheRoot = config.cacheRoot;
    m_impl->local = std::make_unique<TranscriptionService>(config.local);
    m_impl->config = std::move(config);
}

TranscribeService::~TranscribeService() {
    std::list<Impl::Running> jobs;
    {
        std::lock_guard lock(m_impl->mutex);
        jobs.swap(m_impl->jobs);
    }
    for (Impl::Running& job : jobs) {
        job.state->stop.request_stop();
    }
    for (Impl::Running& job : jobs) {
        if (job.thread.joinable()) {
            job.thread.join();
        }
    }
}

void TranscribeService::addProvider(std::string id, std::string label, CustomTranscriber transcriber) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->custom[std::move(id)] = {std::move(label), std::move(transcriber)};
}

std::vector<TranscribeProviderInfo> TranscribeService::providers() const {
    std::vector<TranscribeProviderInfo> out;
    const bool local = m_impl->localAvailable();
    out.push_back({std::string(kLocal), "On this computer", "whisper.cpp: private, free", local,
                   local ? std::string() : "local transcription is not part of this build (WABCUT_WITH_LOCAL_AI)"});
    if (AiPlatform* platform = m_impl->config.platform) {
        const bool cloud = platform->consent().cloudAllowed();
        for (const ModelSpec* m : platform->providers().models().modelsWith(Capability::Transcribe)) {
            const ProviderSpec* p = platform->providers().models().provider(m->provider);
            TranscribeProviderInfo info{std::string(kCloudPrefix) + m->id, m->name.empty() ? m->id : m->name,
                                        p != nullptr ? p->name : m->provider, false, {}};
            const auto candidate = platform->providers().candidate(m->id, Capability::Transcribe, platform->today());
            if (!candidate) {
                info.reason = candidate.error().message;
            } else if (!cloud && (p == nullptr || !p->local)) {
                info.reason = "cloud AI is off (offline mode or the project's no-cloud lock)";
            } else if (p != nullptr && p->keyRequired && !platform->keys().status(*p).hasKey) {
                info.reason = "no key for " + p->name;
            } else {
                info.available = true;
            }
            out.push_back(std::move(info));
        }
    }
    std::lock_guard lock(m_impl->mutex);
    for (const auto& [id, entry] : m_impl->custom) {
        out.push_back({id, entry.first, {}, true, {}});
    }
    return out;
}

std::expected<std::string, Error> TranscribeService::resolveProvider(std::string_view requested) const {
    return m_impl->resolve(requested);
}

std::shared_ptr<const Transcript> TranscribeService::cached(const TranscribeSource& source) const {
    auto id = identify(source);
    auto hit = id ? m_impl->fromCache(*id, source, nullptr) : nullptr;
    if (hit && !source.redacted.empty()) {
        auto redacted = std::make_shared<Transcript>(*hit);
        redactTranscript(*redacted, source.redacted);
        return redacted;
    }
    return hit;
}

media::Job<TranscribeOutcome> TranscribeService::transcribe(TranscribeSource source, TranscribeJobOptions options) {
    auto state = std::make_shared<media::detail::JobState<TranscribeOutcome>>();
    std::list<Impl::Running> finished; // joined below, outside the lock
    {
        std::lock_guard lock(m_impl->mutex);
        for (auto it = m_impl->jobs.begin(); it != m_impl->jobs.end();) {
            bool done = false;
            {
                std::lock_guard l(it->state->mutex);
                done = it->state->result.has_value();
            }
            const auto next = std::next(it);
            if (done) {
                finished.splice(finished.end(), m_impl->jobs, it);
            }
            it = next;
        }
        Impl* impl = m_impl.get();
        m_impl->jobs.push_back({state, std::thread([impl, state, source = std::move(source), options = std::move(options)] {
                                    std::expected<TranscribeOutcome, Error> result = std::unexpected(Error{ErrorCode::Unknown, "transcription failed"});
                                    try {
                                        result = impl->run(source, options, state->stop.get_token(), [&](float p) {
                                            state->progress.store(std::min(p, 0.999f), std::memory_order_relaxed);
                                        });
                                    } catch (const std::exception& e) { // review P2-13: an exception must not terminate the app
                                        result = std::unexpected(Error{ErrorCode::Unknown, std::string("transcription failed: ") + e.what()});
                                    } catch (...) {
                                    }
                                    state->complete(std::move(result));
                                })});
    }
    for (Impl::Running& job : finished) {
        job.thread.join();
    }
    return media::Job<TranscribeOutcome>(state);
}

std::expected<TranscribeOutcome, Error> TranscribeService::run(const TranscribeSource& source,
                                                               const TranscribeJobOptions& options, std::stop_token stop,
                                                               const std::function<void(float)>& progress) {
    return m_impl->run(source, options, std::move(stop), progress);
}

} // namespace wabcut::ai
