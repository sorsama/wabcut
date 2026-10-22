#pragma once

// Capability interfaces (docs/ai.md, "Capabilities"): one interface per kind of work, provider-agnostic. Cloud
// adapters live in wabcut_ai_net, local engines in ai_local; ProviderRegistry (Provider.h) creates them.
//
// Every call is synchronous and cancellable: it blocks the calling job thread (AiJob.h runs it in the background)
// until the result is ready, the stop token fires (AiErrorCode::Cancelled) or the provider fails. Calls never
// throw. Results carry Usage so SpendGuard can settle the actual cost.

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Chat.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Transcript.h>
#include <wabcut/model/Time.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

/// Progress in [0, 1].
using ProgressFn = std::function<void(float)>;

// --- Chat -----------------------------------------------------------------------------------------------------

class IChatModel {
public:
    virtual ~IChatModel() = default;
    virtual const ModelSpec& spec() const = 0;

    /// Runs one model turn. When `onEvent` is set the response streams (where the model supports it) and the
    /// handler sees text and tool-call deltas as they arrive; the returned ChatResponse is the same either way.
    /// Errors: everything in AiErrorCode that applies, notably Unsupported (VideoPart on a model without native
    /// video, tools on an API that cannot carry them) and InvalidRequest (a tool spec outside the strict subset).
    virtual std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                                      std::stop_token stop) = 0;
};

// --- Transcription --------------------------------------------------------------------------------------------

/// A request to a cloud transcriber (local transcription is TranscriptionService, TranscriptionService.h).
struct TranscribeRequest {
    std::filesystem::path audioFile;   ///< FFmpeg-extracted mono FLAC/Opus/WAV
    std::string mediaType = "audio/flac";
    std::string language;              ///< ISO 639-1 hint; empty = detect
    bool diarize = true;
    std::optional<int> speakerCount;   ///< hint, when known
    std::vector<std::string> keyterms; ///< vocabulary hints, where the provider supports them
    std::int64_t audioMilliseconds = 0; ///< duration (cost estimate, spend cap, limit checks); 0 = unknown
    TranscriptSource source;           ///< copied into the Transcript
    Ticks origin{};                    ///< source time of the audio file's first sample (added to every time)
};

/// A cloud transcription as a regular Transcript (Transcript.h), normalised at the adapter boundary: every
/// provider's time unit (seconds, milliseconds) becomes source Ticks, speaker labels become 0, 1, ... in order of
/// first appearance, segments are the provider's utterances (or runs of one speaker), and provenance names the
/// provider ("engine") and the model that actually ran. The transcript passes Transcript::validate().
struct TranscribeResult {
    Transcript transcript;
    Ticks duration{};      ///< audio length as the provider measured it
    Usage usage;           ///< audioMilliseconds (what the provider bills)
    std::string requestId; ///< provider request id (support requests)
};

class ITranscriber {
public:
    virtual ~ITranscriber() = default;
    virtual const ModelSpec& spec() const = 0;
    virtual std::expected<TranscribeResult, AiError> transcribe(const TranscribeRequest& request,
                                                                const ProgressFn& progress, std::stop_token stop) = 0;
};

// --- Embeddings -----------------------------------------------------------------------------------------------

struct EmbeddingRequest {
    std::vector<std::string> texts;
    std::vector<ImagePart> images;
    std::optional<int> dimensions; ///< truncated/Matryoshka output size, where supported
};

struct EmbeddingResult {
    std::vector<std::vector<float>> vectors; ///< texts first, then images, in request order
    Usage usage;
    std::string model;
};

class IEmbedder {
public:
    virtual ~IEmbedder() = default;
    virtual const ModelSpec& spec() const = 0;
    virtual std::expected<EmbeddingResult, AiError> embed(const EmbeddingRequest& request, std::stop_token stop) = 0;
};

// --- Speech synthesis -----------------------------------------------------------------------------------------

struct SpeechRequest {
    std::string text;
    std::string voiceId;
    std::string language;               ///< ISO 639-1; empty = the voice's default
    std::string outputFormat = "mp3_44100_128";
    bool withTimestamps = true;         ///< character alignment, for captioning voice-over
    std::string style;                  ///< delivery instructions ("warm, unhurried") where the model takes them
};

struct SpokenCharacter {
    std::string text; ///< one UTF-8 character
    Ticks t0{};
    Ticks t1{};
};

struct SpeechResult {
    std::vector<std::uint8_t> audio;
    std::string mediaType;              ///< "audio/mpeg", ...
    std::vector<SpokenCharacter> alignment;
    Usage usage;                        ///< characters
};

/// A voice made from recordings of a speaker (instant voice cloning, AI-12).
struct ClonedVoice {
    std::string voiceId;
    bool requiresVerification = false; ///< the provider wants the speaker to verify before the voice can be used
};

class ISpeechSynth {
public:
    virtual ~ISpeechSynth() = default;
    virtual const ModelSpec& spec() const = 0;
    virtual std::expected<SpeechResult, AiError> synthesize(const SpeechRequest& request, std::stop_token stop) = 0;
    /// Instant voice cloning (AI-12, opt-in): a voice made from `samples`, recordings of ONE speaker. Callers only ask
    /// after the user confirmed they have that speaker's permission, and never for a speaker with redacted speech.
    virtual std::expected<ClonedVoice, AiError> cloneVoice(std::span<const std::filesystem::path> samples, std::string_view name,
                                                           std::stop_token stop) {
        (void)samples, (void)name, (void)stop;
        return std::unexpected(AiError(AiErrorCode::Unsupported, spec().name + " cannot clone voices"));
    }
    /// Removes a voice cloneVoice made (a clone is not left on the user's account after the run).
    virtual std::expected<void, AiError> deleteVoice(std::string_view voiceId, std::stop_token stop) {
        (void)voiceId, (void)stop;
        return std::unexpected(AiError(AiErrorCode::Unsupported, spec().name + " cannot clone voices"));
    }
};

