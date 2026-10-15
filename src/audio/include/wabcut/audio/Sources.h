#pragma once

// Basic AudioSource implementations: silence, a test tone, and the ring-buffer reader that connects a decoder
// thread to the audio thread.

#include <wabcut/audio/AudioSource.h>
#include <wabcut/audio/SpscRingBuffer.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace wabcut::audio {

/// Digital silence. Stateless; RT-safe.
class SilenceSource final : public AudioSource {
public:
    void render(std::span<float> interleaved, int channels, Rate sampleRate, Ticks startTime) noexcept override;
};

/// Sine test tone, identical on every channel. The phase is a pure function of source time
/// (phase = frequency · t), so the output is deterministic, seek-accurate and stateless: two renders of the
/// same time range with the same block sizes are bit-identical (different block sizes agree to ~1e-7), and
/// the source can be shared between engines and render() called concurrently. RT-safe.
class SineSource final : public AudioSource {
public:
    /// `amplitude` is linear peak (0.5 = -6 dBFS). Non-finite or negative values are treated as 0.
    explicit SineSource(double frequencyHz, float amplitude = 0.5f) noexcept;

    double frequency() const noexcept { return m_frequency; }
    float amplitude() const noexcept { return m_amplitude; }

    void render(std::span<float> interleaved, int channels, Rate sampleRate, Ticks startTime) noexcept override;

private:
    double m_frequency;
    float m_amplitude;
};

/// Streams frames out of an SpscFrameRing that a decoder (or any producer thread) fills. The audio thread is
/// the ring's consumer, so render() must only ever be called from one thread. `startTime` is ignored: the
/// producer is responsible for queueing audio from the position it was asked to play (prime the ring after a
/// seek, then call AudioEngine::setTimeline / resume). Missing frames are rendered as silence and counted.
class RingBufferSource final : public AudioSource {
public:
    explicit RingBufferSource(std::shared_ptr<SpscFrameRing> ring) noexcept;

    const std::shared_ptr<SpscFrameRing>& ring() const noexcept { return m_ring; }

    /// Frames rendered as silence because the ring was empty (or had the wrong channel count). Any thread.
    std::uint64_t underrunFrames() const noexcept { return m_underrunFrames.load(std::memory_order_relaxed); }

    void render(std::span<float> interleaved, int channels, Rate sampleRate, Ticks startTime) noexcept override;

private:
    std::shared_ptr<SpscFrameRing> m_ring;
    std::atomic<std::uint64_t> m_underrunFrames{0};
};

} // namespace wabcut::audio
