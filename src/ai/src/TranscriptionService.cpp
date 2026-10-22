#include <wabcut/ai/TranscriptionService.h>

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/InferProtocol.h>
#include <wabcut/ai/Timestamps.h>
#include <wabcut/ai/TranscriptJson.h>

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/model/MediaLocator.h>

#include "JsonUtil.h"
#include "TempWorkspace.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <list>
#include <mutex>
#include <thread>

namespace wabcut::ai {

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

/// Version of everything between whisper's tokens and the Transcript (word building, chunking, mapping). Part
/// of the cache key: bump it when a change would give different transcripts for the same model output.
constexpr std::string_view kPipelineVersion = "words-1";

std::unexpected<Error> fail(ErrorCode code, std::string message) { return std::unexpected(Error{code, std::move(message)}); }

std::expected<AudioIdentity, Error> identifyWithMedia(const TranscriptionRequest& request) {
    auto fingerprint = media::sourceFingerprint(request.mediaPath);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    auto decoder = media::AudioDecoder::open(request.mediaPath, request.streamIndex, kTranscriptionRate,
                                             media::ChannelLayout::mono());
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    return AudioIdentity{*fingerprint, (*decoder)->streamInfo().index, (*decoder)->streamInfo().duration};
}

std::expected<ExtractedAudio, Error> extractWithMedia(const TranscriptionRequest& request, const AudioIdentity& identity,
                                                      const std::filesystem::path& pcmFile, std::stop_token stop,
                                                      const std::function<void(float)>& progress) {
    auto decoder = media::AudioDecoder::open(request.mediaPath, identity.streamIndex, kTranscriptionRate,
                                             media::ChannelLayout::mono());
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::ofstream out(pcmFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(ErrorCode::IoError, "cannot write " + pathToUtf8(pcmFile));
    }
    // Output sample n covers source time samplesToTicks(n): sample 0 is the media origin.
    const std::int64_t expected = identity.duration > Ticks{} ? ticksToSamples(identity.duration, kTranscriptionRate) : 0;
    const std::int64_t limit = expected > 0 ? expected + 60LL * kTranscriptionSampleRate : 48LL * 3600 * kTranscriptionSampleRate;
    std::vector<float> block(static_cast<std::size_t>(kTranscriptionSampleRate) * 10);
    std::int64_t samples = 0;
    while (samples < limit) {
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "transcription cancelled");
        }
        auto read = (*decoder)->read(samplesToTicks(samples, kTranscriptionRate), block);
        if (!read) {
            return std::unexpected(read.error());
        }
        const std::size_t useful = read->endOfStream ? read->leadingSilence + read->framesFromStream : block.size();
        out.write(reinterpret_cast<const char*>(block.data()), static_cast<std::streamsize>(useful * sizeof(float)));
        if (!out) {
            return fail(ErrorCode::IoError, "cannot write " + pathToUtf8(pcmFile) + " (disk full?)");
        }
        samples += static_cast<std::int64_t>(useful);
        if (progress && expected > 0) {
            progress(std::min(1.0f, static_cast<float>(samples) / static_cast<float>(expected)));
        }
        if (read->endOfStream) {
            break;
        }
    }
    return ExtractedAudio{samples, Ticks{}};
}

ErrorCode codeForHelperError(const std::string& code) {
    if (code == "cancelled") {
        return ErrorCode::Cancelled;
    }
    if (code == "bad_request") {
        return ErrorCode::InvalidArgument;
    }
    if (code == "io") {
        return ErrorCode::IoError;
    }
    return ErrorCode::DecoderError;
}

int automaticThreads() {
    const unsigned hw = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp<unsigned>(hw == 0 ? 4 : hw, 1, 8));
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

