#pragma once

// True-peak estimation (ITU-R BS.1770-4 Annex 2: 4x oversampling with the standard's 48-tap interpolation filter)
// and the lookahead brick-wall limiter of wabcut.audio.limiter, which limits true peaks, not just samples.

#include <wabcut/audio/dsp/Equalizer.h> // kMaxDspChannels

#include <array>
#include <cstdint>
#include <memory>

namespace wabcut::audio::dsp {

/// Running true-peak estimate of one channel: push samples, read the largest |value| of the 4x oversampled signal
/// around the sample pushed kDelay samples ago.
class TruePeakDetector {
public:
    static constexpr int kTaps = 12;  ///< per phase
    static constexpr int kPhases = 4;
    /// Samples between pushing a sample and the interpolated values around it being available.
    static constexpr int kDelay = kTaps / 2;

    /// Pushes one sample; returns max(|x[n - kDelay]|, |the four interpolated values near it|).
    float push(float x) noexcept;
    void reset() noexcept;

private:
    std::array<float, 2 * kTaps> m_history{}; // mirrored ring: contiguous reads of the last kTaps samples
    int m_pos = 0;
};

/// The interpolation filter of BS.1770-4 (four phases of 12 taps).
const std::array<std::array<float, TruePeakDetector::kTaps>, TruePeakDetector::kPhases>& truePeakFilter() noexcept;

struct LimiterSettings {
    double ceilingDb = -1.0;  ///< dBTP: no true peak of the output exceeds it
    double releaseMs = 50.0;
    double lookaheadMs = 5.0; ///< constant: changing it means a new limiter

    bool operator==(const LimiterSettings&) const = default;
};

/// Lookahead limiter: the gain needed by each (true) peak is held over the lookahead window and smoothed with a box
/// filter of the same length, so it is fully down when the delayed peak arrives (no overshoot, no clipping), then
/// released exponentially. The gain aims 0.15 dB under the ceiling (the 4x estimate reads high-frequency true peaks a
/// little low) and a final hard clip at the ceiling catches sample peaks. Signals below the ceiling minus that margin
/// pass untouched. The output is delayed by latencyFrames().
///
/// Allocates in the constructor only; process() and reset() are RT-safe.
class Limiter {
public:
    /// `maxLookaheadMs` sizes the delay lines (the descriptor allows 20 ms).
    Limiter(int channels, double sampleRate, double lookaheadMs, double maxLookaheadMs = 20.0);

    /// Ceiling and release; the lookahead keeps its construction value.
    void setSettings(double ceilingDb, double releaseMs) noexcept;

    int latencyFrames() const noexcept { return m_latency; }
    int lookaheadFrames() const noexcept { return m_lookahead; }
    /// Input that settles the gain after a reset: the lookahead plus five release time constants (capped at 2 s).
    int settleFrames() const noexcept;

    void process(float* interleaved, int frames) noexcept;
    void reset() noexcept;

    /// Gain applied to the last output frame (1 = no limiting).
    double gain() const noexcept { return m_gain; }

private:
    void pushRequired(double required) noexcept;

    int m_channels;
    double m_sampleRate;
    int m_lookahead;  ///< L
    int m_latency;    ///< L + TruePeakDetector::kDelay + 2
    double m_ceiling = 1.0; ///< the final clip
    double m_target = 1.0;  ///< what the gain aims the detected true peak at (a little under the ceiling)
    double m_releaseMs = 50.0;
    double m_release = 0.0;

    std::array<TruePeakDetector, kMaxDspChannels> m_detectors{};
    // Audio delay line: m_latency + 1 frames.
    std::unique_ptr<float[]> m_delay;
    int m_delayFrames = 0;
    int m_delayPos = 0;
    // Sliding minimum of the required gain over m_holdLength samples (monotonic deque in a ring).
    std::unique_ptr<double[]> m_dequeValue;
    std::unique_ptr<std::int64_t[]> m_dequeIndex;
    int m_dequeCapacity = 0;
    int m_dequeHead = 0;
    int m_dequeSize = 0;
    int m_holdLength = 0;
    // Box filter over the held minimum (m_boxLength samples).
    std::unique_ptr<double[]> m_box;
    int m_boxLength = 0;
    int m_boxPos = 0;
    double m_boxSum = 0.0;
    std::int64_t m_sinceResync = 0;

    std::int64_t m_index = 0;
    double m_gain = 1.0;
};

} // namespace wabcut::audio::dsp
