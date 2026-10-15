#pragma once

// The media-backed AudioServices (full builds): loudness measured on the real render path (playback::OfflineMixdown,
// so clip, track and master effects count exactly as preview and export hear them) with libebur128, cached in the
// AnalysisCache by a fingerprint of everything that changes the audio; speech found from transcript words or, where
// there is no transcript, from the rendered audio's level.

#include <wabcut/audio/edit/AudioAnalysis.h>

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/playback/audio/AudioFeed.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace wabcut::audio::edit {

/// The decoded audio an analysis renders from, for one project state.
using FeedFactory = std::function<std::shared_ptr<playback::AudioFeed>(const ProjectState& state)>;
/// Real media: playback::MediaAudioFeed over the state's locators.
FeedFactory mediaFeedFactory();

struct RenderAnalysisOptions {
    int channels = 2;
    /// Analysis rate; 0 = the sequence's audio rate.
    int sampleRate = 0;
    /// Where results are kept between runs (and projects); null = no cache.
    std::shared_ptr<ai::AnalysisCache> cache;
};

class RenderLoudnessAnalyzer final : public LoudnessAnalyzer {
public:
    explicit RenderLoudnessAnalyzer(FeedFactory feeds, RenderAnalysisOptions options = {});
    std::expected<LoudnessStats, Error> measure(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                std::optional<TimeRange> range, command::ExecutionControl* control) override;
    /// The same, reporting the fraction rendered (0..1) as it goes (LoudnessJobs.h).
    std::expected<LoudnessStats, Error> measure(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                std::optional<TimeRange> range, command::ExecutionControl* control,
                                                const std::function<void(double)>& progress);

    std::uint64_t renders() const noexcept { return m_renders.load(); }
    std::uint64_t cacheHits() const noexcept { return m_hits.load(); }

private:
    FeedFactory m_feeds;
    RenderAnalysisOptions m_options;
    std::atomic<std::uint64_t> m_renders{0};
    std::atomic<std::uint64_t> m_hits{0};
};

/// Speech from the audio level: 10 ms RMS frames; speech where the level rises 12 dB above the track's noise floor
/// (and above -50 dBFS), held through dips shorter than 250 ms.
struct EnergySpeechOptions {
    double floorMarginDb = 12.0;
    double minimumDbfs = -50.0;
    double releaseDb = 6.0;   ///< hysteresis: speech ends below threshold - releaseDb
    Ticks bridge{kTicksPerSecond / 4};
};

class RenderSpeechDetector final : public SpeechDetector {
public:
    RenderSpeechDetector(FeedFactory feeds, std::shared_ptr<const ai::edit::TranscriptProvider> transcripts,
                         EnergySpeechOptions energy = {}, RenderAnalysisOptions options = {});
    std::expected<std::vector<TimeRange>, Error> speech(const ProjectState& state, SequenceId sequence,
                                                        std::span<const TrackId> tracks, SpeechSource source,
                                                        std::optional<TimeRange> range,
                                                        command::ExecutionControl* control) override;

private:
    FeedFactory m_feeds;
    std::shared_ptr<const ai::edit::TranscriptProvider> m_transcripts;
    EnergySpeechOptions m_energy;
    RenderAnalysisOptions m_options;
};

/// Speech ranges in a level envelope (dBFS per frame of `frameTicks`, starting at `start`). Exposed for tests.
std::vector<TimeRange> speechFromLevels(std::span<const double> levelsDb, Ticks start, Ticks frameTicks,
                                        const EnergySpeechOptions& options);

/// Sorts and merges ranges that overlap or are closer than `bridge`.
std::vector<TimeRange> mergeRanges(std::vector<TimeRange> ranges, Ticks bridge = {});

/// Audio sync over the project's media (A.5): files decoded with AudioDecoder as mono at the sync's rates (an angle's
/// files back to back), results cached in `cache` (null: none) by the files' fingerprints and the options.
std::shared_ptr<AudioSyncService> makeMediaAudioSync(std::shared_ptr<ai::AnalysisCache> cache);

/// Music Remix over the project's media (A.7): a stream decoded with AudioDecoder as mono at kRemixAnalysisRate, its
/// beats tracked (ai/analysis DspBeatTracker) and its beat features computed, the result cached in `cache` (null: none)
/// by the file's fingerprint; takes rendered from the stream at its own rate and channels into `takesDirectory`
/// (<user cache>/remix when empty).
std::shared_ptr<MusicAnalyzer> makeMediaMusicAnalyzer(std::shared_ptr<ai::AnalysisCache> cache,
                                                      std::filesystem::path takesDirectory = {});

/// A.9: per-channel levels over the project's media: the stream decoded at its own channel count (MediaAudioFeed), at
/// most ChannelAnalyzer::kMaxSeconds of the range.
std::shared_ptr<ChannelAnalyzer> makeMediaChannelAnalyzer();

/// Real media, the user's analysis cache, and the given transcripts (null: energy only). Includes the audio sync,
/// Music Remix and the channel levels.
std::shared_ptr<AudioServices> makeMediaAudioServices(std::shared_ptr<const ai::edit::TranscriptProvider> transcripts,
                                                      std::shared_ptr<ai::AnalysisCache> cache);

} // namespace wabcut::audio::edit
