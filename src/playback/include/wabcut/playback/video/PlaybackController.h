#pragma once

// PlaybackController: the transport and video side of playback (contract rev 2, "playback").
//
//   PlaybackController playback;
//   playback.setTransport(makeEngineTransport(engine)); // audio is the master clock (default: free-run)
//   playback.setSession(&session);
//   playback.setSequence(sequenceId);
//   monitor->setPlaybackController(&playback);         // ProgramMonitor shows playback.mailbox()
//   playback.setTransportSpeed({2, 1});                // L L: 2x forward
//
// It owns the CompositeMailbox and the Scheduler, and drives the TransportClock. Video follows
// TransportClock::transportTime() at the instant a frame becomes visible (display-latency compensation), so
// audio stays the master clock. Speeds follow Premiere's J/K/L ladder (+-1, 2, 4, 8 and +-1/4 for K-held slow
// play, any rational within +-32 accepted); the keymap's TransportGesture produces them. Seeks start a new
// mailbox epoch: seek() is a jump (black until the target is decoded), scrub() and stepFrames() hold the
// previous picture.
//
// Threading: every member is for the GUI/controller thread except stats(), position() and mailbox() (any
// thread).

#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/playback/video/Scheduler.h>
#include <wabcut/playback/video/TransportClock.h>
#include <wabcut/render/CompositeMailbox.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace wabcut::playback {

struct PlaybackOptions {
    SchedulerOptions scheduler{};
    /// Composites waiting for display (lookahead headroom).
    int mailboxCapacity = 8;
};

struct PlaybackStats {
    // Presentation (render thread, via the mailbox).
    std::uint64_t presented = 0;
    std::uint64_t duplicated = 0; ///< Refreshes that repeated the previous picture while the transport moved.
    std::uint64_t dropped = 0;    ///< Composites superseded before display.
    std::uint64_t late = 0;       ///< Composites displayed after their frame interval had ended.
    std::uint64_t blackFrames = 0;
    double frameAgeMs = 0.0; ///< Publish-to-display age of the latest new picture.
    double maxFrameAgeMs = 0.0;
    // Scheduling and decoding.
    std::uint64_t published = 0;
    std::uint64_t skippedLate = 0;
    std::uint64_t decodeErrors = 0;
    int activeSessions = 0;
    /// Stack effects bypassed in the latest composite (unknown or newer effects): a warning badge.
    int bypassedEffects = 0;
    /// What the latest composite was decoded from (SchedulerStats): the monitor's quality badge.
    int proxyLayers = 0;
    int originalLayers = 0;
    int offlineProxyLayers = 0;
    int decodeDivisor = 1;
    std::string decodeBackends;
    std::string renderBackend; ///< Reported by the monitor (setRenderBackend()).
    std::string transport;     ///< TransportClock::name().
};

class PlaybackController {
public:
    explicit PlaybackController(PlaybackOptions options = {});
    ~PlaybackController();
    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    // ---- Wiring ----
    /// The clock video follows and the target of transport commands (null = a new free-run transport).
    void setTransport(std::shared_ptr<TransportClock> transport);
    std::shared_ptr<TransportClock> transport() const;
    /// The edit to play (must outlive the controller or the next setSession()).
    void setSession(const EditSession* session);
    void setSequence(SequenceId sequence);
    SequenceId sequence() const { return m_sequence.load(); }
    /// What the program monitor shows. Any thread.
    const std::shared_ptr<render::CompositeMailbox>& mailbox() const { return m_mailbox; }

    // ---- Transport ----
    /// Play forward at 1x.
    void play();
    void pause();
    /// Space: play at 1x when stopped, stop when playing (any speed).
    void togglePlay();
    /// Rational speed; zero pauses. Rejected (false) when invalid or beyond +-32x.
    bool setTransportSpeed(TransportSpeed speed);
    /// Commanded speed, zero while paused.
    TransportSpeed transportSpeed() const;
    bool isPlaying() const;
    /// Pause and move by `frames` sequence frames (hold-previous display).
    void stepFrames(int frames);
    /// Jump: black until the target is decoded.
    void seek(Ticks position);
    /// Scrub: the previous picture stays until the target is decoded.
    void scrub(Ticks position);
    /// Timeline position presented now (any thread).
    Ticks position() const;
    /// A.8: whether forward playback stops (parks on the last frame) at the sequence end; the voice-over recorder
    /// turns it off for a pass (Scheduler::setStopAtEnd). Default true.
    void setStopAtEnd(bool stop) { m_scheduler->setStopAtEnd(stop); }
    bool stopAtEnd() const { return m_scheduler->stopAtEnd(); }

    // ---- Stats ----
    void setRenderBackend(std::string name);
    PlaybackStats stats() const;
    Scheduler& scheduler() { return *m_scheduler; }

private:
    class PresentationAdapter;

    void startEpoch(render::SeekDisplay display);
    void onBoundary(int direction);
    Rate sequenceRate() const;

    PlaybackOptions m_options;
    std::shared_ptr<render::CompositeMailbox> m_mailbox;
    std::shared_ptr<TransportClock> m_transport;
    std::unique_ptr<Scheduler> m_scheduler;
    std::atomic<const EditSession*> m_session{nullptr}; // also read by onBoundary() on the scheduler thread
    std::atomic<SequenceId> m_sequence{};
    mutable std::mutex m_mutex; // m_renderBackend, m_transport swaps
    std::string m_renderBackend;
};

} // namespace wabcut::playback
