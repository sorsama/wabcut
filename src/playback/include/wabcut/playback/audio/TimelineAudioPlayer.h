#pragma once

// TimelineAudioPlayer: plays a sequence's audio tracks through the AudioEngine (the audio half of playback).

#include <wabcut/playback/audio/AudioFeed.h>
#include <wabcut/playback/audio/TrackAudio.h>

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/LevelMeter.h>
#include <wabcut/audio/LoudnessTap.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

namespace wabcut::playback {

struct TimelineAudioOptions {
    /// Audio tracks the player can mix (A1..An); tracks beyond are ignored and counted in stats.
    int maxTracks = 16;
    /// How far ahead of the playhead the feeder keeps every track's ring filled.
    Ticks leadTime{kTicksPerSecond / 4};
    /// Preroll requested with every seek(): the transport holds silent while the feeder fills the rings, at most this
    /// long: it ends as soon as every track is refilled from the target (P.1; AudioSource::readyFor).
    Ticks preroll{kTicksPerSecond / 10};
    /// How far ahead of the playhead an edit takes over (old audio plays until then; must be well below
    /// leadTime and above one callback + one feeder pass).
    Ticks editCutover{kTicksPerSecond / 25};
    /// Fades at clip cuts (TrackRenderOptions::edgeFade) and de-click fades inside the track streams.
    Ticks edgeFade{kTicksPerSecond / 500};
    /// Feeder write granularity in frames.
    int chunkFrames = 512;
    /// Run a feeder thread (production). false: nothing feeds until the owner calls feedOnce(), which makes
    /// tests deterministic.
    bool feederThread = true;
    /// Feeder pass interval when nothing wakes it earlier.
    std::chrono::milliseconds feederInterval{5};
};

/// Counters for diagnostics UI and tests (each field read independently).
struct TimelineAudioStats {
    std::uint64_t feederPasses = 0;
    std::uint64_t framesFed = 0;      ///< frames written into track rings
    std::uint64_t underrunFrames = 0; ///< track frames the callback needed but no data was queued (silence)
    std::uint64_t skippedFrames = 0;  ///< stale queued frames dropped by the callback
    std::uint64_t seeks = 0;
    std::uint64_t resyncs = 0;        ///< transport jumps not announced through seek() (or an overrun feeder)
    std::uint64_t editFlushes = 0;    ///< track flushes caused by snapshot changes
    std::uint64_t feedErrors = 0;     ///< AudioFeed::read failures (rendered as silence)
    std::uint64_t droppedTracks = 0;  ///< audio tracks beyond maxTracks in the current sequence
    int activeTracks = 0;
};

/// A meter's readings copied at one moment (TimelineAudioPlayer::trackMeter / masterMeter): valid whatever the player
/// does next. Review A5-P2-3: a reference into the player's source dangled once a change of the master's format
/// replaced that source.
class MeterSnapshot {
public:
    MeterSnapshot() = default;
    explicit MeterSnapshot(const audio::LevelMeter& meter) noexcept;

    int channels() const noexcept { return m_channels; }
    /// Linear peak and RMS of `channel` (0 when out of range), as audio::LevelMeter reports them.
    float peak(int channel) const noexcept { return inRange(channel) ? m_peak[index(channel)] : 0.0f; }
    float rms(int channel) const noexcept { return inRange(channel) ? m_rms[index(channel)] : 0.0f; }
    bool clipped() const noexcept { return m_clipped; }

private:
    bool inRange(int channel) const noexcept { return channel >= 0 && channel < m_channels; }
    static std::size_t index(int channel) noexcept { return static_cast<std::size_t>(channel); }
    int m_channels = 0;
    std::array<float, audio::LevelMeter::kMaxChannels> m_peak{};
    std::array<float, audio::LevelMeter::kMaxChannels> m_rms{};
    bool m_clipped = false;
};

/// Plays the audio tracks of one sequence of an EditSession through an AudioEngine.
///
/// Pipeline: a feeder thread acquires the session snapshot once per pass (Principle 3), renders each audio
/// track ahead of the playhead with renderTrack() (clip volume/pan, sample-accurate edges, cut fades) through
/// the AudioFeed, and writes it into a per-track SPSC ring tagged with timeline times, before the track's fader. The
/// audio callback runs the player's source, which reads every track's ring at the transport's timeline time, applies
/// each track's fader and panner (Track::volume / Track::pan, published by the feeder; an automation pass's live
/// override instead while one is set: setLiveTrackMix) and mixes the tracks in an
/// audio::Mixer (track mute/solo/enable as in Premiere: muted or disabled tracks are silent, and while any track is
/// soloed only soloed tracks play; mute beats solo; no track is solo-safe), then the master bus: the sequence's master
/// effects and master fader (Sequence::masterVolume), the master meter and the soft clip guard. The result feeds the
/// live loudness tap (loudness()).
///
/// Transport: drive it through the player (play, pause, seek, setSpeed, setLoop), which forwards to the
/// AudioEngine (whose TransportMap stays the master clock) and keeps the rings consistent:
///   * seek: latest-seek-wins epoch; every ring is flushed and refilled from the target during the preroll;
///   * edits (snapshot changes): the model's ChangeSet (diff() of the previous and the new snapshot) names the
///     changed tracks and time ranges. A track is refilled only where a changed range overlaps audio already
///     queued, from that range's start but never before `editCutover` ahead of the playhead; old audio plays
///     until then, so an edit is heard within one lead window and without a gap. Edits outside the queued
///     audio cost nothing; mute/solo/enable go straight to the mixer, the faders and panners to the callback.
///   * speed: the MVP renders only at 1/1 forward (the engine mutes other speeds; the clock still moves);
///   * transport jumps the player was not told about are detected by the callback and resynced.
/// Linked A/V needs nothing special: audio clips are evaluated on their own.
///
/// Threading: the control functions belong to one non-audio thread (normally the GUI/playback thread). The
/// meters and stats() may be read from any thread. Nothing on the callback path locks or allocates.
class TimelineAudioPlayer {
public:
    /// `engine` and the session (setSession) must outlive the player. Allocates all rings and buffers.
    TimelineAudioPlayer(audio::AudioEngine& engine, std::shared_ptr<AudioFeed> feed, TimelineAudioOptions options = {});
    ~TimelineAudioPlayer(); ///< stop()s.