/// One running wabcut-infer: request/response with deadlines, cancellation and crash detection.
class HelperSession {
public:
    HelperSession(std::unique_ptr<ChildProcess> child, std::stop_token stop, std::chrono::milliseconds inactivity)
        : m_child(std::move(child)), m_stop(std::move(stop)), m_inactivity(inactivity) {}
    ~HelperSession() {
        if (m_child) {
            m_child->writeLine(infer::encode(infer::Request{infer::Shutdown{}}));
            m_child->closeInput();
            if (!m_child->wait(2s)) {
                m_child->kill();
            }
        }
    }

    bool send(const infer::Request& request) { return m_child->writeLine(infer::encode(request)); }

    /// The next response, or an error that ends the session (the helper is killed). `what` describes the
    /// current step for messages; `deadline` bounds the whole wait, `m_inactivity` the silence between lines.
    std::expected<infer::Response, Error> next(const char* what, std::chrono::milliseconds deadline) {
        const auto until = Clock::now() + deadline;
        auto lastLine = Clock::now();
        std::string line;
        for (;;) {
            if (m_stop.stop_requested()) {
                m_child->kill(); // cancel = kill: whisper may be deep inside a GPU kernel
                return fail(ErrorCode::Cancelled, "transcription cancelled");
            }
            switch (m_child->readLine(line, 100ms)) {
            case ChildProcess::ReadStatus::Line: {
                auto response = infer::decodeResponse(line);
                if (!response) {
                    m_child->kill();
                    return fail(ErrorCode::DecoderError, std::string("wabcut-infer sent an invalid message while ") +
                                                             what + ": " + response.error().message);
                }
                return response;
            }
            case ChildProcess::ReadStatus::Closed: {
                const auto code = m_child->wait(5s);
                m_child->kill();
                return fail(ErrorCode::DecoderError,
                            std::string("wabcut-infer ") +
                                (code ? ChildProcess::describeExit(*code) : std::string("closed its output")) +
                                " while " + what);
            }
            case ChildProcess::ReadStatus::Overflow:
                m_child->kill(); // it broke the protocol's bounds
                return fail(ErrorCode::DecoderError, std::string("wabcut-infer sent more output than allowed while ") +
                                                         what + "; it was stopped");
            case ChildProcess::ReadStatus::Timeout:
                break;
            }
            const auto now = Clock::now();
            if (now >= until || now - lastLine >= m_inactivity) {
                m_child->kill();
                const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
                    now >= until ? deadline : std::chrono::duration_cast<std::chrono::milliseconds>(m_inactivity));
                return fail(ErrorCode::DecoderError, std::string("wabcut-infer did not answer within ") +
                                                         std::to_string(waited.count()) + " s while " + what +
                                                         "; it was stopped");
            }
        }
    }

    /// Waits for the answer to request `id` that `accept` takes, skipping nothing else: a Failure for it becomes
    /// an Error; any other message is a protocol error.
    template <class T>
    std::expected<T, Error> expect(const std::string& id, const char* what, std::chrono::milliseconds deadline) {
        auto response = next(what, deadline);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (auto* failure = std::get_if<infer::Failure>(&*response); failure != nullptr) {
            return fail(codeForHelperError(failure->code), std::string("wabcut-infer: ") + what + " failed: " + failure->message);
        }
        if (auto* value = std::get_if<T>(&*response); value != nullptr && infer::responseId(*response) == id) {
            return *value;
        }
        m_child->kill();
        return fail(ErrorCode::DecoderError, std::string("wabcut-infer answered out of order while ") + what);
    }

private:
    std::unique_ptr<ChildProcess> m_child;
    std::stop_token m_stop;
    std::chrono::milliseconds m_inactivity;
};

std::string makeId(int n) { return std::to_string(n); }

} // namespace

