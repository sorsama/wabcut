#pragma once

// TransportClock: the transport video follows (docs/architecture.md, "playback"; audio is the master clock).
//
// Video never owns time. The PlaybackController issues transport commands through this interface, and the
// scheduler and the program monitor read where the timeline is at a given steady-clock instant. Two
// implementations:
//   * makeEngineTransport(engine): commands go to an audio::AudioEngine; time is AudioEngine::clockAt(), i.e.
//     TransportMap::transportTime() of the sample being heard (latency compensated). Integration with the
//     timeline audio player can supply its own implementation that forwards the commands to
//     TimelineAudioPlayer (so its PCM rings are flushed on seeks) while time still comes from the shared
//     engine: no coupling beyond the clock.
//   * makeFreeRunTransport(): a steady-clock transport for headless use (tests, benchmarks, no audio device).
//
// Threading: queries are lock-free and callable from any thread (the render thread calls them on every
// refresh). Commands are made from one controller thread at a time.

#include <wabcut/audio/TransportMap.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <memory>
#include <string>

namespace wabcut::audio {
class AudioEngine;
}

namespace wabcut::playback {

using audio::TransportSpeed;

/// Steady-clock now, in nanoseconds since the steady clock's epoch (the time base of every hostNs argument).
std::int64_t steadyNowNs() noexcept;

class TransportClock {
public:
    virtual ~TransportClock() = default;

    // ---- Queries: any thread, lock-free ----
    /// Timeline time presented at steady time `hostNs` (seen/heard then), never negative.
    virtual Ticks transportTime(std::int64_t hostNs) const noexcept = 0;
    /// Commanded speed while playing; zero() while paused.
    virtual TransportSpeed effectiveSpeed() const noexcept = 0;
    bool isPlaying() const noexcept { return !effectiveSpeed().isZero(); }

    // ---- Commands: controller thread ----
    virtual void play() = 0;
    virtual void pause() = 0;
    /// Speed used while playing (normalized; invalid or zero speeds are ignored: use pause()).
    virtual void setSpeed(TransportSpeed speed) = 0;
    /// Discontinuity: the transport continues (playing or paused) from `target`.
    virtual void seek(Ticks target) = 0;

    /// "audio engine (WASAPI)", "free-run", ... for stats.
    virtual std::string name() const = 0;
};

/// Video follows `engine` (which must outlive the clock). `seekPreroll` is passed to AudioEngine::seek() so
/// decoders get time to fill before audio starts again.
std::shared_ptr<TransportClock> makeEngineTransport(audio::AudioEngine& engine, Ticks seekPreroll = {});
/// A steady-clock transport, paused at `position`.
std::shared_ptr<TransportClock> makeFreeRunTransport(Ticks position = {});

} // namespace wabcut::playback
