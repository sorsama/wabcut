#pragma once

// Syncing recordings of one event by their audio (A.5; docs/ai.md "Audio sync"): a camera's scratch audio and a
// separate recorder (dual-system sound, Premiere's Synchronize by audio), several cameras (multicam's audio sync).
//
// Coarse to fine:
//   1. Every recording is read once as mono at `analysisRate` and reduced to a 1 kHz onset envelope (high-passed band
//      energy in dB, its rises above a 30 ms running mean: gain, hum and a different microphone change little of it).
//   2. GCC-PHAT of each recording's envelope against the reference's over every lag that leaves `minOverlapSeconds`
//      of overlap; the peak-to-sidelobe ratio of the correlation is the match's confidence. Too little overlap, a
//      flat or ambiguous correlation (unrelated audio, silence) is a failure with its reason, never a guess.
//   3. Windows across the overlap are read at `refineRate` and correlated (GCC-PHAT on the high-passed waveform,
//      parabolic peak interpolation) around the coarse lag: sample-accurate offsets. A line through them gives the
//      offset at the overlap's middle and the clock drift between the devices (ppm), with a speed proposal that
//      cancels it.
// Recordings that do not overlap the reference are synced through another synced one (chaining).
//
// Pure C++ on an abstract reader (SyncSignal): the media-backed one decodes files with AudioDecoder (a camera that
// split its recording into several files is one signal, the files back to back), tests make signals up.

#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::audio::edit {

/// A recording, read as mono at any rate.
class SyncSignal {
public:
    virtual ~SyncSignal() = default;
    /// Its length in frames at `rate`.
    virtual std::int64_t frames(int rate) const = 0;
    /// Mono samples [first, first + out.size()) at `rate`, zeros outside the recording. Called from one thread at a
    /// time per signal. An error ends the sync.
    virtual std::expected<void, Error> read(int rate, std::int64_t first, std::span<float> out) = 0;
};

struct SyncOptions {
    int analysisRate = 8000;         ///< the coarse pass reads at this rate (its envelope is 1 kHz)
    int refineRate = 48000;          ///< the fine pass; offsets are samples at this rate
    double minOverlapSeconds = 5.0;  ///< a match needs this much common audio
    double minConfidence = 0.5;      ///< 0..1; below it the pair does not sync
    int refineWindows = 5;           ///< fine windows across the overlap (offset and drift)
    double windowSeconds = 4.0;
    double searchSeconds = 0.5;      ///< fine search around the coarse lag (covers drift up to ~0.5 s)
    bool chain = true;               ///< sync a recording through another synced one when the reference fails
    int threads = 0;                 ///< parallel recordings; 0 = up to 4
};

/// Where one recording sits on the reference's clock.
struct SyncMatch {
    bool synced = false;
    /// refineRate samples from the reference's start to this recording's start (positive: it starts later), as the
    /// line through the fine windows has it at the overlap's middle.
    std::int64_t offset = 0;
    std::int64_t offsetAtStart = 0;  ///< the same at the overlap's start (differs by the drift)
    double confidence = 0.0;         ///< 0..1
    double peakToSidelobe = 0.0;     ///< the coarse correlation's peak in standard deviations of its sidelobes
    double overlapSeconds = 0.0;
    int windows = 0;                 ///< fine windows that agreed
    /// This recording's clock against the reference's, parts per million: positive when it runs fast (it recorded
    /// more samples per real second).
    double driftPpm = 0.0;
    double driftResidualMs = 0.0;    ///< how far the fine windows sit from the line (RMS)
    bool driftSignificant = false;   ///< |drift| >= 2 ppm and well measured: correct it with speedProposal
    double speedProposal = 1.0;      ///< clip speed that cancels the drift (1 + driftPpm / 1e6)
    std::size_t relativeTo = 0;      ///< the recording it was matched against (chaining; the reference by default)
    std::string problem;             ///< why it did not sync
};

struct SyncReport {
    std::size_t reference = 0;
    std::vector<SyncMatch> matches; ///< one per signal; the reference's is synced at 0 with confidence 1
    double seconds = 0.0;           ///< wall time
};

