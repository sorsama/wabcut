#pragma once

// Beat and tempo detection on mono audio, the classic DSP way (own implementation, no third-party code):
//
//   1. onset strength: log-mel spectral flux (Hann-windowed STFT, 40 mel bands 30 Hz-8 kHz, dB, the positive change
//      from frame to frame summed over the bands), high-passed and normalised, 100 frames a second;
//   2. tempo: the autocorrelation of the onset strength over 60..200 BPM weighted by a log-normal prior around
//      120 BPM; when the twice-as-fast tempo explains the onsets as well (most of the autocorrelation at half the
//      lag), it wins, so 174 BPM is not heard as 87;
//   3. beats: dynamic programming after Ellis (2007), "Beat Tracking by Dynamic Programming": each beat maximises
//      onset strength plus a penalty, -tightness x log(interval / period)^2, for straying from the tempo;
//   4. end beats off the tempo by more than a fifth are dropped, then weak end beats (librosa's rule: under half
//      the RMS of the beats' onset strengths);
//   5. each beat is moved to the attack in the waveform near it (1 ms energy steps), so beat times are sample
//      exact; the tempo is then fitted to the beats (least squares);
//   6. downbeats: the place in the bar (beatsPerBar) whose beats are loudest just after their attack, low
//      frequencies (kick drums) counting double.
//
// BeatTracker is the interface; DspBeatTracker is this pipeline. A learned tracker (Beat This!, ONNX through
// ai_local) implements the same interface later and plugs in where a tracker id is chosen.

#include <wabcut/ai/analysis/ShotDetection.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::analysis {

struct BeatOptions {
    double minBpm = 60.0;
    double maxBpm = 200.0;
    double priorBpm = 120.0;
    double priorOctaves = 1.0;  ///< standard deviation of the log-normal tempo prior
    double tightness = 100.0;   ///< how strongly beats keep the tempo (Ellis' alpha)
    int beatsPerBar = 4;
    std::string tracker = "dsp"; ///< "dsp" (DspBeatTracker); learned trackers register other ids

    /// Every setting that changes the result, for cache keys.
    std::string fingerprint() const;
};

struct BeatAnalysis {
    static constexpr std::string_view kAnalyzer = "wabcut-beats";
    double bpm = 0.0;
    float confidence = 0.0f;   ///< 0..1: how periodic and regular the beats are
    std::vector<Ticks> beats;  ///< source time of each beat (its attack)
    std::vector<Ticks> downbeats;
    int beatsPerBar = 4;
    Ticks duration;            ///< length of the audio analysed
    std::string tracker;       ///< e.g. "dsp-1"
    bool operator==(const BeatAnalysis&) const = default;
};

/// Mono audio: samples from source time `origin` (sample i plays at origin + i / sampleRate).
struct MonoAudio {
    std::span<const float> samples;
    int sampleRate = 22050;
    Ticks origin;
};

class BeatTracker {
public:
    virtual ~BeatTracker() = default;
    /// "dsp-1", "beat-this-1", ...: stored with results and in cache keys.
    virtual std::string id() const = 0;
    /// Errors: InvalidArgument (no audio, bad rate), Cancelled.
    virtual std::expected<BeatAnalysis, Error> track(const MonoAudio& audio, const BeatOptions& options,
                                                     const AnalysisProgress& progress = {}) = 0;
};

class DspBeatTracker : public BeatTracker {
public:
    std::string id() const override { return "dsp-1"; }
    std::expected<BeatAnalysis, Error> track(const MonoAudio& audio, const BeatOptions& options,
                                             const AnalysisProgress& progress = {}) override;
};

/// The tracker for `options.tracker`, or null when none of that name exists ("dsp" always does).
std::unique_ptr<BeatTracker> makeBeatTracker(std::string_view name);

/// Convenience: DspBeatTracker on `audio`.
std::expected<BeatAnalysis, Error> detectBeats(const MonoAudio& audio, const BeatOptions& options = {},
                                               const AnalysisProgress& progress = {});

} // namespace wabcut::ai::analysis
