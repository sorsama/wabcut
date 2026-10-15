#pragma once

// Pitch-preserving time stretch for variable-speed audition (preview only): WSOLA, waveform-similarity overlap-add
// (Verhelst and Roelands, ICASSP 1993). Each synthesis frame is a Hann-windowed segment of the input taken near the
// position the speed asks for, shifted within a tolerance to the offset that best continues the previous segment, and
// overlap-added at a fixed hop. Low latency (one 20 ms window) and cheap, good on speech, acceptable on music.

#include <memory>

namespace wabcut::audio::dsp {

/// Streaming WSOLA. Push input at the timeline rate, pull output at the device rate; over time it consumes speed()
/// input frames per output frame. All memory is allocated in the constructor: push(), pull(), setSpeed() and reset()
/// are RT-safe.
class TimeStretch {
public:
    /// `maxPushFrames` bounds one push() (the caller's largest block times the highest speed).
    TimeStretch(int channels, double sampleRate, double maxSpeed = 4.0, int maxPushFrames = 8192);
    ~TimeStretch();
    TimeStretch(const TimeStretch&) = delete;
    TimeStretch& operator=(const TimeStretch&) = delete;

    /// Speed in [1/8, maxSpeed]: input frames per output frame. Takes effect at the next synthesis hop.
    void setSpeed(double speed) noexcept;
    double speed() const noexcept;

    /// Appends `frames` interleaved input frames (at most maxPushFrames per call; more is dropped).
    void push(const float* interleaved, int frames) noexcept;
    /// Writes up to `frames` output frames; returns how many were available (the rest of the span is untouched).
    int pull(float* interleaved, int frames) noexcept;
    /// Output frames ready without more input.
    int available() const noexcept;
    /// Input frames that must be pushed before the first output (one window plus the search tolerance).
    int latencyInputFrames() const noexcept;

    /// Forgets all input and output: the next push starts a new stream (its first frames fade in over one hop).
    void reset() noexcept;

    /// Totals since the last reset, for duration checks.
    long long inputFramesPushed() const noexcept;
    long long outputFramesProduced() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::audio::dsp
