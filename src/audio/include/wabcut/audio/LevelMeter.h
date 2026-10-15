#pragma once

// Peak / RMS level meter: written by the audio thread, read by the UI at display rate.

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace wabcut::audio {

/// Meter ballistics.
struct MeterBallistics {
    /// Peak release (fall-back) rate. Attack is instantaneous. 20 dB in 1.5 s is close to an IEC 60268-10
    /// type I PPM and reads well on a timeline mixer.
    float peakFallDbPerSecond = 20.0f / 1.5f;
    /// Time constant of the exponential RMS average (a VU-like 300 ms window by default).
    float rmsWindowSeconds = 0.3f;
};

/// Per-channel peak and RMS meter with a latched clip indicator.
///
/// Threading. process() and reset() belong to one thread at a time (the audio thread while playing). The
/// readers (peak(), rms(), clipped(), clearClip()) may run on any thread concurrently with it. Every published
/// value is an independent std::atomic<float>/<bool> accessed with memory_order_relaxed: the UI only needs
/// each number to be some recent value, no other memory is published through them, and they are never read
/// together as a consistent group, so stronger ordering would only add cost. The ballistics state itself is
/// private to the processing thread.
class LevelMeter {
public:
    static constexpr int kMaxChannels = 8;

    /// Defaults to stereo at 48 kHz; call configure() before use when that is wrong.
    LevelMeter() noexcept { configure(2, 48000, {}); }
    LevelMeter(int channels, int sampleRate, MeterBallistics ballistics = {}) noexcept {
        configure(channels, sampleRate, ballistics);
    }

    LevelMeter(const LevelMeter&) = delete;
    LevelMeter& operator=(const LevelMeter&) = delete;

    /// Set the layout and ballistics and clear all state. Not RT-safe with respect to concurrent process():
    /// call it before the meter is in use or from the processing thread itself. `channels` is clamped to
    /// [1, kMaxChannels] and `sampleRate` to at least 1.
    void configure(int channels, int sampleRate, MeterBallistics ballistics) noexcept;

    int channels() const noexcept { return m_channels; }

    /// Meter one block of interleaved audio with channels() channels (a trailing partial frame is ignored).
    /// RT-safe. NaN/inf samples are ignored, so a bad source cannot latch the meter.
    void process(std::span<const float> interleaved) noexcept;

    /// Account for `frames` of digital silence without touching a buffer (e.g. a muted or paused bus). RT-safe.
    void processSilence(std::int64_t frames) noexcept;

    /// Drop the ballistics state and publish zeros (the clip latch is kept). Processing thread only.
    void reset() noexcept;

    /// Linear peak level (1.0 = 0 dBFS; can exceed 1). Any thread. Out-of-range channels read 0.
    float peak(int channel) const noexcept;
    /// Linear RMS level. Any thread. Out-of-range channels read 0.
    float rms(int channel) const noexcept;
    /// True once any sample reached or exceeded 0 dBFS since the last clearClip(). Any thread.
    bool clipped() const noexcept { return m_clipped.load(std::memory_order_relaxed); }
    /// Reset the clip latch (the user clicked the clip LED). Any thread.
    void clearClip() const noexcept { m_clipped.store(false, std::memory_order_relaxed); }

private:
    void publish() noexcept;

    int m_channels = 2;
    float m_peakFallPerFrame = 1.0f; // linear factor applied per frame of release
    double m_rmsCoefficient = 1.0;   // one-pole smoothing coefficient per sample

    // Processing-thread state.
    std::array<float, kMaxChannels> m_peakState{};
    std::array<double, kMaxChannels> m_meanSquare{};

    // Published values.
    std::array<std::atomic<float>, kMaxChannels> m_peak{};
    std::array<std::atomic<float>, kMaxChannels> m_rms{};
    mutable std::atomic<bool> m_clipped{false};
};

} // namespace wabcut::audio
