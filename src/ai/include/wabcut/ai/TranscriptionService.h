#pragma once

// Local transcription (docs/ai.md feature 1): media -> 16 kHz mono PCM (media::AudioDecoder) -> VAD chunks ->
// whisper.cpp in the out-of-process wabcut-infer helper -> a Transcript in source time, cached per source.
//
// Each job runs on its own thread and is a media::Job: cancellable, with progress. The helper is started per
// job and killed on cancel, timeout or crash, so a misbehaving model or GPU driver ends that job with a clean
// Error and never touches the editor's process.

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Transcript.h>
#include <wabcut/ai/VadChunker.h>

#include <wabcut/media/Job.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>

namespace wabcut::ai {

struct TranscriptionOptions {
    std::string model = "base";    ///< ModelStore id or alias ("tiny", "base", "large-v3-turbo")
    std::string language = "auto"; ///< ISO 639-1 ("en", "th", ...) or "auto" (detected once, then kept; see mixedLanguages)
    bool useGpu = true;            ///< let the helper use a GPU backend (Vulkan, ...) when one is present
    bool vad = true;               ///< skip silence with Silero VAD (downloads the 0.9 MB model on first use)
    std::string vadModel = "silero"; ///< ModelStore id of the VAD model
    /// Also run whisper's own VAD inside each chunk (drops the shorter pauses too). Off by default: in
    /// whisper.cpp 1.9.5 its time remapping can collapse a segment and repeat text (seen on Thai speech), and
    /// chunking already skips every silence longer than ChunkOptions::maxGapSamples.
    bool vadInsideChunks = false;
    int threads = 0;               ///< CPU threads for the helper; 0 = automatic
    int beamSize = 5;              ///< 1 = greedy decoding (faster, slightly worse)
    std::string initialPrompt;     ///< vocabulary hint (names, jargon)
    /// Whisper's translate task: the words come out in English, whatever the language spoken (card 073). Not for the
    /// English-only models. The transcript's language is then "en"; a translated transcript is cached apart from the
    /// transcription of the same audio.
    bool translate = false;
    /// 0.7 (card 111): the recording switches between languages (an interview with a translator, a streamer who changes
    /// language): with language "auto" every stretch of speech is transcribed in the language it is spoken in, instead of
    /// the language of the first one for the whole file. The audio is cut at pauses into short chunks (about a sentence
    /// each, ChunkOptions::closeAfterSamples) and the recogniser names the language of each; every segment of the
    /// transcript carries its own (TranscriptSegment::language), the transcript's is the one most words are in. A
    /// language that changes in the middle of a sentence stays in the language the recogniser heard first. Ignored with a
    /// named language or translate; needs a multilingual model (InvalidArgument for ".en" ones). A transcript of mixed
    /// languages is cached apart from one of a single language.
    bool mixedLanguages = false;
    bool useCache = true;          ///< reuse / store the result in the AnalysisCache
    bool refresh = false;          ///< with useCache: transcribe anyway, then store (replaces the entry)
    ChunkOptions chunking;
};

/// Where the time of one job went (for diagnostics, benchmarks and the CLI's --timing).
struct TranscriptionStats {
    bool fromCache = false;
    std::string device;         ///< what the model ran on, as the helper reports it
    double mediaSeconds = 0.0;  ///< extracted audio length
    double speechSeconds = 0.0; ///< audio actually sent to the recogniser (VAD chunks)
    double extractMs = 0.0;     ///< decoding the media to 16 kHz PCM
    double loadMs = 0.0;        ///< starting the helper and loading the models
    double vadMs = 0.0;         ///< the VAD pass
    double recognizeMs = 0.0;   ///< whisper time (sum over chunks, measured by the helper)
    double totalMs = 0.0;       ///< the whole job
};

struct TranscriptionRequest {
    std::string mediaPath; ///< UTF-8
    int streamIndex = -1;  ///< container index of the audio stream; -1 = the best one
    TranscriptionOptions options;
    /// Media times silenced before the helper hears anything (the project's audio redactions, audioRedactionRanges):
    /// redacted speech is never recognised. Part of the cache key.
    std::vector<TimeRange> silence;
    /// Called on the job's thread for every recognised segment as soon as it is decoded, with the transcript so
    /// far (its last segment is the new one). Source times, final values.
    std::function<void(const Transcript& sofar)> onSegment;
    /// Called once at the end of a successful job.
    std::function<void(const TranscriptionStats&)> onStats;
};

/// Identity of the audio to transcribe (cache key) and the extracted PCM (test seams of the service).
struct AudioIdentity {
    std::uint64_t fingerprint = 0;
    int streamIndex = -1;
    Ticks duration{}; ///< 0 if unknown
};
struct ExtractedAudio {
    std::int64_t samples = 0; ///< 16 kHz mono float32 samples written
    Ticks origin{};           ///< source time of sample 0
};

struct TranscriptionServiceConfig {
    /// The helper; defaultInferExecutable() when empty.
    std::filesystem::path inferExecutable;
    std::filesystem::path modelRoot = ModelStore::defaultRoot();
    /// Extra model manifest entries (custom ggml models, tests), looked up by id before the built-in ones.
    std::vector<ModelInfo> models;
    /// Analysis cache root; empty disables caching.
    std::filesystem::path cacheRoot;
    /// Temporary PCM files; <temp>/wabcut-transcribe when empty.
    std::filesystem::path workDir;
    /// When set, missing models are downloaded on first use; otherwise a missing model is a NotFound error.
    IDownloadTransport* download = nullptr;
    /// Where the helper's stderr (whisper.cpp's log) goes; empty = discarded.
    std::filesystem::path helperLog;
    /// Extra helper arguments (tests use them to script a fake helper).
    std::vector<std::string> helperArguments;