std::expected<void, Error> silencePcmRanges(const std::filesystem::path& pcmFile, std::int64_t samples,
                                            std::span<const TimeRange> ranges) {
    if (ranges.empty() || samples <= 0) {
        return {};
    }
    std::fstream file(pcmFile, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        return fail(ErrorCode::IoError, "cannot open " + pathToUtf8(pcmFile));
    }
    const std::vector<float> zeros(4096, 0.0f);
    for (const TimeRange& r : ranges) {
        // Every sample that plays any part of the range: floor of the start, ceiling of the end.
        const std::int64_t a = std::clamp<std::int64_t>(
            r.start.value <= 0 ? 0 : mulDiv(r.start.value, kTranscriptionSampleRate, kTicksPerSecond, Rounding::Floor).value_or(samples),
            0, samples);
        const std::int64_t b = std::clamp<std::int64_t>(
            r.end().value <= 0 ? 0 : mulDiv(r.end().value, kTranscriptionSampleRate, kTicksPerSecond, Rounding::Ceil).value_or(samples),
            0, samples);
        file.seekp(static_cast<std::streamoff>(a * static_cast<std::int64_t>(sizeof(float))));
        for (std::int64_t at = a; at < b;) {
            const auto n = static_cast<std::size_t>(std::min<std::int64_t>(b - at, static_cast<std::int64_t>(zeros.size())));
            file.write(reinterpret_cast<const char*>(zeros.data()), static_cast<std::streamsize>(n * sizeof(float)));
            at += static_cast<std::int64_t>(n);
        }
    }
    file.flush();
    if (!file) {
        return fail(ErrorCode::IoError, "cannot write " + pathToUtf8(pcmFile));
    }
    return {};
}

std::string rangesKey(std::span<const TimeRange> ranges) {
    std::string out;
    for (const TimeRange& r : ranges) {
        out += std::to_string(r.start.value) + "+" + std::to_string(r.duration.value) + ";";
    }
    return out;
}

std::expected<AudioIdentity, Error> identifyMediaAudio(const TranscriptionRequest& request) {
    return identifyWithMedia(request);
}

std::expected<ExtractedAudio, Error> extractMediaAudio(const TranscriptionRequest& request, const AudioIdentity& identity,
                                                       const std::filesystem::path& pcmFile, std::stop_token stop,
                                                       const std::function<void(float)>& progress) {
    return extractWithMedia(request, identity, pcmFile, std::move(stop), progress);
}

std::filesystem::path defaultInferExecutable() {
    if (const char* overridePath = std::getenv("WABCUT_INFER_PATH"); overridePath != nullptr && *overridePath != '\0') {
        return std::filesystem::path(reinterpret_cast<const char8_t*>(overridePath));
    }
#ifdef _WIN32
    return currentExecutableDir() / "wabcut-infer.exe";
#else
    return currentExecutableDir() / "wabcut-infer";
#endif
}

struct TranscriptionService::Impl {
    TranscriptionServiceConfig config;
    std::mutex mutex;
    struct Running {
        std::shared_ptr<media::detail::JobState<Transcript>> state;
        std::thread thread;
    };
    std::list<Running> jobs;

    std::expected<Transcript, Error> run(const TranscriptionRequest& request, std::stop_token stop,
                                         const std::function<void(float)>& progress);
};

TranscriptionService::TranscriptionService(TranscriptionServiceConfig config) : m_impl(std::make_unique<Impl>()) {
    if (config.inferExecutable.empty()) {
        config.inferExecutable = defaultInferExecutable();
    }
    if (config.workDir.empty()) {
        std::error_code ec;
        config.workDir = std::filesystem::temp_directory_path(ec) / "wabcut-transcribe";
    }
    if (!config.identify) {
        config.identify = identifyWithMedia;
    }
    if (!config.extract) {
        config.extract = extractWithMedia;
    }
    m_impl->config = std::move(config);
}

