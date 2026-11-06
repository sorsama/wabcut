#pragma once

// Dynamics Processing (0.7, card 044; wabcut.audio.dynamicsProcessing): Premiere's graph-based multiband dynamics.
//
//   * The signal is split into up to four bands by Linkwitz-Riley 24 dB/octave crossovers (two Butterworth sections per
//     side; the lower bands are all-passed at the later crossovers), so the bands sum back to an all-pass: a flat
//     magnitude when nothing acts.
//   * Every band is its own dynamics section with a mode: Compress (downward compression), Expand (downward
//     expansion), Gate (an expander that takes the whole Range once under the threshold) or Limit (a compressor of
//     ratio infinity). A band has a threshold, ratio, range (the most it ever takes off), attack, release, knee and
//     make-up gain, and can be bypassed or soloed. The transfer curve is evaluated per sample on the band's level with
//     the same detector for every channel layout: one linked gain for all channels, from the band's peak over the
//     channels (the LFE of 5.1 and 7.1 is left out of the detection and follows the gain).
//   * Master section: input and output gain, a dry/wet mix, an Auto Gate in front of the bands, and an optional
//     lookahead true-peak limiter after them (5 ms: the effect's only latency; the dry signal of the mix is delayed by
//     it so the two stay in time).
//   * Metering (DynamicsMeters): the input and output peaks and the gain reduction of each band, written by the audio
//     thread with relaxed atomics and read (and cleared) by the interface.
//
// Detectors. Compress and Limit use the smooth decoupled peak detector of Giannoulis, Massberg and Reiss ("Digital
// Dynamic Range Compressor Design - A Tutorial and Analysis", JAES 2012): the gain reduction, computed from the
// instantaneous level, is held with the release time and smoothed with the attack time, so a steady tone settles on the
// static curve. Expand and Gate follow a peak envelope (10 ms decay) and smooth the reduction with the attack time when
// it falls (the gate opens) and the release time when it rises (it closes).
//
// Allocates in the constructor only; process() and reset() are RT-safe and work sample by sample (a block of up to
// kDynamicsBlock frames at a time), so the output does not depend on how a stream is cut into blocks.

#include <wabcut/audio/dsp/Biquad.h>
#include <wabcut/audio/dsp/Equalizer.h> // kMaxDspChannels
#include <wabcut/audio/dsp/Limiter.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace wabcut::audio::dsp {

inline constexpr int kDynamicsMaxBands = 4;
/// Frames per internal block (process() cuts longer input into blocks of this size; the effect chain's cells are this).
inline constexpr int kDynamicsBlock = 32;

enum class DynamicsMode : int { Compress = 0, Expand = 1, Gate = 2, Limit = 3 };

/// One band's controls.
struct DynamicsBand {
    DynamicsMode mode = DynamicsMode::Compress;
    double thresholdDb = -24.0;
    double ratio = 2.0;      ///< 2 = 2:1 (Compress and Expand); Gate and Limit ignore it
    double rangeDb = 40.0;   ///< the most the gain is ever taken down (0..120)
    double attackMs = 10.0;
    double releaseMs = 100.0;
    double kneeDb = 6.0;     ///< soft-knee width centred on the threshold (0 = hard knee)
    double makeupDb = 0.0;
    bool bypass = false;     ///< the band passes untouched (no make-up either)
    bool solo = false;       ///< only the soloed bands are heard

    bool operator==(const DynamicsBand&) const = default;
};

/// The gain reduction (dB, >= 0) of the static curve at a steady level (dB), Range applied.
double dynamicsReductionDb(double levelDb, const DynamicsBand& band) noexcept;
/// The static transfer curve: the output level (dB) for a steady input level (dB): in - reduction + make-up (a bypassed
/// band: in).
double dynamicsCurveDb(double inputDb, const DynamicsBand& band) noexcept;

/// What is fixed for a processor's life (a change builds a new one): the band count, each band's mode and the optional
/// stages.
struct DynamicsStructure {
    int bands = 4; ///< 1..4
    std::array<DynamicsMode, kDynamicsMaxBands> modes{DynamicsMode::Compress, DynamicsMode::Compress, DynamicsMode::Compress,
                                                     DynamicsMode::Compress};
    bool autoGate = false;
    bool limiter = false;
};

/// What may change at every parameter cell (the modes in `band` are ignored: the structure's count).
struct DynamicsSettings {
    std::array<double, kDynamicsMaxBands - 1> crossoverHz{120.0, 1200.0, 8000.0};
    std::array<DynamicsBand, kDynamicsMaxBands> band{};
    double autoGateThresholdDb = -50.0;
    double autoGateReleaseMs = 150.0;
    double limiterCeilingDb = -1.0;
    double limiterReleaseMs = 100.0;
};

/// Gains and the mix over a parameter cell (linear gains; mix 0..1): the value at the cell's start, the change per frame and
/// how many frames of the cell came before this call's first frame (`position`). Frame k of a call plays at
/// start + step * (position + k): the same arithmetic however a stream is cut into calls, so the output does not depend on it
/// bit for bit (an accumulated sum would round differently from every other cut).
struct DynamicsRamps {
    double input = 1.0;
    double inputStep = 0.0;
    double output = 1.0;
    double outputStep = 0.0;
    double mix = 1.0;
    double mixStep = 0.0;
    double position = 0.0;
};

