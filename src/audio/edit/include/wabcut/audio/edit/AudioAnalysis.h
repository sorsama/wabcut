#pragma once

// What the dialogue-first audio commands need from the host, beside the project: loudness measurements and where
// people speak. Pure interfaces; the media-backed implementations (OfflineMixdown + libebur128, transcripts, the
// audio energy) are in MediaAnalysis.h (full builds), and tests use fakes.

#include <wabcut/audio/edit/AudioSync.h>
#include <wabcut/audio/edit/Remix.h>
#include <wabcut/audio/edit/SoundClassifier.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::audio::edit {

/// EBU R128 / ITU-R BS.1770 statistics of a rendered programme. Levels are -infinity for silence.
struct LoudnessStats {
    double integratedLufs = -std::numeric_limits<double>::infinity();
    double rangeLu = 0.0;
    double truePeakDbtp = -std::numeric_limits<double>::infinity();
    double samplePeakDbfs = -std::numeric_limits<double>::infinity();
    double maxMomentaryLufs = -std::numeric_limits<double>::infinity();
    double maxShortTermLufs = -std::numeric_limits<double>::infinity();
    Ticks duration;

    bool hasIntegrated() const noexcept { return integratedLufs > -1e300; }
};

/// What is measured: one clip as it renders on its own (its media, Volume/Panner and effect stack), one track
/// (its clips, transitions and track effects, before mute and fader), or the sequence's mix (mute/solo, the master
/// audio effects: what export writes).
struct LoudnessTarget {
    enum class Scope : std::uint8_t { Clip, Track, Mix };
    Scope scope = Scope::Mix;
    ClipId clip;
    TrackId track;

    static LoudnessTarget ofClip(ClipId c) { return {Scope::Clip, c, {}}; }
    static LoudnessTarget ofTrack(TrackId t) { return {Scope::Track, {}, t}; }
    static LoudnessTarget mix() { return {Scope::Mix, {}, {}}; }
    bool operator==(const LoudnessTarget&) const = default;
};

class LoudnessAnalyzer {
public:
    virtual ~LoudnessAnalyzer() = default;
    /// Measures `target` of `sequence` in `state` over `range` (timeline; nullopt = the target's extent: the clip,
    /// the track's first to last clip, the sequence's content). `control` may stop a long measurement.
    virtual std::expected<LoudnessStats, Error> measure(const ProjectState& state, SequenceId sequence,
                                                        const LoudnessTarget& target, std::optional<TimeRange> range,
                                                        command::ExecutionControl* control) = 0;
};

/// How speech is found on dialogue tracks.
enum class SpeechSource : std::uint8_t {
    Auto,       ///< transcript words where the clips have a transcript, the audio energy elsewhere
    Transcript, ///< only transcript words
    Energy,     ///< only the rendered audio's level
};

std::string_view toString(SpeechSource source);
std::optional<SpeechSource> speechSourceFromString(std::string_view text);

class SpeechDetector {
public:
    virtual ~SpeechDetector() = default;
    /// Timeline ranges (sorted, disjoint) where the given tracks carry speech, within `range` (nullopt = the
    /// whole sequence).
    virtual std::expected<std::vector<TimeRange>, Error> speech(const ProjectState& state, SequenceId sequence,
                                                                std::span<const TrackId> tracks, SpeechSource source,
                                                                std::optional<TimeRange> range,
                                                                command::ExecutionControl* control) = 0;
};

/// Enhance Speech (A.4, docs/ai.md "Enhance Speech"): renders the enhanced take of a media item's audio stream offline
/// (DeepFilterNet3 in the wabcut-embed helper; implementation: wabcut/ai/audio/AudioJobs.h), or finds the one already
/// rendered for the same file and model.
class SpeechEnhancer {
public:
    struct Take {
        std::string path;         ///< UTF-8, absolute: a 48 kHz float WAV of the stream from source time 0
        std::string model;        ///< model and processing version (AudioTake::model)
        SourceFingerprint source; ///< the file it was rendered from
        bool reused = false;      ///< an earlier render of the same file and model was found
        double seconds = 0.0;     ///< length of the audio
        double elapsed = 0.0;     ///< wall time of the render, seconds (0 when reused)
        std::string device;       ///< what the model ran on ("DirectML", "CPU"; "" when reused)
    };
    virtual ~SpeechEnhancer() = default;
    /// The model and processing version takes are rendered with now; a take of another is stale.
    virtual std::string modelVersion() const = 0;
    /// The take of `item`'s audio stream `streamOrdinal`. `progress` gets 0..1. Errors: NotFound (model or helper
    /// missing), IoError, DecoderError (the helper failed), Unsupported (not enough memory), Cancelled.
    virtual std::expected<Take, Error> render(const MediaItem& item, int streamOrdinal, std::stop_token stop,
                                              const std::function<void(double)>& progress) = 0;
};