TranscriptionService::~TranscriptionService() {
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

media::Job<Transcript> TranscriptionService::transcribe(TranscriptionRequest request) {
    auto state = std::make_shared<media::detail::JobState<Transcript>>();
    std::lock_guard lock(m_impl->mutex);
    // Reap finished jobs.
    m_impl->jobs.remove_if([](Impl::Running& r) {
        std::lock_guard l(r.state->mutex);
        if (r.state->result && r.thread.joinable()) {
            r.thread.join();
            return true;
        }
        return false;
    });
    Impl* impl = m_impl.get();
    m_impl->jobs.push_back({state, std::thread([impl, state, request = std::move(request)] {
                                std::expected<Transcript, Error> result =
                                    std::unexpected(Error{ErrorCode::Unknown, "transcription failed"});
                                try {
                                    result = impl->run(request, state->stop.get_token(), [&](float p) {
                                        state->progress.store(std::min(p, 0.999f), std::memory_order_relaxed);
                                    });
                                } catch (const std::exception& e) { // review P2-13
                                    result = std::unexpected(Error{ErrorCode::Unknown, std::string("transcription failed: ") + e.what()});
                                } catch (...) {
                                }
                                state->complete(std::move(result));
                            })});
    return media::Job<Transcript>(state);
}

std::expected<Transcript, Error> TranscriptionService::run(const TranscriptionRequest& request, std::stop_token stop,
                                                           const std::function<void(float)>& progress) {
    return m_impl->run(request, std::move(stop), progress);
}

std::expected<std::vector<SampleRange>, Error> TranscriptionService::detectSpeech(const std::filesystem::path& pcmFile,
                                                                                 std::int64_t samples, std::stop_token stop,
                                                                                 std::string vadModelId) {
    const TranscriptionServiceConfig& config = m_impl->config;
    const ModelInfo* vadModel = nullptr;
    for (const ModelInfo& m : config.models) {
        if (m.id == vadModelId) {
            vadModel = &m;
        }
    }
    if (vadModel == nullptr) {
        vadModel = findModel(vadModelId);
    }
    if (vadModel == nullptr || vadModel->kind != ModelKind::Vad) {
        return fail(ErrorCode::InvalidArgument, "unknown VAD model '" + vadModelId + "'");
    }
    std::error_code ec;
    if (!std::filesystem::exists(config.inferExecutable, ec)) {
        return fail(ErrorCode::NotFound, "local AI is not part of this build (no wabcut-infer helper)");
    }
    ModelStore store(config.modelRoot);
    auto vadPath = store.ensure(*vadModel, config.download, {}, stop);
    if (!vadPath) {
        return std::unexpected(vadPath.error());
    }
    ChildProcessOptions childOptions;
    childOptions.executable = config.inferExecutable;
    childOptions.arguments = config.helperArguments;
    childOptions.stderrFile = config.helperLog;
    childOptions.aiHelper = true; // 0.2: below normal and the helper policy's CPU cap (Settings > AI)
    auto child = ChildProcess::spawn(childOptions);
    if (!child) {
        return std::unexpected(child.error());
    }
    HelperSession helper(std::move(*child), stop, config.inactivityTimeout);
    if (!helper.send(infer::Hello{})) {
        return fail(ErrorCode::DecoderError, "wabcut-infer exited at start");
    }
    auto hello = helper.expect<infer::HelloReply>("", "starting", config.helperStartTimeout);
    if (!hello) {
        return std::unexpected(hello.error());
    }
    helper.send(infer::LoadVad{"vad", *vadPath});
    if (auto loaded = helper.expect<infer::Loaded>("vad", "loading the VAD model", config.modelLoadTimeout); !loaded) {
        return std::unexpected(loaded.error());
    }
    std::vector<SampleRange> speech;
    int nextId = 1;
    constexpr std::int64_t kBlock = 600LL * kTranscriptionSampleRate; // bounded memory in the helper
    for (std::int64_t begin = 0; begin < samples; begin += kBlock) {
        const std::int64_t count = std::min(kBlock, samples - begin);
        const std::string id = makeId(nextId++);
        helper.send(infer::VadRequest{id, {pcmFile, begin, count}});
        auto vad = helper.expect<infer::VadResult>(id, "detecting speech", config.inactivityTimeout);
        if (!vad) {
            return std::unexpected(vad.error());
        }
        const auto found = speechFromCentiseconds(vad->segments, begin, count);
        speech.insert(speech.end(), found.begin(), found.end());
    }
    return speech;
}

std::expected<Transcript, Error> TranscriptionService::Impl::run(const TranscriptionRequest& request,
                                                                 std::stop_token stop,
                                                                 const std::function<void(float)>& progress) {
    const TranscriptionOptions& options = request.options;
    const auto jobStart = Clock::now();
    auto msSince = [](Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); };
    TranscriptionStats stats;
    auto report = [&](float p) {
        if (progress) {
            progress(std::clamp(p, 0.0f, 1.0f));
        }
    };
    auto resolve = [&](const std::string& id) -> const ModelInfo* {
        for (const ModelInfo& m : config.models) {
            if (m.id == id) {
                return &m;
            }
        }
        return findModel(id);
    };
    const ModelInfo* model = resolve(options.model);
    if (model == nullptr || model->kind != ModelKind::Whisper) {
        return fail(ErrorCode::InvalidArgument, "unknown transcription model '" + options.model + "'");
    }
    const ModelInfo* vadModel = options.vad ? resolve(options.vadModel) : nullptr;
    if (options.vad && (vadModel == nullptr || vadModel->kind != ModelKind::Vad)) {
        return fail(ErrorCode::InvalidArgument, "unknown VAD model '" + options.vadModel + "'");
    }
    if (options.beamSize < 1 || options.beamSize > 16 || options.threads < 0) {
        return fail(ErrorCode::InvalidArgument, "invalid transcription options");
    }

    // ---- Identity and cache ----
    auto identity = config.identify(request);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    std::map<std::string, std::string> params{
        {"language", options.language},
        {"beamSize", std::to_string(options.beamSize)},
        {"vad", options.vad ? std::string("silero@") + vadModel->sha256.substr(0, 16) +
                                  (options.vadInsideChunks ? "+chunks" : "")
                            : std::string("off")},
        {"initialPrompt", options.initialPrompt},
        {"maxChunkSamples", std::to_string(options.chunking.maxChunkSamples)},
        {"pipeline", std::string(kPipelineVersion)},
    };
    if (!request.silence.empty()) {
        params["redacted"] = rangesKey(request.silence); // review P1-3: silenced audio is another transcript
    }
    const AnalysisKey cacheKey{identity->fingerprint, identity->streamIndex, "transcript",
                               "whisper/" + model->id + "@" + model->sha256.substr(0, 16), hashParams(params)};
    std::optional<AnalysisCache> cache;
    if (!config.cacheRoot.empty()) {
        cache.emplace(config.cacheRoot);
    }
    if (cache && options.useCache && !options.refresh) {
        if (auto hit = cache->get(cacheKey); hit && *hit) {
            if (auto cached = transcriptFromJson(**hit)) {
                cached->source.path = request.mediaPath;
                report(1.0f);
                if (request.onStats) {
                    stats.fromCache = true;
                    stats.device = cached->provenance.params.contains("device") ? cached->provenance.params.at("device") : "";
                    stats.totalMs = msSince(jobStart);
                    request.onStats(stats);
                }
                return cached;
            }
        }
    }

    // ---- Models (download on first use) ----
    ModelStore store(config.modelRoot);
    auto modelPath = store.ensure(*model, config.download, {}, stop);
    if (!modelPath) {
        return std::unexpected(modelPath.error());
    }
    std::optional<std::filesystem::path> vadPath;
    if (vadModel != nullptr) {
        auto path = store.ensure(*vadModel, config.download, {}, stop);
        if (!path) {
            return std::unexpected(path.error());
        }
        vadPath = *path;
    }
    report(0.02f);

    // ---- PCM ----
    auto workspace = detail::TempWorkspace::create(config.workDir);
    if (!workspace) {
        return std::unexpected(workspace.error());
    }
    TempFile pcm{(*workspace)->path() / "audio.f32"};
    const auto extractStart = Clock::now();
    auto extracted = config.extract(request, *identity, pcm.path, stop, [&](float p) { report(0.02f + 0.13f * p); });
    if (!extracted) {
        return std::unexpected(extracted.error());
    }
    stats.extractMs = msSince(extractStart);
    stats.mediaSeconds = static_cast<double>(extracted->samples) / kTranscriptionSampleRate;
    if (auto silenced = silencePcmRanges(pcm.path, extracted->samples, request.silence); !silenced) {
        return std::unexpected(silenced.error()); // never transcribe redacted speech
    }
    report(0.15f);

    // ---- Helper ----
    const auto loadStart = Clock::now();
    ChildProcessOptions childOptions;
    childOptions.executable = config.inferExecutable;
    childOptions.arguments = config.helperArguments;
    childOptions.stderrFile = config.helperLog;
    childOptions.aiHelper = true; // 0.2: below normal and the helper policy's CPU cap (Settings > AI)
    auto child = ChildProcess::spawn(childOptions);
    if (!child) {
        return std::unexpected(child.error());
    }
    HelperSession helper(std::move(*child), stop, config.inactivityTimeout);
    int nextId = 1;
    if (!helper.send(infer::Hello{})) {
        return fail(ErrorCode::DecoderError, "wabcut-infer exited at start");
    }
    auto hello = helper.expect<infer::HelloReply>("", "starting", config.helperStartTimeout);
    if (!hello) {
        return std::unexpected(hello.error());
    }
    if (hello->protocol != infer::kProtocolVersion) {
        return fail(ErrorCode::VersionMismatch, "wabcut-infer speaks protocol " + std::to_string(hello->protocol) +
                                                    ", expected " + std::to_string(infer::kProtocolVersion));
    }
    const std::string loadId = makeId(nextId++);
    helper.send(infer::LoadModel{loadId, *modelPath, options.useGpu, 0});
    auto loaded = helper.expect<infer::Loaded>(loadId, "loading the model", config.modelLoadTimeout);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    if (vadPath) {
        const std::string vadId = makeId(nextId++);
        helper.send(infer::LoadVad{vadId, *vadPath});
        auto vadLoaded = helper.expect<infer::Loaded>(vadId, "loading the VAD model", config.modelLoadTimeout);
        if (!vadLoaded) {
            return std::unexpected(vadLoaded.error());
        }
    }

    stats.loadMs = msSince(loadStart);
    stats.device = loaded->device;

    // ---- Chunks ----
    const auto vadStart = Clock::now();
    const std::int64_t total = extracted->samples;
    std::vector<SampleRange> chunks;
    if (vadPath) {
        std::vector<SampleRange> speech;
        constexpr std::int64_t kBlock = 600LL * kTranscriptionSampleRate; // VAD 10 min at a time (bounded memory)
        for (std::int64_t begin = 0; begin < total; begin += kBlock) {
            const std::int64_t count = std::min(kBlock, total - begin);
            const std::string id = makeId(nextId++);
            helper.send(infer::VadRequest{id, {pcm.path, begin, count}});
            auto vad = helper.expect<infer::VadResult>(id, "detecting speech", config.inactivityTimeout);
            if (!vad) {
                return std::unexpected(vad.error());
            }
            const auto found = speechFromCentiseconds(vad->segments, begin, count);
            speech.insert(speech.end(), found.begin(), found.end());
            report(0.15f + 0.05f * static_cast<float>(begin + count) / static_cast<float>(std::max<std::int64_t>(total, 1)));
        }
        chunks = planChunks(speech, total, options.chunking);
    } else {
        chunks = fixedChunks(total, options.chunking);
    }
    report(0.2f);
    stats.vadMs = msSince(vadStart);

    // ---- Transcription ----
    Transcript transcript;
    transcript.language = options.language == "auto" ? std::string{} : options.language;
    std::string language = options.language;
    std::int64_t totalChunkSamples = 0;
    for (const SampleRange& c : chunks) {
        totalChunkSamples += c.length();
    }
    std::int64_t doneSamples = 0;
    stats.speechSeconds = static_cast<double>(totalChunkSamples) / kTranscriptionSampleRate;
    for (const SampleRange& chunk : chunks) {
        const std::string id = makeId(nextId++);
        infer::TranscribeRequest t;
        t.id = id;
        t.pcm = {pcm.path, chunk.begin, chunk.length()};
        t.language = language;
        t.threads = options.threads > 0 ? options.threads : automaticThreads();
        t.beamSize = options.beamSize;
        t.vad = vadPath.has_value() && options.vadInsideChunks;
        t.initialPrompt = options.initialPrompt;
        if (!helper.send(t)) {
            return fail(ErrorCode::DecoderError, "wabcut-infer exited before transcribing");
        }
        const auto chunkDeadline = std::chrono::milliseconds(std::chrono::hours(6));
        for (;;) {
            auto response = helper.next("transcribing", chunkDeadline);
            if (!response) {
                return std::unexpected(response.error());
            }
            if (infer::responseId(*response) != id) {
                return fail(ErrorCode::DecoderError, "wabcut-infer answered out of order while transcribing");
            }
            if (const auto* p = std::get_if<infer::Progress>(&*response)) {
                const double fraction = static_cast<double>(doneSamples) +
                                        static_cast<double>(chunk.length()) * std::clamp(p->percent, 0, 100) / 100.0;
                report(0.2f + 0.8f * static_cast<float>(fraction / static_cast<double>(std::max<std::int64_t>(totalChunkSamples, 1))));
                continue;
            }
            if (const auto* s = std::get_if<infer::Segment>(&*response)) {
                if (transcript.language.empty() && !s->language.empty()) {
                    transcript.language = s->language;
                }
                const ChunkResult piece{chunk, {s->segment}};
                if (auto stitched = stitchChunks(std::span(&piece, 1), extracted->origin,
                                                 wordSplitFor(transcript.language), transcript);
                    !stitched) {
                    return std::unexpected(stitched.error());
                }
                if (request.onSegment && !transcript.segments.empty()) {
                    request.onSegment(transcript);
                }
                continue;
            }
            if (const auto* r = std::get_if<infer::Result>(&*response)) {
                if (transcript.language.empty()) {
                    transcript.language = r->language;
                }
                if (language == "auto" && !r->language.empty()) {
                    language = r->language; // keep the detected language for the remaining chunks
                }
                if (const auto wall = r->stats.find("wallMs"); wall != r->stats.end()) {
                    stats.recognizeMs += wall->second;
                }
                break;
            }
            if (const auto* f = std::get_if<infer::Failure>(&*response)) {
                return fail(codeForHelperError(f->code), "wabcut-infer: transcribing failed: " + f->message);
            }
            return fail(ErrorCode::DecoderError, "wabcut-infer sent an unexpected message while transcribing");
        }
        doneSamples += chunk.length();
    }

    // ---- Result ----
    transcript.source = {identity->fingerprint, identity->streamIndex, request.mediaPath};
    transcript.provenance.engine = hello->engine;
    transcript.provenance.engineVersion = hello->engineVersion;
    transcript.provenance.modelId = model->id;
    transcript.provenance.modelVersion = model->sha256;
    transcript.provenance.params = params;
    transcript.provenance.params["device"] = loaded->device;
    transcript.provenance.createdAt = detail::utcNowIso8601();
    if (transcript.language.empty()) {
        transcript.language = language == "auto" ? std::string("und") : language;
    }
    if (auto valid = transcript.validate(); !valid) {
        return std::unexpected(valid.error());
    }
    if (cache && options.useCache) {
        (void)cache->put(cacheKey, toJson(transcript)); // a cache failure never fails the job
    }
    report(1.0f);
    if (request.onStats) {
        stats.totalMs = msSince(jobStart);
        request.onStats(stats);
    }
    return transcript;
}

} // namespace wabcut::ai
