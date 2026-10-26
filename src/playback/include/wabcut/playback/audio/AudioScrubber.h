#pragma once

// AudioScrubber (0.5): Premiere's audio scrubbing (Toggle Audio Scrubbing, Shift+S). While the transport is stopped,
// dragging the playhead and stepping frames play a short window of the sequence mix at the new position.
//
//   * scrub(at), from the control thread, never blocks: the latest request wins. A worker thread renders the window
//     through OfflineMixdown, the preview == export path (track mute and solo, clip and track effects, faders and pan,
//     submixes, the master bus, redactions, enhanced takes, nested sequences): max(one frame of the sequence,
//     minWindow), at most maxWindow, faded in and out over `fade`.
//   * Direction (follow-up): a backward drag (direction < 0) plays the window that ends at the playhead backwards, from
//     the playhead back in time, tape-like (Premiere's backward scrub); the fades are applied after the reversal.
//   * Rate limit: at most one window starts every minInterval. Requests that come in between replace the queued one, so
//     a fast drag hears where the playhead is now, not a backlog of where it was.
//   * Output: the AudioEngine's overlay (AudioEngine::setOverlay), mixed into the device output whatever the transport
//     does. The transport map and the master clock never move for it, so neither does the picture. A window that starts
//     while another still sounds fades that one out over `fade` (no click); a request while playing is dropped.
//   * Threading: scrub(), cancel(), setSession() and setSequence() from one control thread (the GUI's). The worker owns
//     the mixdown and its feed, which must not be the player's (feeds are not thread-safe). The overlay's render() is
//     real-time safe: windows are allocated on the worker, handed to the audio thread through a lock-free queue and
//     handed back through another to be freed on the worker.
//   * Tests: workerThread = false, then process(now) renders and hands over with an injected time (rate limit and
//     replacement are deterministic); a Null-backend engine is pulled block by block.

#include <wabcut/playback/audio/AudioFeed.h>
#include <wabcut/playback/audio/TrackRenderer.h>

#include <wabcut/audio/AudioEngine.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>

namespace wabcut::playback {

struct AudioScrubOptions {
    /// The shortest window (Premiere plays about a frame; shorter than this is a click, not a word).
    Ticks minWindow{kTicksPerSecond * 3 / 50}; // 60 ms
    /// The longest window (a sequence at a very low frame rate).
    Ticks maxWindow{kTicksPerSecond / 5}; // 200 ms
    /// The fade in and out of every window, and the fade-out of one a newer window cuts short.
    Ticks fade{kTicksPerSecond / 200}; // 5 ms
    /// At most one window starts this often.
    std::chrono::milliseconds minInterval{30};
    /// Effects settle on this much audio before a window (the player's 0.5 s / 2 s are for sample-exact export; a
    /// scrub is a preview and must answer at once).
    Ticks effectPreroll{kTicksPerSecond / 20};
    Ticks clipPreroll{kTicksPerSecond / 10};
    /// Run a worker thread (production). false: nothing renders until the owner calls process() (tests).
    bool workerThread = true;
};

/// Counters for tests and diagnostics (each field read on its own).
struct AudioScrubStats {
    std::uint64_t requests = 0;   ///< scrub() calls taken
    std::uint64_t replaced = 0;   ///< queued requests a newer one replaced before they were rendered
    std::uint64_t rendered = 0;   ///< windows rendered and handed to the overlay
    std::uint64_t dropped = 0;    ///< requests not played (playing, no sequence, the overlay's queue full)
    std::uint64_t workerPasses = 0; ///< times the worker woke and ran a pass (a rate-limited wait is one, not a spin)
    std::uint64_t started = 0;    ///< windows the audio thread began to play
    std::uint64_t cutShort = 0;   ///< windows faded out early (a newer window, cancel())
    std::uint64_t skipped = 0;    ///< windows a newer one replaced before the audio thread played them
    std::uint64_t framesPlayed = 0;
    std::int64_t lastWindowFrames = 0;
    Ticks lastWindowStart; ///< where the last window starts in the sequence (a reversed one: its earliest frame)
    bool lastWindowReversed = false;
    /// From the request of the last window rendered to its hand-over (rate-limit wait plus rendering), microseconds:
    /// what scrubbing adds to the output device's latency.
    std::int64_t lastLatencyMicros = 0;
};

class AudioScrubber {
public:
    /// `engine` must outlive the scrubber; `feed` is the worker's own (not the player's).
    AudioScrubber(audio::AudioEngine& engine, std::shared_ptr<AudioFeed> feed, AudioScrubOptions options = {});
    ~AudioScrubber(); ///< stop()s.
    AudioScrubber(const AudioScrubber&) = delete;
    AudioScrubber& operator=(const AudioScrubber&) = delete;

    /// The session must outlive the scrubber (or be unset first).
    void setSession(const EditSession* session);
    /// The sequence whose mix is heard; a change drops what is queued.
    void setSequence(SequenceId sequence);

    /// Installs the overlay in the running engine (its format: the engine's config()) and starts the worker. Fails when
    /// the engine is not running or the scrubber is already started.
    std::expected<void, Error> start();
    /// Removes the overlay and stops the worker. Idempotent.
    void stop();
    bool isStarted() const noexcept;

    /// Hear the sequence at `at` (negative: 0). Never blocks; ignored while not started. `direction` < 0 (a backward drag):
    /// the window before `at`, played backwards from `at`.
    void scrub(Ticks at, int direction = 1);
    /// Drops the queued request and fades out the window sounding (playback starts, the sequence changes).
    void cancel();

    /// Manual mode (workerThread false; also usable with the worker for a pass now): hands the stop of a cancel(),
    /// renders and hands over the queued request when the rate limit allows at `now`, and frees the windows the audio
    /// thread is done with. Returns whether a window was handed over.
    bool process(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    /// The window length at a sequence frame rate: max(one frame, minWindow), at most maxWindow.
    Ticks windowLength(Rate frameRate) const noexcept;
    AudioScrubStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::playback
