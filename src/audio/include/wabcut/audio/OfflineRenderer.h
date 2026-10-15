#pragma once

// Deterministic, faster-than-real-time pull of an audio graph: used by export and by tests.

#include <wabcut/audio/AudioSource.h>

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace wabcut::audio {

/// Pulls an AudioSource block by block exactly like the audio thread does, with no device, no clock and no
/// threads. Rendering the same range twice produces identical samples (provided the source is deterministic).
///
/// Threading: an OfflineRenderer is used from one thread at a time; it does the source's real-time work on
/// that thread, so the source must not also be attached to a running AudioEngine unless it supports
/// concurrent render() calls (SineSource and SilenceSource do).
class OfflineRenderer {
public:
    /// Fails with InvalidArgument for a null source, channels outside [1, 64], an inexact or invalid sample
    /// rate, or maxBlockFrames < 1.
    static std::expected<OfflineRenderer, Error> create(std::shared_ptr<AudioSource> source, int channels,
                                                       Rate sampleRate, int maxBlockFrames = 1024);

    int channels() const noexcept { return m_channels; }
    Rate sampleRate() const noexcept { return m_sampleRate; }

    /// Source time of the next frame to be rendered.
    Ticks position() const noexcept { return m_position; }
    /// Jump to `position` (a discontinuity for the source, like a transport seek).
    void seek(Ticks position) noexcept { m_position = position; }

    /// Render interleaved.size() / channels() frames and advance position(). Fails with InvalidArgument (and
    /// renders nothing) if the size is not a whole number of frames.
    std::expected<void, Error> render(std::span<float> interleaved) noexcept;

    /// Convenience: render `frames` frames into a new buffer (allocates).
    std::expected<std::vector<float>, Error> render(std::int64_t frames);

private:
    OfflineRenderer(std::shared_ptr<AudioSource> source, int channels, Rate sampleRate, int maxBlockFrames) noexcept;

    std::shared_ptr<AudioSource> m_source;
    int m_channels;
    Rate m_sampleRate;
    int m_maxBlockFrames;
    Ticks m_position{};
};

} // namespace wabcut::audio
