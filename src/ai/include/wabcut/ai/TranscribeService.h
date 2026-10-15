#pragma once

// TranscribeService: THE way to turn a media file's audio into a Transcript (full builds; it decodes media), shared by
// the app's Transcript panel, the transcript.transcribe command (wabcut-cli, MCP, the AI assistant) and
// `wabcut-cli transcribe`.
//
//   source -> the analysis cache (any transcript of this audio, any provider) -> a provider:
//     "local"          whisper.cpp in the wabcut-infer helper (TranscriptionService: 16 kHz extraction, Silero VAD
//                      chunks, cancel/timeout/crash isolation), when the helper was built (WABCUT_WITH_LOCAL_AI);
//     "cloud:<model>"  a cloud transcription model through the AI platform (AiPlatform::transcriber): the offline and
//                      project no-cloud locks, consent, the monthly spend cap and the request log all apply. The
//                      audio goes up as 16 kHz mono 16-bit WAV, cut at silences into chunks within the model's limits
//                      (maxAudioSeconds, maxUploadBytes) and stitched back in source time;
//     "auto"           local when available, else the user's preferred cloud transcription model;
//     any id added with addProvider() (tests, hosts with their own engine).
//   -> the result is stored in the analysis cache under the source's fingerprint, where CachedTranscriptProvider (and so
//      every transcript command) finds it.
//
// Jobs run on their own threads as media::Job: progress 0..1, cancel (a cloud upload or the helper is stopped).

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/Transcript.h>
#include <wabcut/ai/TranscriptRedaction.h>
#include <wabcut/ai/TranscriptionService.h>

#include <wabcut/media/Job.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

class AiPlatform;

/// One audio stream of a media file.
struct TranscribeSource {
    std::string mediaPath;    ///< UTF-8
    int containerStream = -1; ///< container index of the audio stream; -1 = the best one
    /// Media times whose sound the project redacts (audioRedactionRanges, TranscriptRedaction.h). They are silenced
    /// before any transcriber hears the audio - nothing of them is uploaded - and every result, cached ones too, has
    /// the words there replaced by kRedactedWord. Hosts with a project must pass them.
    std::vector<TimeRange> redacted;
};

struct TranscribeJobOptions {
    /// "auto", "local", "cloud:<model id>" or an id added with addProvider().
    std::string provider = "auto";
    std::string language = "auto";   ///< ISO 639-1 hint ("en", "th", ...) or "auto"
    /// The local provider's whisper settings (model "base" by default; language and caching come from above).
    TranscriptionOptions local;
    /// Return a transcript of this audio already in the analysis cache (from any provider) instead of transcribing.
    /// The result is stored either way (when the service has a cache), so false re-transcribes and replaces it.
    bool useCache = true;
    /// The user answered the cloud consent prompt with "Allow once" for this job (AiPlatform CallOptions). Never
    /// overrides the offline mode or the project's no-cloud lock.
    bool allowOnce = false;
    /// Who asked (CallOptions::caller): an extension's job needs allowOnce whatever consent is recorded.
    std::string caller;
};

struct TranscribeOutcome {
    Transcript transcript;
    std::string provider;  ///< what ran: "local", "cloud:<model>", a custom id; for a cache hit, what made it
    bool fromCache = false;
    std::string device;    ///< local: what whisper ran on, as the helper reports it ("Vulkan0", "CPU")
    TranscriptionStats localStats; ///< local runs: where the time went
    /// For the user, when something fell back: local transcription ran on the CPU because no Vulkan GPU was found.
    std::string note;
};

/// The note for a local run on `device`: empty on a GPU, else that no Vulkan GPU was usable (no driver, an
/// unsupported one, or a build without the Vulkan backend) so whisper ran on the CPU.
std::string cpuFallbackNote(std::string_view device);

struct TranscribeProviderInfo {
    std::string id;     ///< what TranscribeJobOptions::provider takes
    std::string label;
    std::string detail;
    bool available = false;
    std::string reason; ///< why not, when unavailable
};

/// A transcription engine plugged in under an id. Gets the source and options; reports progress 0..1.
using CustomTranscriber = std::function<std::expected<Transcript, Error>(
    const TranscribeSource& source, const TranscribeJobOptions& options, const std::function<void(float)>& progress,
    std::stop_token stop)>;

struct TranscribeServiceConfig {
    /// The analysis cache; empty = no cache reads or writes.
    std::filesystem::path cacheRoot = AnalysisCache::defaultRoot();
    /// Cloud transcription (guarded); null = no cloud providers. Must outlive the service.
    AiPlatform* platform = nullptr;
    /// The local engine (inferExecutable empty = defaultInferExecutable()); its cacheRoot is set to ours.
    TranscriptionServiceConfig local;
    /// Upload WAVs; <temp>/wabcut-transcribe when empty.
    std::filesystem::path workDir;
    /// Longest audio one cloud request carries (the model's own limits apply too); longer audio is cut at silences.
    Ticks cloudChunk = Ticks::fromSeconds(600);
};

class TranscribeService {
public:
    explicit TranscribeService(TranscribeServiceConfig config = {});
    ~TranscribeService(); ///< cancels running jobs and waits for them
    TranscribeService(const TranscribeService&) = delete;
    TranscribeService& operator=(const TranscribeService&) = delete;

    /// Adds (or replaces) a provider under `id`.
    void addProvider(std::string id, std::string label, CustomTranscriber transcriber);

    /// Every provider with availability: local, the cloud transcription models the platform can use, the added ones.
    std::vector<TranscribeProviderInfo> providers() const;
    /// What `requested` runs as ("auto" resolved), or why nothing can (NotFound, with how to set one up).
    std::expected<std::string, Error> resolveProvider(std::string_view requested) const;

    /// A transcript of this audio in the analysis cache (any provider), or null.
    std::shared_ptr<const Transcript> cached(const TranscribeSource& source) const;

    /// Starts a job. Errors (in the job): NotFound (media, stream, provider, helper, model), InvalidArgument, IoError,
    /// DecoderError, Unsupported (a cloud refusal: the message names the AI error, e.g. "ConsentRequired",
    /// "Offline", "SpendCapExceeded", "NoCredentials"), Cancelled.
    media::Job<TranscribeOutcome> transcribe(TranscribeSource source, TranscribeJobOptions options);
    /// The same, synchronously on the calling thread.
    std::expected<TranscribeOutcome, Error> run(const TranscribeSource& source, const TranscribeJobOptions& options,
                                                std::stop_token stop = {}, const std::function<void(float)>& progress = {});

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// 16 kHz mono 16-bit PCM WAV of [first, first + count) samples of a raw 16 kHz float32 file (what cloud
/// transcribers take); returns the samples written. Exposed for tests.
std::expected<std::int64_t, Error> writeWav16k(const std::filesystem::path& pcm, std::int64_t first, std::int64_t count,
                                               const std::filesystem::path& wav);

/// Speech in a raw 16 kHz mono float32 file from its level (10 ms RMS frames well above the noise floor): what cloud
/// uploads are cut along when the local VAD is not built. Sample ranges, sorted.
std::vector<SampleRange> energySpeech(const std::filesystem::path& pcm, std::int64_t samples);

} // namespace wabcut::ai
