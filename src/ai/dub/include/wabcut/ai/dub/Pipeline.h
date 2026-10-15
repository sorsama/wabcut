#pragma once

// Running a dub or a narration (AI-12, docs/ai.md "Going global"): every line through the user's voice provider (one
// guarded, billed request each), the speech decoded and its leading and trailing silence trimmed, the lines fitted to
// their slots (TimeFit.h; narration first laid out, Dub.h), sped up where needed, and written as ONE generated audio
// file with provenance (kind "audio", operation "dub" or "narration"). Nothing is placed: assembleDub() makes the edit.
//
// The run keeps its own hard cap: before each line it adds that line's worst-case cost (estimateSpeechCost) to what
// the lines before were estimated at, and stops (SpendCapExceeded) before sending a line that could go past `budget`.
// A voice cloned for the run (opt-in) is deleted from the provider's account when the run ends, also on failure.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/dub/Dub.h>
#include <wabcut/ai/dub/TimeFit.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::dub {

enum class DubKind { Dub, Narration };

/// Decoding, stretching and writing speech. wabcut_ai_dub_media has the real one (FFmpeg's decoder, the audio engine's
/// pitch-preserving WSOLA stretcher, float WAV files); tests bring their own.
class SpeechAudio {
public:
    virtual ~SpeechAudio() = default;
    /// The speech as mono samples at `rate`.
    virtual std::expected<std::vector<float>, Error> decode(const SpeechResult& speech, int rate) = 0;
    /// `samples` played `speed` times as fast (speed > 1), pitch kept.
    virtual std::vector<float> stretch(std::span<const float> samples, int rate, double speed) = 0;
    /// Writes a mono float WAV and describes it as a media item (name, locator, info; no id).
    virtual std::expected<MediaItem, Error> write(const std::filesystem::path& file, std::span<const float> samples, int rate) = 0;
    /// A recording of one speaker for voice cloning: `ranges` (media time) of `media`'s audio stream `stream`, joined,
    /// as a WAV file.
    virtual std::expected<std::filesystem::path, Error> extractSample(const MediaItem& media, int stream,
                                                                      std::span<const TimeRange> ranges,
                                                                      const std::filesystem::path& file) = 0;
};

struct DubJob {
    DubKind kind = DubKind::Dub;
    std::vector<DubLine> lines;
    SpeechRequest voice;                 ///< voiceId, language, style; the text is each line's
    FitOptions fit;
    // Narration: from `at`, `gap` apart, or on the rough cut's `beats` (layoutNarration).
    Ticks at{};
    Ticks gap{kTicksPerSecond / 2};
    std::vector<TimeRange> beats;
    // The file and its provenance.
    std::filesystem::path directory;
    std::string name;                    ///< the media item's name
    std::string provider;
    std::string createdAt;               ///< ISO-8601 UTC; empty = now
    MediaId source;                      ///< the media it speaks for, if one (provenance)
    /// Opt-in voice cloning: recordings of the speaker to clone first (the voice is deleted after the run).
    std::vector<std::filesystem::path> cloneSamples;
    std::optional<MicroUsd> budget;      ///< the run's hard cap
    int rate = 48000;
};

struct DubRun {
    RenderedDub audio;
    std::vector<DubLine> lines;          ///< as spoken
    std::vector<FittedLine> fitted;
    std::vector<Ticks> generated;        ///< each line's length as generated (after trimming)
    FitReport report;
    std::size_t characters = 0;
    MicroUsd estimated = 0;              ///< what the lines were estimated at (the platform logs what was billed)
    bool cloned = false;
    std::vector<std::string> warnings;
};

using LineProgress = std::function<void(std::size_t done, std::size_t total)>;

std::expected<DubRun, AiError> runDub(const DubJob& job, ISpeechSynth& speech, SpeechAudio& audio, std::stop_token stop,
                                      const LineProgress& progress = {});

/// Leading and trailing silence (below -50 dBFS) off, 20 ms kept on each side.
std::vector<float> trimSilence(std::span<const float> samples, int rate);

/// Characters as billed (UTF-8 code points).
std::size_t characterCount(std::string_view text);

/// The whole run's cost: each line's estimate summed (expected and ceiling).
CostEstimate estimateDubCost(const ModelSpec& model, std::span<const DubLine> lines, std::chrono::year_month_day date);


} // namespace wabcut::ai::dub