// --- Translation ----------------------------------------------------------------------------------------------

struct TranslationSegment {
    std::string id;   ///< caller's id (a caption cue id); returned unchanged
    std::string text;

    bool operator==(const TranslationSegment&) const = default;
};

struct TranslationRequest {
    std::string sourceLanguage; ///< ISO 639-1; empty = detect
    std::string targetLanguage; ///< ISO 639-1
    std::vector<TranslationSegment> segments;
    std::string context;        ///< what the text is (e.g. "captions of a cooking video"), glossary, tone
    std::optional<int> maxCharactersPerSegment; ///< keep lines short enough for captions/dubbing
};

struct TranslationResult {
    std::vector<TranslationSegment> segments; ///< same ids, same order as the request
    Usage usage;
};

class ITranslator {
public:
    virtual ~ITranslator() = default;
    virtual std::expected<TranslationResult, AiError> translate(const TranslationRequest& request,
                                                                std::stop_token stop) = 0;
};

// --- Media generation (asynchronous on the provider: create, poll, fetch) -------------------------------------

/// SoundEffect (0.2): a sound effect from a text prompt (ElevenLabs sound generation, Premiere's Generative Media).
enum class GeneratedMediaKind { Video, Image, SoundEffect };

std::string_view toString(GeneratedMediaKind kind);

/// A generation job (AI-10, docs/ai.md "Generative media"). Images given as conditioning are local files (PNG or
/// JPEG) the adapter sends inline; nothing else of the project leaves this machine.
struct MediaGenerationRequest {
    GeneratedMediaKind kind = GeneratedMediaKind::Video;
    std::string prompt;
    std::string negativePrompt;       ///< where the model supports one
    std::filesystem::path firstFrame; ///< optional image the video starts from (image-to-video)
    std::filesystem::path lastFrame;  ///< optional image the video ends on (models with lastFrame support)
    std::filesystem::path inputClip;  ///< optional clip to extend or edit (models that accept one)
    std::optional<Ticks> duration;    ///< wanted length; adapters use the model's next allowed duration (>=)
    std::string aspectRatio = "16:9"; ///< "16:9", "9:16", "1:1", ...
    std::string resolution = "720p";  ///< video: "720p", "1080p", "4k"; images: a size or tier, e.g. "1024x1024", "2K"
    bool audio = false;               ///< ask for generated sound where the model makes it (often a pricier tier)
    int count = 1;                    ///< outputs per request (images; video models usually allow 1)
    std::optional<std::int64_t> seed;
    /// Sound effects (0.2): a seamless loop where the model makes one; how closely to follow the prompt (0..1; the
    /// model's default when unset). `duration` unset = the model picks the length from the prompt.
    bool loop = false;
    std::optional<double> promptInfluence;
    Json extra = Json::object();      ///< provider-specific options
};

struct GenerationTicket {
    std::string provider;
    std::string model;
    std::string id;       ///< provider task id / operation name
    std::string pollUrl;  ///< when the provider returns one
    std::string cancelUrl;
    std::string resultUrl;

    bool operator==(const GenerationTicket&) const = default;
};

enum class GenerationState { Queued, Running, Succeeded, Failed, Cancelled };

std::string_view toString(GenerationState state);

struct GenerationStatus {
    GenerationState state = GenerationState::Queued;
    float progress = 0.0f;               ///< 0..1 where the provider reports it; else 0
    std::vector<std::string> outputUrls; ///< Succeeded: expiring URLs, fetch them right away
    /// Succeeded with the output inline (e.g. images as base64): decoded bytes and their media type.
    std::vector<std::vector<std::uint8_t>> inlineOutputs;
    std::vector<std::string> inlineTypes;
    std::string failure;                 ///< Failed: the provider's reason (redacted)
    std::string failureCode;             ///< e.g. "content_policy", "quota"
    /// The provider does not bill this failure (documented for failed / filtered jobs); otherwise a failure after
    /// the job was accepted settles its estimate (docs/ai.md "Security", spend accounting).
    bool unbilled = false;
    std::optional<std::chrono::milliseconds> pollAfter; ///< the provider's hint for the next poll
    Usage usage;                         ///< what the provider billed or will bill, when it says
};

class IMediaGenerator {
public:
    virtual ~IMediaGenerator() = default;
    virtual const ModelSpec& spec() const = 0;
    /// Submits the job. Images (no queue) may finish at once: then poll() returns Succeeded right away.
    virtual std::expected<GenerationTicket, AiError> create(const MediaGenerationRequest& request,
                                                            std::stop_token stop) = 0;
    virtual std::expected<GenerationStatus, AiError> poll(const GenerationTicket& ticket, std::stop_token stop) = 0;
    /// Downloads every output into `directory`; returns the files in output order. Only HTTPS URLs on the
    /// provider's documented download hosts are fetched (the key goes only to the provider's own host).
    virtual std::expected<std::vector<std::filesystem::path>, AiError>
    fetch(const GenerationStatus& status, const std::filesystem::path& directory, std::stop_token stop) = 0;
    /// Asks the provider to stop a queued or running job (best effort). Unsupported where the API has no cancel.
    virtual std::expected<void, AiError> cancel(const GenerationTicket& ticket, std::stop_token stop) {
        (void)ticket;
        (void)stop;
        return std::unexpected(AiError(AiErrorCode::Unsupported, "this provider cannot cancel a running job"));
    }
};

} // namespace wabcut::ai
