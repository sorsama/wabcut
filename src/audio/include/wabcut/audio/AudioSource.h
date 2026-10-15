#pragma once

// The pull interface of the audio graph. Everything that produces sound for the engine (a timeline mixer, a
// clip reader fed by a decoder ring buffer, a test tone) implements AudioSource.

#include <wabcut/audio/TransportMap.h>

#include <wabcut/model/Time.h>

#include <cstdint>
#include <span>

namespace wabcut::audio {

/// A producer of interleaved float32 audio, pulled by the audio thread.
///
/// REAL-TIME CONTRACT. render() runs on the audio callback thread (or on the thread that drives an offline
/// render). It must not block, wait, lock a mutex, allocate or free memory, do file or network I/O, log, or
/// throw. Do all of that on another thread and hand results over through lock-free structures (see
/// SpscFrameRing, SeqLock and the std::atomic parameters of Mixer). Implementations must be noexcept in
/// fact as well as in signature: an escaping exception terminates the process.
///
/// Sources are shared with std::shared_ptr and kept alive by the engine's control side. The audio thread
/// never owns a reference, so a source is always destroyed on a non-real-time thread.
struct AudioSource {
    virtual ~AudioSource() = default;

    /// Fill the whole of `interleaved` (frames = interleaved.size() / channels, frame-major:
    /// L0 R0 L1 R1 ...) with audio that starts at source time `startTime`.
    ///
    /// `startTime` is the position of the first frame on the source's own timeline; consecutive calls during
    /// uninterrupted playback are contiguous (next startTime = startTime + frames samples). A discontinuity
    /// means the transport seeked. `startTime` need not be sample-aligned. The span can have any length,
    /// including zero; the engine never passes more than AudioDeviceConfig::maxBlockFrames frames.
    /// Write silence (zeros) for anything you cannot produce in time; never leave the buffer uninitialised.
    virtual void render(std::span<float> interleaved, int channels, Rate sampleRate, Ticks startTime) noexcept = 0;

    /// Optional variable-speed audition (preview only), same real-time contract as render(): fill `interleaved`
    /// with the source played forward at `speed` from `startTime`, pitch preserved; the call covers source time
    /// [startTime, startTime + speed x frames). The engine calls it for audible shuttle speeds (isAudibleShuttle) and
    /// renders silence when it returns false (the default).
    virtual bool renderAtSpeed(std::span<float> /*interleaved*/, int /*channels*/, Rate /*sampleRate*/,
                               Ticks /*startTime*/, TransportSpeed /*speed*/) noexcept {
        return false;
    }

    /// Whether the source already holds the audio from the target of the seek the engine numbered `seekGeneration`
    /// (AudioEngine::seek) on (P.1): the engine then ends that seek's preroll at once instead of holding the transport
    /// for the whole preroll asked for. Called on the audio thread (same contract as render()). Default: never, so
    /// the full preroll applies.
    virtual bool readyFor(std::uint64_t /*seekGeneration*/) const noexcept { return false; }
};

/// Shuttle speeds that are heard (time-stretched): forward from 1/4x to 4x. Reverse and faster speeds stay silent
/// (the clock still moves), as does an AudioSource without renderAtSpeed().
constexpr bool isAudibleShuttle(TransportSpeed speed) noexcept {
    const TransportSpeed s = speed.normalized();
    if (s.isZero() || s.isNormal() || s.isReverse()) {
        return false;
    }
    return static_cast<std::int64_t>(s.num) * 4 >= s.den && s.num <= static_cast<std::int64_t>(s.den) * 4;
}
inline constexpr double kMaxShuttleSpeed = 4.0;

} // namespace wabcut::audio