/// Live levels for the interface. The audio thread notes each block; the reader takes (reads and clears) them.
class DynamicsMeters {
public:
    struct Snapshot {
        float inputPeak = 0.0f;  ///< linear, largest sample since the last take
        float outputPeak = 0.0f;
        std::array<float, kDynamicsMaxBands> reductionDb{}; ///< largest gain reduction of each band since the last take
        std::uint64_t blocks = 0;                           ///< blocks processed in all (a counter: it moves while audio plays)
    };

    /// RT-safe.
    void note(float inputPeak, float outputPeak, const std::array<float, kDynamicsMaxBands>& reductionDb) noexcept;
    /// Reads and clears the peaks.
    Snapshot take() noexcept;
    /// Reads without clearing.
    Snapshot peek() const noexcept;

private:
    std::atomic<float> m_input{0.0f};
    std::atomic<float> m_output{0.0f};
    std::array<std::atomic<float>, kDynamicsMaxBands> m_reduction{};
    std::atomic<std::uint64_t> m_blocks{0};
};

/// The meters of an effect instance (its id value), shared by every processor built for it (the player's chains and an
/// export). Created on first use; they live as long as a processor or a caller holds them. Not RT-safe.
std::shared_ptr<DynamicsMeters> dynamicsMetersFor(std::uint64_t instance);
/// The meters of an instance if some processor holds them now, else null. Not RT-safe.
std::shared_ptr<DynamicsMeters> findDynamicsMeters(std::uint64_t instance);

class DynamicsProcessing {
public:
    DynamicsProcessing(int channels, double sampleRate, const DynamicsStructure& structure);

    /// Where the levels are noted (null: nowhere). Not RT-safe.
    void attachMeters(std::shared_ptr<DynamicsMeters> meters);

    /// The frames the output lags the input: the limiter's lookahead (0 without it).
    int latencyFrames() const noexcept { return m_latency; }
    /// Input that settles the detectors after a reset: five of the longest release time constants (capped at 2 s).
    static int settleFrames(const DynamicsSettings& settings, const DynamicsStructure& structure, double sampleRate) noexcept;

    /// Processes `frames` interleaved frames in place, with `settings` and the ramps at the first frame.
    void process(float* interleaved, int frames, const DynamicsSettings& settings, DynamicsRamps ramps) noexcept;
    void reset() noexcept;

    /// The smoothed gain reduction of band `b` after the last block, dB (>= 0; for Expand and Gate the gain taken off).
    double bandReductionDb(int b) const noexcept { return m_reduction[static_cast<std::size_t>(b)]; }

private:
    struct Channel {
        std::array<std::array<BiquadState, 2>, kDynamicsMaxBands - 1> lowPass{};
        std::array<std::array<BiquadState, 2>, kDynamicsMaxBands - 1> highPass{};
        /// all-pass[band][crossover]: the compensation of a lower band at a later crossover.
        std::array<std::array<BiquadState, kDynamicsMaxBands - 1>, kDynamicsMaxBands - 1> allPass{};
    };
    struct Coefficients {
        std::array<BiquadCoefficients, kDynamicsMaxBands - 1> lowPass{};
        std::array<BiquadCoefficients, kDynamicsMaxBands - 1> highPass{};
        std::array<BiquadCoefficients, kDynamicsMaxBands - 1> allPass{};
    };
    struct BandState {
        double envelope = 0.0; ///< Expand / Gate: the peak envelope
        double held = 0.0;     ///< Compress / Limit: the held reduction
        double smooth = 0.0;   ///< the smoothed reduction (dB)
        double active = 1.0;   ///< the dynamics (1) or the untouched band (0) after a bypass, smoothed
        double audible = 1.0;  ///< 1 heard, 0 muted by another band's solo, smoothed
    };

    void setCrossovers(const DynamicsSettings& settings) noexcept;
    void processBlock(float* io, int frames, const DynamicsSettings& settings, DynamicsRamps& ramps) noexcept;

    int m_channels;
    double m_rate;
    DynamicsStructure m_structure;
    int m_bands;
    int m_latency = 0;
    std::array<bool, kMaxDspChannels> m_detects{}; ///< the channels the detectors listen to (all but the LFE of 5.1 / 7.1)

    std::array<double, kDynamicsMaxBands - 1> m_frequency{-1.0, -1.0, -1.0};
    Coefficients m_k{};
    std::array<Channel, kMaxDspChannels> m_filters{};
    std::array<BandState, kDynamicsMaxBands> m_state{};
    std::array<double, kDynamicsMaxBands> m_reduction{};

    // Time constants that never change.
    double m_envelopeDecay;
    double m_switchCoefficient; ///< bypass / solo smoothing (5 ms)

    // Auto Gate.
    double m_gateEnvelope = 0.0;
    double m_gateGain = 0.0;
    std::int64_t m_gateHoldLeft = 0;

    // The limiter and the dry path that waits for it.
    std::unique_ptr<Limiter> m_limiter;
    std::vector<float> m_dryRing; ///< (latency + 1) frames
    std::size_t m_dryPos = 0;
    std::array<float, kDynamicsBlock * kMaxDspChannels> m_dry{}; ///< the block's dry frames, delayed

    std::shared_ptr<DynamicsMeters> m_meters;
};

} // namespace wabcut::audio::dsp
