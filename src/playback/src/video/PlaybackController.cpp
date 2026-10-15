#include <wabcut/playback/video/PlaybackController.h>

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace wabcut::playback {

namespace {

constexpr std::int64_t kMaxShuttle = 32;

int directionOf(TransportSpeed s) {
    return s.isZero() ? 0 : s.isReverse() ? -1 : 1;
}

} // namespace

/// Lets the mailbox (render thread) map refresh times to the timeline through the transport, lock-free.
class PlaybackController::PresentationAdapter final : public render::PresentationClock {
public:
    explicit PresentationAdapter(std::shared_ptr<TransportClock> transport)
        : m_transport(std::move(transport)) {}
    Ticks presentationTime(std::int64_t hostNs) const noexcept override {
        return m_transport->transportTime(hostNs);
    }
    int direction() const noexcept override { return directionOf(m_transport->effectiveSpeed()); }

private:
    std::shared_ptr<TransportClock> m_transport;
};

PlaybackController::PlaybackController(PlaybackOptions options)
    : m_options(options), m_mailbox(std::make_shared<render::CompositeMailbox>(options.mailboxCapacity)),
      m_transport(makeFreeRunTransport()) {
    m_mailbox->setClock(std::make_shared<PresentationAdapter>(m_transport));
    m_scheduler = std::make_unique<Scheduler>(m_mailbox, m_transport, m_options.scheduler);
    m_scheduler->setBoundaryCallback([this](int direction) { onBoundary(direction); });
}

PlaybackController::~PlaybackController() {
    m_scheduler.reset(); // stops the scheduler thread before the transport goes
}

void PlaybackController::setTransport(std::shared_ptr<TransportClock> transport) {
    if (!transport) {
        transport = makeFreeRunTransport();
    }
    {
        std::lock_guard lock(m_mutex);
        m_transport = transport;
    }
    m_mailbox->setClock(std::make_shared<PresentationAdapter>(transport));
    m_scheduler->setClock(transport);
    startEpoch(render::SeekDisplay::HoldPrevious);
}

std::shared_ptr<TransportClock> PlaybackController::transport() const {
    std::lock_guard lock(m_mutex);
    return m_transport;
}

void PlaybackController::setSession(const EditSession* session) {
    m_session = session;
    m_scheduler->setSession(session, m_sequence.load());
    startEpoch(render::SeekDisplay::Black);
}

void PlaybackController::setSequence(SequenceId sequence) {
    m_sequence = sequence;
    m_scheduler->setSession(m_session.load(), sequence);
    startEpoch(render::SeekDisplay::Black);
}

void PlaybackController::startEpoch(render::SeekDisplay display) {
    m_scheduler->retarget(m_mailbox->beginSeek(display));
}

void PlaybackController::play() {
    setTransportSpeed(TransportSpeed::normal());
}

void PlaybackController::pause() {
    auto t = transport();
    if (!t->isPlaying()) {
        return;
    }
    t->pause();
    // Lookahead composites beyond the paused position must not show: new epoch, keep the current picture.
    startEpoch(render::SeekDisplay::HoldPrevious);
}

void PlaybackController::togglePlay() {
    if (isPlaying()) {
        pause();
    } else {
        play();
    }
}

bool PlaybackController::setTransportSpeed(TransportSpeed speed) {
    if (!speed.isValid()) {
        return false;
    }
    const TransportSpeed n = speed.normalized();
    if (n.isZero()) {
        pause();
        return true;
    }
    if (std::abs(static_cast<std::int64_t>(n.num)) > kMaxShuttle * n.den) {
        return false;
    }
    auto t = transport();
    const int before = directionOf(t->effectiveSpeed());
    t->setSpeed(n);
    if (!t->isPlaying()) {
        t->play();
    }
    if (before != directionOf(n)) {
        // Started, or reversed: frames queued for the other direction are wrong.
        startEpoch(render::SeekDisplay::HoldPrevious);
    } else {
        m_scheduler->wake();
    }
    return true;
}

TransportSpeed PlaybackController::transportSpeed() const {
    return transport()->effectiveSpeed();
}

bool PlaybackController::isPlaying() const {
    return transport()->isPlaying();
}

Rate PlaybackController::sequenceRate() const {
    if (const EditSession* session = m_session.load()) {
        const ProjectSnapshot snapshot = session->snapshot();
        if (const Sequence* seq = snapshot ? snapshot->findSequence(m_sequence.load()) : nullptr;
            seq && seq->frameRate.isValid() && seq->frameRate.isExact()) {
            return seq->frameRate;
        }
    }
    return kRate30;
}

void PlaybackController::stepFrames(int frames) {
    auto t = transport();
    const Rate rate = sequenceRate();
    const Ticks here = snapToFrame(t->transportTime(steadyNowNs()), rate);
    if (t->isPlaying()) {
        t->pause();
    }
    // Checked frame arithmetic (Principle 1): a huge step saturates at the sequence start or stays put.
    const std::optional<Ticks> offset = framesToTicks(frames, rate, Rounding::Floor);
    const std::optional<Ticks> target = offset ? checkedAdd(here, *offset) : std::nullopt;
    t->seek(Ticks{std::max<std::int64_t>(0, target.value_or(frames < 0 ? Ticks{} : here).value)});
    startEpoch(render::SeekDisplay::HoldPrevious);
}

void PlaybackController::seek(Ticks position) {
    transport()->seek(position);
    startEpoch(render::SeekDisplay::Black);
}

void PlaybackController::scrub(Ticks position) {
    transport()->seek(position);
    startEpoch(render::SeekDisplay::HoldPrevious);
}

Ticks PlaybackController::position() const {
    return transport()->transportTime(steadyNowNs());
}

void PlaybackController::onBoundary(int direction) {
    // Scheduler thread: playback ran off the sequence. Park on the last frame (forward) or the first
    // (reverse).
    auto t = transport();
    const Ticks here = t->transportTime(steadyNowNs());
    t->pause();
    if (direction > 0) {
        const Rate rate = sequenceRate();
        const Ticks last =
            snapToFrame(here, rate) - framesToTicks(1, rate, Rounding::Floor).value_or(Ticks{});
        t->seek(Ticks{std::max<std::int64_t>(0, last.value)});
    } else {
        t->seek(Ticks{});
    }
    startEpoch(render::SeekDisplay::HoldPrevious);
}

void PlaybackController::setRenderBackend(std::string name) {
    std::lock_guard lock(m_mutex);
    m_renderBackend = std::move(name);
}

PlaybackStats PlaybackController::stats() const {
    PlaybackStats s;
    const render::MailboxStats m = m_mailbox->stats();
    s.presented = m.presented;
    s.duplicated = m.repeated;
    s.dropped = m.dropped;
    s.late = m.late;
    s.blackFrames = m.blackFrames;
    s.frameAgeMs = static_cast<double>(m.lastFrameAgeNs) / 1e6;
    s.maxFrameAgeMs = static_cast<double>(m.maxFrameAgeNs) / 1e6;
    const SchedulerStats sch = m_scheduler->stats();
    s.published = sch.published;
    s.skippedLate = sch.skippedLate;
    s.decodeErrors = sch.decodeErrors;
    s.activeSessions = sch.activeSessions;
    s.decodeBackends = sch.decodeBackends;
    s.bypassedEffects = sch.bypassedEffects;
    s.proxyLayers = sch.proxyLayers;
    s.originalLayers = sch.originalLayers;
    s.offlineProxyLayers = sch.offlineProxyLayers;
    s.decodeDivisor = sch.decodeDivisor;
    std::lock_guard lock(m_mutex);
    s.renderBackend = m_renderBackend;
    s.transport = m_transport->name();
    return s;
}

} // namespace wabcut::playback
