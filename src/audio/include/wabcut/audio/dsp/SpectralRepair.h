#pragma once

// 0.7 (card 047): the DSP of Spectral Repair (wabcut.audio.spectralRepair; the edit list: wabcut/model/SpectralRepair.h).
//
// The repairs are applied as a CORRECTION to the sound, never as a re-synthesis of all of it: the output is the input
// delayed by latencyFrames() plus the overlap-added correction of the few STFT frames an edit touches. Outside the edits
// the output is the input, sample for sample. Within an edit:
//
//   * The sound is analysed with a streaming STFT (sqrt-Hann windows, 75 % overlap, 1024 / 2048 / 4096 points) whose
//     frame grid is anchored to the ABSOLUTE frame number (the chain's grid offset), so preview and export, starting at
//     different places, cut the sound into the same frames and agree to the last bit once the pre-roll is past.
//   * Attenuate scales the bins of the region by the asked gain, with a soft edge. Heal and Fill-with-noise-floor replace
//     them with a level taken from the nearest untouched bins before, after, above and below (heal: an inverse-distance
//     mean of their log levels; noise floor: the lower quartile of the quiet bins in time), with a random but fixed phase,
//     so no tone or "musical noise" is made; the level lost to overlap-adding unrelated phases is made up (the gain is
//     measured once by overlap-adding a flat random-phase spectrum).
//   * Remove Tone works in the time domain: the tone and its harmonics are demodulated at their source frequency (the edit's
//     trace, integrated to a phase that is a function of source time), low-passed and subtracted, which cuts a narrow
//     notch that follows a moving tone and leaves everything around it alone.
//   * Every region edit's correction is GATED to the seconds it was made for (a crossfade of a quarter of its length, at most
//     5 ms, at its edges): an STFT frame is 40 ms long, but a click is healed without touching what is next to it, and the
//     sound outside every edit stays the input, sample for sample. Edits apply in order, each to the spectrum the one before
//     left; the tones come first, on the time-domain signal.
// Edit times are source time: the host's SpectralTimeMap turns chain frames into source seconds (the clip's time map).
//
// Listen modes: Off plays the repaired sound; Selection plays only the sound inside the edits' regions (what Heal and
// Attenuate act on, and the tones); Removed plays only what the repair takes away.
//
// Threading: construction allocates and belongs to a non-real-time thread; process() and reset() are RT-safe (no
// allocation, no locks).

#include <wabcut/model/SpectralRepair.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace wabcut::audio::dsp {

/// What the repair asks of its host: where on the clip's source a chain frame plays.
class SpectralTimeMap {
public:
    virtual ~SpectralTimeMap() = default;
    /// Source time, seconds, of chain frame `frame`.
    virtual double sourceSeconds(std::int64_t frame) const noexcept = 0;
    /// How many times higher the sound plays than the source at `frame` (a clip resampled to a higher speed): an edit's
    /// frequencies are multiplied by it. 1 when the pitch is kept or the clip plays at normal speed.
    virtual double pitchRatio(std::int64_t frame) const noexcept {
        (void)frame;
        return 1.0;
    }
};

/// The chain frame is the source time at 1x: a clip that plays from the start of its source with nothing else (tests,
/// the offline renderer).
class IdentityTimeMap final : public SpectralTimeMap {
public:
    IdentityTimeMap(int sampleRate, double sourceStartSeconds = 0.0) : m_rate(sampleRate), m_start(sourceStartSeconds) {}
    double sourceSeconds(std::int64_t frame) const noexcept override { return m_start + static_cast<double>(frame) / m_rate; }

private:
    double m_rate;
    double m_start;
};

enum class SpectralListen : std::uint8_t { Off, Selection, Removed };

struct SpectralRepairOptions {
    int fftSize = 2048; ///< 1024, 2048 or 4096 (anything else is rounded to one of them)
    SpectralListen listen = SpectralListen::Off;
};

class SpectralRepair {
public:
    static constexpr int kMaxChannels = 8;

    /// Edits that fail spectralEditProblem() are left out; at most kMaxSpectralEdits are used.
    SpectralRepair(int channels, int sampleRate, std::span<const SpectralEdit> edits, const SpectralRepairOptions& options = {});
    ~SpectralRepair();
    SpectralRepair(const SpectralRepair&) = delete;
    SpectralRepair& operator=(const SpectralRepair&) = delete;

    /// There is something to do (an edit that survived).
    bool active() const noexcept;
    /// Frames the output lags the input.
    int latencyFrames() const noexcept;
    /// Input frames that bring the state from a reset to steady behaviour (the pre-roll after a seek).
    int settleFrames() const noexcept;
    int fftSize() const noexcept;
    /// Frames of context the heal looks ahead (and back), part of the latency.
    int contextFrames() const noexcept;

    /// Processes `frames` interleaved frames in place. `firstFrame` is the chain frame of the first one (a call that does not
    /// continue the last one starts the state over), `gridOffset` the absolute frame number of chain frame 0 (anchors the
    /// STFT grid; use the clip's timeline sample number of the chain origin), `map` the source time of the frames.
    void process(float* interleaved, int frames, std::int64_t firstFrame, std::int64_t gridOffset, const SpectralTimeMap& map) noexcept;
    /// Clears all state (a discontinuity).
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// The repair of a whole signal at once, for the editor's "result" view and tests: the input repaired, the same length
/// (the latency is compensated). The same code as the chain runs.
std::vector<float> renderSpectralRepair(std::span<const float> interleaved, int channels, int sampleRate,
                                        std::span<const SpectralEdit> edits, const SpectralRepairOptions& options,
                                        const SpectralTimeMap& map, std::int64_t gridOffset = 0);

/// How much an edit covers the point (seconds of source, Hz of source) in [0, 1], for an analysis at `fftSize` / `sampleRate`
/// (the soft edge is a hop in time and two bins in frequency). Exposed for the editor's drawing and tests.
double spectralCoverage(const SpectralEdit& edit, double seconds, double hz, int fftSize, int sampleRate);

} // namespace wabcut::audio::dsp