/// Syncs `recordings` to recordings[reference]. `progress` gets 0..1 (it may throw to stop); Cancelled when `stop` is
/// requested. Errors: InvalidArgument (fewer than two signals, a bad reference or option), the readers' errors,
/// Cancelled. A recording that cannot be synced is not an error: its match says why.
// (No parameter may be called `signals`/`slots`/`emit`: Qt app code includes this header with Qt keywords on.)
std::expected<SyncReport, Error> syncSignals(std::span<SyncSignal* const> recordings, std::size_t reference,
                                             const SyncOptions& options = {}, std::stop_token stop = {},
                                             const std::function<void(double)>& progress = {});

// ---- Building blocks (tests) ----

/// The onset envelope of a whole signal: one value per millisecond.
std::expected<std::vector<float>, Error> onsetEnvelope(SyncSignal& signal, int rate, std::stop_token stop = {},
                                                       const std::function<void(double)>& progress = {});

struct CoarseMatch {
    bool found = false;
    std::int64_t lag = 0;         ///< envelope frames (ms): b's start on a's clock
    double peakToSidelobe = 0.0;
    double confidence = 0.0;
    std::int64_t overlap = 0;     ///< frames
    std::string problem;
};
/// GCC-PHAT of two envelopes over the lags with at least `minOverlap` frames in common.
CoarseMatch coarseMatch(std::span<const float> a, std::span<const float> b, std::int64_t minOverlap);

// ---- For the commands: media items as recordings ----

/// The host's audio sync over the project's media (audio.syncClips, and multicam's audio method). A recording ("angle")
/// is one media item, or several played back to back (a camera that split its recording); `stream` is the audio
/// stream ordinal of each.
class AudioSyncService {
public:
    struct Angle {
        std::vector<MediaId> media;
        int stream = 0;
        bool keepOrder = false; ///< play the files as listed; otherwise in the order they were recorded
    };
    struct Result {
        SyncReport report;
        /// Per angle, per media item (in the order the angle was played): where that file starts on the reference's
        /// clock, in ticks.
        std::vector<std::vector<Ticks>> starts;
        /// The angles' files in the order they were played (sorted by creation when the angle asked for it).
        std::vector<std::vector<MediaId>> order;
        bool cached = false;
    };
    virtual ~AudioSyncService() = default;
    /// Errors: NotFound (a missing or offline media item, a stream it lacks), InvalidArgument, decoding errors,
    /// Cancelled.
    virtual std::expected<Result, Error> sync(const ProjectState& state, std::span<const Angle> angles, std::size_t reference,
                                              const SyncOptions& options, std::stop_token stop,
                                              const std::function<void(double)>& progress) = 0;
};

/// Multicam's audio sync (C.8's AudioSyncAnalyzer::offsets): where each angle's audio stream 0 starts relative to
/// angles[0]'s, in ticks (positive: later). Fails, with the angle and the reason, when one cannot be synced.
std::expected<std::vector<Ticks>, Error> angleOffsets(AudioSyncService& sync, const ProjectState& state,
                                                      std::span<const MediaId> angles, std::stop_token stop = {},
                                                      const std::function<void(double)>& progress = {});

/// C.8's AudioSyncAnalyzer (Nesting.h) on an AudioSyncService: what multicam.create's "audio" sync runs (angleOffsets),
/// stopped and followed through the command's checkpoints. The host bundles it with its other services as
/// command::MulticamServices(std::make_shared<MulticamAudioSync>(audioServices->sync)).
class MulticamAudioSync final : public AudioSyncAnalyzer {
public:
    explicit MulticamAudioSync(std::shared_ptr<AudioSyncService> sync) : m_sync(std::move(sync)) {}

    std::expected<std::vector<Ticks>, Error> offsets(const ProjectState& state, std::span<const MediaId> angles) const override;
    std::expected<std::vector<Ticks>, Error> offsets(const ProjectState& state, std::span<const MediaId> angles,
                                                     const AnalysisControl& control) const override;

private:
    std::shared_ptr<AudioSyncService> m_sync;
};

} // namespace wabcut::audio::edit
