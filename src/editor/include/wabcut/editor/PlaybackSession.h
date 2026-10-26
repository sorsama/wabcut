#pragma once

// PlaybackSession: picture and sound for one EditSession — the AudioEngine (the master clock), the
// TimelineAudioPlayer fed by a MediaAudioFeed, and the video PlaybackController following the engine's clock.
//
//   editor::PlaybackSession playback(session, {});      // AudioBackend::Default; tests use Null
//   playback.start();
//   playback.setSequence(sequenceId);
//   monitor->setPlaybackController(&playback.video());  // ProgramMonitor shows the video side
//   playback.togglePlay();                              // space; J/K/L go through setSpeed()
//
// Transport commands go through the video controller to a TransportClock that forwards them to the
// TimelineAudioPlayer (so its PCM rings are flushed and refilled on seeks) and reads time from the engine:
// video always follows audio. When the audio device cannot start (and no fallback is allowed) playback still
// runs, on a free-run clock without sound.
//
// Threading: like PlaybackController, every member is for the GUI thread except position() and stats().

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/playback/audio/AudioScrubber.h>
#include <wabcut/playback/audio/TimelineAudioPlayer.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/playback/video/TransportClock.h>

#include <expected>
#include <memory>
#include <string>

namespace wabcut::editor {

struct PlaybackSessionOptions {
    audio::AudioDeviceConfig audio{};
    playback::TimelineAudioOptions timelineAudio{};
    playback::PlaybackOptions video{};
    /// Preroll the engine holds on a seek while the feeder refills.
    Ticks seekPreroll{kTicksPerSecond / 10};
    /// Open decoders the audio feed keeps.
    std::size_t maxAudioDecoders = 16;
    /// 0.5: audio scrubbing (its own feed keeps this many decoders open).
    playback::AudioScrubOptions scrub{};
    std::size_t maxScrubDecoders = 8;
};

struct PlaybackSessionStats {
    playback::PlaybackStats video;
    playback::TimelineAudioStats audio;
    audio::AudioEngineStats engine;
    std::string audioBackend; ///< "WASAPI", "Null", ... ("" without sound)
    bool hasSound = false;
};

class PlaybackSession {
public:
    /// `session` must outlive the playback session.
    explicit PlaybackSession(const EditSession& session, PlaybackSessionOptions options = {});
    ~PlaybackSession();
    PlaybackSession(const PlaybackSession&) = delete;
    PlaybackSession& operator=(const PlaybackSession&) = delete;

    /// Starts the audio engine and the timeline audio player and connects the transport. Without a working
    /// audio device (and no Null fallback) the error is returned but the video side runs on a free-run clock.
    std::expected<void, Error> start();
    void stop();
    bool hasSound() const { return m_audioStarted; }

    void setSequence(SequenceId sequence);
    SequenceId sequence() const { return m_sequence; }

    // ---- Transport (GUI thread) ----
    void play();
    void pause();
    void togglePlay();
    /// J/K/L ladder speed (zero pauses).
    bool setSpeed(playback::TransportSpeed speed);
    playback::TransportSpeed speed() const;
    bool isPlaying() const;
    void stepFrames(int frames);
    /// Jump (black until decoded) and scrub (holds the previous picture).
    void seek(Ticks position);
    void scrub(Ticks position);
    /// Position presented now (any thread).
    Ticks position() const;
    /// P.2, Premiere's Loop: playback wraps inside `range` (sound and picture); nullopt turns it off.
    void setLoop(std::optional<TimeRange> range);
    std::optional<TimeRange> loop() const;
    /// 0.5, Premiere's audio scrubbing: a short, faded window of the sequence mix at `at`, heard once
    /// (playback::AudioScrubber: rendered on its worker, rate-limited, the latest request wins). The transport clock and
    /// the picture do not move. Only while stopped and with sound; returns whether it was asked for. `direction` < 0 (a
    /// backward drag): the window before `at`, played backwards from `at`.
    bool scrubAudio(Ticks at, int direction = 1);
    /// The scrubber while the session has sound (else null): its stats for tests and diagnostics.
    playback::AudioScrubber* audioScrubber() { return m_audioStarted && m_scrubber->isStarted() ? m_scrubber.get() : nullptr; }

    PlaybackSessionStats stats() const;

    playback::PlaybackController& video() { return *m_video; }
    audio::AudioEngine& engine() { return *m_engine; }
    /// Null until start() succeeded with sound.
    playback::TimelineAudioPlayer* audioPlayer() { return m_audioStarted ? m_player.get() : nullptr; }

private:
    const EditSession& m_session;
    PlaybackSessionOptions m_options;
    SequenceId m_sequence;
    std::unique_ptr<audio::AudioEngine> m_engine;
    std::shared_ptr<playback::AudioFeed> m_feed;
    std::unique_ptr<playback::TimelineAudioPlayer> m_player;
    std::unique_ptr<playback::PlaybackController> m_video;
    /// 0.5: audio scrubbing, on a feed of its own (feeds are not shared between threads).
    std::shared_ptr<playback::AudioFeed> m_scrubFeed;
    std::unique_ptr<playback::AudioScrubber> m_scrubber;
    bool m_audioStarted = false;
};

/// A TransportClock for video that reads time from `engine` (latency compensated) and sends commands to
/// `player`, which forwards them to the same engine and keeps its rings consistent. Commands may come from the
/// GUI thread and from the scheduler (end of sequence); they are serialized. Both must outlive the clock. 0.5: with a
/// `scrubber`, play and a speed change cancel its window (a scrub never sounds over playback); it must outlive the clock.
std::shared_ptr<playback::TransportClock> makeTimelineAudioTransport(playback::TimelineAudioPlayer& player,
                                                                     audio::AudioEngine& engine,
                                                                     playback::AudioScrubber* scrubber = nullptr);

} // namespace wabcut::editor
