#include <wabcut/editor/PlaybackSession.h>

#include <wabcut/playback/audio/MediaAudioFeed.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

namespace wabcut::editor {

namespace {

std::uint64_t packSpeed(playback::TransportSpeed s) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.num)) << 32) |
           static_cast<std::uint32_t>(s.den);
}

playback::TransportSpeed unpackSpeed(std::uint64_t v) {
    return {static_cast<std::int32_t>(static_cast<std::uint32_t>(v >> 32)),
            static_cast<std::int32_t>(static_cast<std::uint32_t>(v & 0xffffffffu))};
}

class TimelineAudioTransport final : public playback::TransportClock {
public:
    TimelineAudioTransport(playback::TimelineAudioPlayer& player, audio::AudioEngine& engine)
        : m_player(player), m_engine(engine) {
        const playback::TransportSpeed speed = engine.speed().normalized();
        m_speed.store(packSpeed(speed.isZero() ? playback::TransportSpeed::normal() : speed));
        m_playing.store(!engine.isPaused());
    }

    Ticks transportTime(std::int64_t hostNs) const noexcept override {
        const auto tp = std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(hostNs)));
        return Ticks{std::max<std::int64_t>(0, m_engine.clockAt(tp).value)};
    }

    playback::TransportSpeed effectiveSpeed() const noexcept override {
        return m_playing.load(std::memory_order_acquire) ? unpackSpeed(m_speed.load(std::memory_order_acquire))
                                                         : playback::TransportSpeed::zero();
    }

    void play() override {
        std::lock_guard lock(m_commands);
        m_player.play();
        m_playing.store(true, std::memory_order_release);
    }
    void pause() override {
        std::lock_guard lock(m_commands);
        m_player.pause();
        m_playing.store(false, std::memory_order_release);
    }
    void setSpeed(playback::TransportSpeed speed) override {
        const playback::TransportSpeed n = speed.normalized();
        if (n.isZero()) {
            return;
        }
        std::lock_guard lock(m_commands);
        m_player.setSpeed(n);
        m_speed.store(packSpeed(n), std::memory_order_release);
    }
    void seek(Ticks target) override {
        std::lock_guard lock(m_commands);
        m_player.seek(Ticks{std::max<std::int64_t>(0, target.value)});
    }
    std::string name() const override {
        const std::string backend = m_engine.backendName();
        return backend.empty() ? std::string("timeline audio") : "timeline audio (" + backend + ")";
    }

private:
    playback::TimelineAudioPlayer& m_player;
    audio::AudioEngine& m_engine;
    std::mutex m_commands; // GUI thread and the scheduler's end-of-sequence stop
    std::atomic<std::uint64_t> m_speed{0};
    std::atomic<bool> m_playing{false};
};

} // namespace

std::shared_ptr<playback::TransportClock> makeTimelineAudioTransport(playback::TimelineAudioPlayer& player,
                                                                     audio::AudioEngine& engine) {
    return std::make_shared<TimelineAudioTransport>(player, engine);
}

PlaybackSession::PlaybackSession(const EditSession& session, PlaybackSessionOptions options)
    : m_session(session), m_options(std::move(options)) {
    m_engine = std::make_unique<audio::AudioEngine>();
    m_feed = std::make_shared<playback::MediaAudioFeed>(session, m_options.maxAudioDecoders);
    playback::TimelineAudioOptions audioOptions = m_options.timelineAudio;
    audioOptions.preroll = m_options.seekPreroll;
    m_player = std::make_unique<playback::TimelineAudioPlayer>(*m_engine, m_feed, audioOptions);
    m_player->setSession(&session);
    m_video = std::make_unique<playback::PlaybackController>(m_options.video);
    m_video->setSession(&session);
}

PlaybackSession::~PlaybackSession() {
    stop();
    m_video.reset(); // its scheduler may still read the transport, which refers to the player and the engine
    m_player.reset();
}

std::expected<void, Error> PlaybackSession::start() {
    if (m_audioStarted) {
        return {};
    }
    if (auto started = m_engine->start(m_options.audio); !started) {
        m_video->setTransport(playback::makeFreeRunTransport(m_video->position()));
        return std::unexpected(std::move(started.error()));
    }
    if (auto started = m_player->start(); !started) {
        m_engine->stop();
        m_video->setTransport(playback::makeFreeRunTransport(m_video->position()));
        return std::unexpected(std::move(started.error()));
    }
    m_audioStarted = true;
    const Ticks here = m_video->position();
    m_video->setTransport(makeTimelineAudioTransport(*m_player, *m_engine));
    m_video->seek(here);
    return {};
}

void PlaybackSession::stop() {
    if (!m_audioStarted) {
        return;
    }
    const Ticks here = m_video->position();
    m_video->pause();
    m_video->setTransport(playback::makeFreeRunTransport(here));
    m_player->stop();
    m_engine->stop();
    m_audioStarted = false;
}

void PlaybackSession::setSequence(SequenceId sequence) {
    m_sequence = sequence;
    m_player->setSequence(sequence);
    m_video->setSequence(sequence);
}

void PlaybackSession::play() {
    m_video->play();
}
void PlaybackSession::pause() {
    m_video->pause();
}
void PlaybackSession::togglePlay() {
    m_video->togglePlay();
}
bool PlaybackSession::setSpeed(playback::TransportSpeed speed) {
    return m_video->setTransportSpeed(speed);
}
playback::TransportSpeed PlaybackSession::speed() const {
    return m_video->transportSpeed();
}
bool PlaybackSession::isPlaying() const {
    return m_video->isPlaying();
}
void PlaybackSession::stepFrames(int frames) {
    m_video->stepFrames(frames);
}
void PlaybackSession::seek(Ticks position) {
    m_video->seek(position);
}
void PlaybackSession::scrub(Ticks position) {
    m_video->scrub(position);
}
Ticks PlaybackSession::position() const {
    return m_video->position();
}

PlaybackSessionStats PlaybackSession::stats() const {
    PlaybackSessionStats s;
    s.video = m_video->stats();
    s.hasSound = m_audioStarted;
    if (m_audioStarted) {
        s.audio = m_player->stats();
        s.engine = m_engine->stats();
        s.audioBackend = m_engine->backendName();
    }
    return s;
}

} // namespace wabcut::editor