    TimelineAudioPlayer(const TimelineAudioPlayer&) = delete;
    TimelineAudioPlayer& operator=(const TimelineAudioPlayer&) = delete;

    void setSession(const EditSession* session);
    void setSequence(SequenceId sequence);

    /// Installs the player's source in the engine (which fixes the channel count and rate: the engine's
    /// config()) and starts the feeder at the current transport position. Fails if the engine's format is not
    /// supported or the player is already started.
    std::expected<void, Error> start();
    /// Removes the source from the engine and stops the feeder.
    void stop();
    bool isStarted() const noexcept;

    // ---- Transport (forwarded to the engine) ---------------------------------------------------------------
    void play();
    void pause();
    /// Latest seek wins: rings flush, the engine prerolls, the feeder refills from `target`.
    void seek(Ticks target);
    void setSpeed(audio::TransportSpeed speed);
    void setLoop(std::optional<TimeRange> range);
    /// The master clock (AudioEngine::clock()).
    Ticks position() const noexcept;

    // ---- Live fader overrides (automation passes: Touch / Latch / Write) ------------------------------------
    /// Overrides the fader (dB) and/or panner (-1..1) of audio track `track` (p02: or submix track; TrackId{}: the master fader; the master
    /// has no panner of its own, so a pan there is a balance on the mix) while set, heard at once: the callback applies
    /// it instead of the evaluated Track::volume / pan (or Sequence::masterVolume), ramped over ~10 ms on every change
    /// and on the way in and out. Both unset clears it. The model is never touched: the UI writes the pass as keys.
    /// Cleared by clearLiveMix(), stop() and whenever the transport is paused (the pass ends with playback, however it
    /// was stopped: within a feeder pass), and when the slot plays another track. Returns false when the
    /// player is not started or does not play `track`. Any thread; RT-safe publication (atomics).
    bool setLiveTrackMix(TrackId track, std::optional<double> gainDb, std::optional<double> pan);
    void clearLiveMix() noexcept;

    /// Manual-feed mode (options.feederThread == false): run one feeder pass now. Also callable in threaded mode
    /// to force a pass (e.g. right after an edit). Not on the audio thread.
    void feedOnce();

    /// A.8 / review A5-P2-3: hands the engine `wrap(program)` in place of `program`, the source it plays now (a
    /// voice-over pass wraps the program it plays), atomically with the player's own source changes: a change of the
    /// master's format waits while the engine plays anything but the player's source, so the wrapper never plays a
    /// source the player no longer feeds. Returns `program` (null and nothing installed when `wrap` returns null).
    /// Not on the audio thread; waits for a running feeder pass.
    using SourceWrap = std::function<std::shared_ptr<audio::AudioSource>(std::shared_ptr<audio::AudioSource> program)>;
    std::shared_ptr<audio::AudioSource> wrapEngineSource(const SourceWrap& wrap);
    /// Ends a wrap: when the engine still plays `wrapper`, it plays the player's current source again (`program` when
    /// the player is not started).
    void unwrapEngineSource(const std::shared_ptr<audio::AudioSource>& wrapper,
                            const std::shared_ptr<audio::AudioSource>& program);

    // ---- Meters and statistics (any thread) ---------------------------------------------------------------
    /// Post-fader meter of audio track `index` (0 = A1); a silent meter when out of range.
    MeterSnapshot trackMeter(int index) const noexcept;
    MeterSnapshot masterMeter() const noexcept;
    /// Resets a meter's clip latch (the user clicked its clip LED).
    void clearTrackClip(int index) const noexcept;
    void clearMasterClip() const noexcept;
    /// p02 audio: the post-fader meter of submix track `index` (0 = the sequence's first submix); silent when out of
    /// range or not routed to.
    MeterSnapshot submixMeter(int index) const noexcept;
    void clearSubmixClip(int index) const noexcept;

    /// A.3: live EBU R128 loudness of what leaves the player (post master effects, master fader and soft clip guard:
    /// what export renders), measured by an audio::LoudnessTap on its own worker thread (manual-feed mode: by
    /// feedOnce()): momentary (400 ms), short-term (3 s), integrated and true peak since play() started from pause or
    /// since resetLoudness(). Shuttle audition is not measured. Any thread; a default reading before start().
    audio::LoudnessReading loudness() const noexcept;
    void resetLoudness() noexcept;
    /// Off saves the tap's work when no loudness meter is shown (readings keep their last values). On by default.
    void setLoudnessEnabled(bool enabled) noexcept;
    TimelineAudioStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::playback