/// Stem separation (A.4, docs/ai.md "Stem separation"): splits a media item's audio stream into stems (HTDemucs:
/// drums, bass, other, vocals), each a new file to import as linked media.
class StemSeparator {
public:
    struct Stem {
        std::string part;        ///< "vocals", ...
        std::string path;        ///< UTF-8, absolute
        MediaInfo info;          ///< probed
        SourceFingerprint fingerprint;
    };
    struct Result {
        std::vector<Stem> stems;
        std::string model;       ///< model and separation version (DerivedProvenance::model)
        double seconds = 0.0;
        double elapsed = 0.0;
        std::string device;
    };
    virtual ~StemSeparator() = default;
    virtual std::string modelVersion() const = 0;
    /// Errors as SpeechEnhancer::render; Unsupported also when the computer has too little memory for the model.
    virtual std::expected<Result, Error> separate(const MediaItem& item, int streamOrdinal, std::stop_token stop,
                                                  const std::function<void(double)>& progress) = 0;
};

/// Music Remix (A.7, docs/ai.md "Music Remix"): the analysis a remix is planned from (beats, downbeats and beat
/// features of a media item's whole audio stream; implementation: MediaAnalysis.h makeMediaMusicAnalyzer), and the
/// rendered remix take.
class MusicAnalyzer {
public:
    struct Take {
        std::string path;         ///< UTF-8, absolute: a float WAV of the remix
        MediaInfo info;           ///< probed
        SourceFingerprint fingerprint;
        double elapsed = 0.0;     ///< seconds the render took
    };
    virtual ~MusicAnalyzer() = default;
    /// The analysis of `item`'s audio stream `streamOrdinal` (cached by the file's fingerprint). `progress` gets 0..1.
    /// Errors: NotFound (offline, no such stream), DecoderError, IoError, InvalidArgument (no beats), Cancelled.
    virtual std::expected<MusicAnalysis, Error> analyze(const MediaItem& item, int streamOrdinal, std::stop_token stop,
                                                        const std::function<void(double)>& progress) = 0;
    /// Renders `plan` from that stream (its rate and channels) into a new take file. Errors as analyze().
    virtual std::expected<Take, Error> render(const MediaItem& item, int streamOrdinal, const RemixPlan& plan,
                                              std::stop_token stop, const std::function<void(double)>& progress) = 0;
};

/// A.9: the level of one source channel (audio.channelLevels: "which microphone is on which channel", "the left-only
/// audio"), dBFS (-144 for silence).
struct ChannelLevel {
    double peakDb = -144.0;
    double rmsDb = -144.0;
};

/// A.9: measures each channel of a media item's audio stream over a source range (implementation: MediaAnalysis.h
/// makeMediaChannelAnalyzer; at most kMaxSeconds of it, from the range's start).
class ChannelAnalyzer {
public:
    static constexpr double kMaxSeconds = 120.0;
    virtual ~ChannelAnalyzer() = default;
    /// One level per source channel. Errors: NotFound (offline, no such stream), DecoderError, Cancelled.
    virtual std::expected<std::vector<ChannelLevel>, Error> measure(const ProjectState& state, const MediaItem& item,
                                                                     int streamOrdinal, TimeRange sourceRange,
                                                                     command::ExecutionControl* control) = 0;
};

/// p02 audio: Essential Sound auto-tagging (audio.autoTag): the Dialogue / Music / SFX / Ambience a stretch of a media
/// item's audio sounds like, by SoundClassifier.h's local, deterministic classifier (implementation: MediaAnalysis.h
/// makeMediaSoundTagger: at most kMaxSeconds from the middle of the range, decoded to mono).
class SoundTagger {
public:
    static constexpr double kMaxSeconds = 30.0;
    virtual ~SoundTagger() = default;
    /// Errors: NotFound (offline, no such stream), DecoderError, Cancelled.
    virtual std::expected<SoundGuess, Error> classify(const ProjectState& state, const MediaItem& item, int streamOrdinal,
                                                     TimeRange sourceRange, command::ExecutionControl* control) = 0;
};

/// The host's audio analysis for the audio.* commands (EnginePolicy::services, alone or in a command::ServiceBundle
/// with the transcript and media-analysis services).
class AudioServices : public command::CommandServices {
public:
    AudioServices(std::shared_ptr<LoudnessAnalyzer> loudnessAnalyzer, std::shared_ptr<SpeechDetector> speechDetector)
        : loudness(std::move(loudnessAnalyzer)), speech(std::move(speechDetector)) {}

    std::shared_ptr<LoudnessAnalyzer> loudness;
    std::shared_ptr<SpeechDetector> speech;
    /// A.4: local audio AI (null: audio.enhanceSpeech renders nothing and audio.separateStems fails).
    std::shared_ptr<SpeechEnhancer> enhancer;
    std::shared_ptr<StemSeparator> separator;
    /// A.5: audio sync (null: audio.syncClips fails).
    std::shared_ptr<AudioSyncService> sync;
    /// A.7: Music Remix's analysis and takes (null: audio.remix fails).
    std::shared_ptr<MusicAnalyzer> music;
    /// A.9: per-channel levels (null: audio.channelLevels fails).
    std::shared_ptr<ChannelAnalyzer> channels;
    /// p02 audio: Essential Sound auto-tagging (null: audio.autoTag fails).
    std::shared_ptr<SoundTagger> tagger;
};

} // namespace wabcut::audio::edit