    std::chrono::milliseconds helperStartTimeout{std::chrono::seconds(20)};
    std::chrono::milliseconds modelLoadTimeout{std::chrono::seconds(180)};
    /// Longest the helper may stay silent while it works (it reports progress every few seconds).
    std::chrono::milliseconds inactivityTimeout{std::chrono::seconds(120)};

    /// Default: media::sourceFingerprint plus the decoder's choice of stream.
    std::function<std::expected<AudioIdentity, Error>(const TranscriptionRequest&)> identify;
    /// Default: media::AudioDecoder at 16 kHz mono into `pcmFile` (raw float32). `progress` takes 0..1.
    std::function<std::expected<ExtractedAudio, Error>(const TranscriptionRequest&, const AudioIdentity&,
                                                       const std::filesystem::path& pcmFile, std::stop_token,
                                                       const std::function<void(float)>& progress)>
        extract;
};

/// The wabcut-infer helper: $WABCUT_INFER_PATH when set (packagers, tests), else the one next to the running
/// executable.
std::filesystem::path defaultInferExecutable();

/// 0.7 (card 061): the longest vocabulary prompt (bytes of UTF-8). Whisper reads at most 224 tokens of prompt (half
/// its text context); names and made-up words take 2 to 4 tokens each, so this keeps well inside it.
inline constexpr std::size_t kMaxVocabularyPromptBytes = 480;
/// The vocabulary hint whisper takes as `initialPrompt` for the user's custom words: "Vocabulary: Elden Ring, Malenia,
/// Radahn." Empty without words. Words are used in order; the ones that no longer fit in kMaxVocabularyPromptBytes
/// are left out (`used`, when given, gets how many were kept).
std::string vocabularyPrompt(std::span<const std::string> words, std::size_t* used = nullptr);

/// Zeroes the samples of `ranges` (media time; sample 0 = media time 0) in a raw 16 kHz mono float32 file of
/// `samples` samples. Errors: IoError.
std::expected<void, Error> silencePcmRanges(const std::filesystem::path& pcmFile, std::int64_t samples,
                                            std::span<const TimeRange> ranges);
/// A stable key of `ranges` for analysis-cache parameters ("" when empty).
std::string rangesKey(std::span<const TimeRange> ranges);

/// The default identify / extract seams: media::sourceFingerprint plus the decoder's choice of stream; the stream
/// at 16 kHz mono into a raw float32 file. Shared with TranscribeService and the energy probe.
std::expected<AudioIdentity, Error> identifyMediaAudio(const TranscriptionRequest& request);
std::expected<ExtractedAudio, Error> extractMediaAudio(const TranscriptionRequest& request, const AudioIdentity& identity,
                                                       const std::filesystem::path& pcmFile, std::stop_token stop,
                                                       const std::function<void(float)>& progress);

class TranscriptionService {
public:
    explicit TranscriptionService(TranscriptionServiceConfig config = {});
    ~TranscriptionService(); ///< cancels running jobs and waits for them

    TranscriptionService(const TranscriptionService&) = delete;
    TranscriptionService& operator=(const TranscriptionService&) = delete;

    /// Starts a transcription job. Errors (in the job): NotFound (media, stream, helper or a model that is not
    /// downloaded), InvalidArgument (unknown model or options), DecoderError (the helper failed or crashed),
    /// IoError, Cancelled.
    media::Job<Transcript> transcribe(TranscriptionRequest request);

    /// The same, synchronously on the calling thread. `progress` takes 0..1.
    std::expected<Transcript, Error> run(const TranscriptionRequest& request, std::stop_token stop = {},
                                         const std::function<void(float)>& progress = {});

    /// Speech in a raw 16 kHz mono float32 PCM file (`samples` long) by Silero VAD in the helper, as sample ranges.
    /// Errors: NotFound (no helper, or the VAD model is not downloaded and no download transport is set),
    /// DecoderError, Cancelled.
    std::expected<std::vector<SampleRange>, Error> detectSpeech(const std::filesystem::path& pcmFile, std::int64_t samples,
                                                                std::stop_token stop = {}, std::string vadModel = "silero");

    /// The analysis-cache key of a request (after identify()); for tools and tests.
    static std::string analyzerId() { return "transcript"; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::ai
