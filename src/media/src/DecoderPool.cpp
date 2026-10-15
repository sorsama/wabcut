#include "FfmpegUtil.h"

#include <wabcut/media/DecoderPool.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace wabcut::media {

using namespace detail;

namespace {

Error cancelledError() {
    return Error{ErrorCode::Cancelled, "Cancelled"};
}

struct Session {
    bool audio = false;
    std::string path;
    int stream = -1;
    Rate rate{};          // audio only
    ChannelLayout layout; // audio only
    std::unique_ptr<VideoDecoder> video;
    std::unique_ptr<AudioDecoder> audioDecoder;
    Ticks lastStart{}; ///< Start of the last frame/block delivered.
    Ticks cursor{};    ///< End of the last frame/block delivered.
    bool positioned = false;
    bool busy = false;
    std::uint64_t lastUsed = 0;
    std::uint64_t owner = 0; ///< FrameRequest::owner of the last video request it served
    int softwareThreads = 0;
    VideoDecoderStats counted{}; ///< The decoder's stats already added to DecoderPoolStats

    bool isOpen() const { return video || audioDecoder; }
};

struct Pending {
    std::uint64_t sequence = 0;
    DecodePriority priority = DecodePriority::Preview;
    std::uint32_t client = 0;
    std::uint64_t epoch = 0;
    bool audio = false;
    FrameRequest frame;
    AudioRequest audioRequest;
    std::shared_ptr<JobState<FrameHandle>> frameState;
    std::shared_ptr<JobState<AudioBlock>> audioState;

    bool cancelRequested() const {
        return audio ? audioState->stop.stop_requested() : frameState->stop.stop_requested();
    }
    void cancel() const {
        if (audio) {
            audioState->complete(std::unexpected(cancelledError()));
        } else {
            frameState->complete(std::unexpected(cancelledError()));
        }
    }
};

} // namespace

struct DecoderPool::State {
    DecoderPoolConfig config;
    int threadShare = 1; ///< Software decode threads per video session.

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::uint64_t changes = 0; ///< Bumped on every event that may make queued work runnable.
    std::vector<std::unique_ptr<Session>> sessions;
    std::deque<Pending> queue;
    std::map<std::uint32_t, std::uint64_t> epochs;
    bool paused = false;
    bool stopping = false;
    std::uint64_t nextSequence = 0;
    std::uint64_t useCounter = 0;
    DecoderPoolStats stats;
    std::vector<std::thread> workers;

    // -- Locked helpers --

    std::uint64_t epochOf(std::uint32_t client) const {
        const auto it = epochs.find(client);
        return it == epochs.end() ? 0 : it->second;
    }

    bool stale(const Pending& p) const { return p.epoch < epochOf(p.client) || p.cancelRequested(); }

    void signal() {
        ++changes;
        wake.notify_all();
    }

    void refreshCounts() {
        stats.videoSessions = 0;
        stats.audioSessions = 0;
        stats.softwareThreadsInUse = 0;
        for (const auto& s : sessions) {
            (s->audio ? stats.audioSessions : stats.videoSessions)++;
            stats.softwareThreadsInUse += s->softwareThreads;
        }
        stats.peakVideoSessions = std::max(stats.peakVideoSessions, stats.videoSessions);
        stats.peakAudioSessions = std::max(stats.peakAudioSessions, stats.audioSessions);
        stats.peakSoftwareThreads = std::max(stats.peakSoftwareThreads, stats.softwareThreadsInUse);
        stats.queued = queue.size();
    }

    bool matches(const Session& s, const Pending& p) const {
        if (s.audio != p.audio) {
            return false;
        }
        if (p.audio) {
            const AudioRequest& r = p.audioRequest;
            return s.path == r.path && s.stream == r.stream && s.rate == r.outputRate && s.layout == r.layout;
        }
        return s.path == p.frame.path && s.stream == p.frame.stream;
    }

    /// Picks (or creates) the session for `p`; nullptr if it has to wait. `closed` receives a session evicted
    /// to make room, to be destroyed outside the lock.
    Session* assign(const Pending& p, std::unique_ptr<Session>& closed) {
        const Ticks t = p.audio ? p.audioRequest.time : p.frame.time;
        // (1) Sequential reuse.
        Session* best = nullptr;
        for (const auto& s : sessions) {
            if (s->busy || !s->positioned || !matches(*s, p)) {
                continue;
            }
            if (!p.audio && p.frame.owner != 0 && s->owner != 0 && s->owner != p.frame.owner) {
                continue; // another layer's session stays its own (they would take turns moving its cursor)
            }
            const Ticks behind =
                p.audio ? Ticks{config.reuseWindow.value / 8} : Ticks{}; // audio keeps history
            const bool after = t >= s->lastStart - behind;
            if (after && t - s->cursor <= config.reuseWindow && (!best || s->cursor > best->cursor)) {
                best = s.get();
            }
        }
        if (best) {
            ++stats.sequentialReuses;
            return best;
        }
        // (1b) The requester's own idle session of the same source: a jump of the same layer seeks it rather than
        //      opening the file again (stream info, hardware probe, first decode). Other owners' sessions stay theirs.
        if (!p.audio && p.frame.owner != 0) {
            for (const auto& s : sessions) {
                if (!s->busy && s->owner == p.frame.owner && matches(*s, p)) {
                    ++stats.seekReuses;
                    return s.get();
                }
            }
        }
        const std::size_t open = static_cast<std::size_t>(std::count_if(
            sessions.begin(), sessions.end(), [&](const auto& s) { return s->audio == p.audio; }));
        const auto cap =
            static_cast<std::size_t>(p.audio ? config.maxAudioSessions : config.maxVideoSessions);
        // (2) A new session under the cap.
        if (open < cap) {
            return createSession(p);
        }
        // (3) An idle session of the same source (it will seek).
        Session* sameSource = nullptr;
        Session* lru = nullptr;
        for (const auto& s : sessions) {
            if (s->busy || s->audio != p.audio) {
                continue;
            }
            if (matches(*s, p) && (!sameSource || s->lastUsed > sameSource->lastUsed)) {
                sameSource = s.get();
            }
            if (!lru || s->lastUsed < lru->lastUsed) {
                lru = s.get();
            }
        }
        if (sameSource) {
            return sameSource;
        }
        // (4) Replace the least recently used idle session.
        if (lru) {
            const auto it =
                std::find_if(sessions.begin(), sessions.end(), [&](const auto& s) { return s.get() == lru; });
            closed = std::move(*it);
            sessions.erase(it);
            ++stats.sessionsClosed;
            return createSession(p);
        }
        return nullptr;
    }

    Session* createSession(const Pending& p) {
        auto s = std::make_unique<Session>();
        s->audio = p.audio;
        if (p.audio) {
            s->path = p.audioRequest.path;
            s->stream = p.audioRequest.stream;
            s->rate = p.audioRequest.outputRate;
            s->layout = p.audioRequest.layout;
        } else {
            s->path = p.frame.path;
            s->stream = p.frame.stream;
        }
        sessions.push_back(std::move(s));
        return sessions.back().get();
    }

    /// Adds what a video session's decoder did since the last count (`now`: its current stats) to the totals.
    void countVideoWork(Session& session, const VideoDecoderStats& now) {
        stats.videoFramesDecoded += now.framesDecoded - session.counted.framesDecoded;
        stats.videoFramesDelivered += now.framesDelivered - session.counted.framesDelivered;
        stats.videoFramesReadBack += now.framesReadBack - session.counted.framesReadBack;
        stats.videoFramesConverted += now.framesConverted - session.counted.framesConverted;
        session.counted = now;
    }

    void removeSession(Session* session) {
        const auto it =
            std::find_if(sessions.begin(), sessions.end(), [&](const auto& s) { return s.get() == session; });
        if (it != sessions.end()) {
            sessions.erase(it);
        }
    }

    // -- Worker --

    void run() {
        std::unique_lock lock(mutex);
        for (;;) {
            if (stopping) {
                return;
            }
            const std::uint64_t seen = changes;
            std::optional<Pending> job;
            Session* session = nullptr;
            std::unique_ptr<Session> closed;
            std::vector<Pending> dropped;
            if (!paused) {
                // Highest priority first, then submission order.
                std::vector<std::size_t> order(queue.size());
                for (std::size_t i = 0; i < order.size(); ++i) {
                    order[i] = i;
                }
                std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                    if (queue[a].priority != queue[b].priority) {
                        return queue[a].priority > queue[b].priority;
                    }
                    return queue[a].sequence < queue[b].sequence;
                });
                std::vector<std::size_t> remove;
                for (const std::size_t i : order) {
                    if (stale(queue[i])) {
                        dropped.push_back(std::move(queue[i]));
                        remove.push_back(i);
                        continue;
                    }
                    session = assign(queue[i], closed);
                    if (session) {
                        job = std::move(queue[i]);
                        remove.push_back(i);
                        break;
                    }
                }
                std::sort(remove.rbegin(), remove.rend());
                for (const std::size_t i : remove) {
                    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
                }
                stats.cancelled += dropped.size();
            }
            if (!job) {
                refreshCounts();
                lock.unlock();
                for (const Pending& p : dropped) {
                    p.cancel();
                }
                closed.reset();
                lock.lock();
                if (dropped.empty()) {
                    wake.wait(lock, [&] { return stopping || changes != seen; });
                }
                continue;
            }

            session->busy = true;
            session->lastUsed = ++useCounter;
            if (!job->audio) {
                session->owner = job->frame.owner;
            }
            refreshCounts();
            lock.unlock();
            for (const Pending& p : dropped) {
                p.cancel();
            }
            closed.reset(); // close the evicted decoder outside the lock
            execute(*job, *session);
            lock.lock();
        }
    }

    /// Runs one request on its (busy) session, outside the lock.
    void execute(const Pending& p, Session& session) {
        std::expected<void, Error> opened;
        if (!session.isOpen()) {
            opened = openSession(p, session);
        }
        if (!opened) {
            std::unique_lock lock(mutex);
            removeSession(&session);
            ++stats.failed;
            refreshCounts();
            signal();
            lock.unlock();
            if (p.audio) {
                p.audioState->complete(std::unexpected(opened.error()));
            } else {
                p.frameState->complete(std::unexpected(opened.error()));
            }
            return;
        }
        if (p.audio) {
            const AudioRequest& r = p.audioRequest;
            AudioBlock block;
            block.interleaved.resize(r.frames * static_cast<std::size_t>(r.layout.channels));
            auto got = session.audioDecoder->read(r.time, block.interleaved);
            std::unique_lock lock(mutex);
            session.busy = false;
            if (got) {
                block.result = *got;
                session.lastStart = r.time;
                session.cursor = r.time + samplesToTicks(static_cast<std::int64_t>(r.frames), r.outputRate);
                session.positioned = true;
            }
            const bool isStale = stale(p);
            (got && !isStale ? stats.completed : (isStale ? stats.cancelled : stats.failed))++;
            signal();
            lock.unlock();
            if (isStale) {
                p.audioState->complete(std::unexpected(cancelledError()));
            } else if (got) {
                p.audioState->complete(std::move(block));
            } else {
                p.audioState->complete(std::unexpected(got.error()));
            }
            return;
        }
        auto frame = session.video->frameAt(p.frame.time);
        const VideoDecoderStats work = session.video->stats(); // the session is busy: this worker owns its decoder
        std::unique_lock lock(mutex);
        session.busy = false;
        countVideoWork(session, work);
        if (frame) {
            session.lastStart = frame->pts;
            session.cursor = frame->pts + frame->duration;
            session.positioned = true;
        }
        const bool isStale = stale(p);
        (frame && !isStale ? stats.completed : (isStale ? stats.cancelled : stats.failed))++;
        signal();
        lock.unlock();
        if (isStale) {
            p.frameState->complete(std::unexpected(cancelledError()));
        } else {
            p.frameState->complete(std::move(frame));
        }
    }

    std::expected<void, Error> openSession(const Pending& p, Session& session) {
        if (p.audio) {
            const AudioRequest& r = p.audioRequest;
            auto decoder = AudioDecoder::open(r.path, r.stream, r.outputRate, r.layout);
            if (!decoder) {
                return std::unexpected(decoder.error());
            }
            std::lock_guard lock(mutex);
            session.audioDecoder = std::move(*decoder);
            ++stats.sessionsCreated;
            return {};
        }
        DecoderOptions options = config.decoderOptions;
        options.threadCount = threadShare;
        auto decoder = VideoDecoder::open(p.frame.path, p.frame.stream, options);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        std::lock_guard lock(mutex);
        session.softwareThreads = (*decoder)->isHardwareAccelerated() ? 0 : threadShare;
        session.video = std::move(*decoder);
        ++stats.sessionsCreated;
        return {};
    }
};

DecoderPool::DecoderPool(DecoderPoolConfig config) : m_state(std::make_unique<State>()) {
    ensureFfmpegLogging();
    State& s = *m_state;
    config.maxVideoSessions = std::max(1, config.maxVideoSessions);
    config.maxAudioSessions = std::max(1, config.maxAudioSessions);
    config.workerThreads = std::max(1, config.workerThreads);
    if (config.softwareThreadBudget <= 0) {
        config.softwareThreadBudget = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
    }
    s.config = config;
    s.threadShare = std::max(1, config.softwareThreadBudget / config.maxVideoSessions);
    s.stats.softwareThreadBudget = config.softwareThreadBudget;
    for (int i = 0; i < config.workerThreads; ++i) {
        s.workers.emplace_back([state = m_state.get()] { state->run(); });
    }
}

DecoderPool::~DecoderPool() {
    State& s = *m_state;
    std::deque<Pending> abandoned;
    {
        std::lock_guard lock(s.mutex);
        s.stopping = true;
        abandoned.swap(s.queue);
        s.stats.cancelled += abandoned.size();
        s.signal();
    }
    for (const Pending& p : abandoned) {
        p.cancel();
    }
    for (std::thread& t : s.workers) {
        t.join(); // running requests finish first
    }
    s.sessions.clear();
}

Job<FrameHandle> DecoderPool::requestFrame(FrameRequest request, FrameCallback onDone) {
    auto state = std::make_shared<JobState<FrameHandle>>();
    state->onDone = std::move(onDone);
    Pending p;
    p.priority = request.priority;
    p.client = request.client;
    p.epoch = request.epoch;
    p.frame = std::move(request);
    p.frameState = state;
    {
        std::lock_guard lock(m_state->mutex);
        ++m_state->stats.requests;
        if (p.epoch < m_state->epochOf(p.client)) {
            ++m_state->stats.cancelled;
        } else {
            p.sequence = m_state->nextSequence++;
            m_state->queue.push_back(std::move(p));
            m_state->stats.queued = m_state->queue.size();
            m_state->signal();
            return Job<FrameHandle>(state);
        }
    }
    state->complete(std::unexpected(cancelledError())); // already stale when submitted
    return Job<FrameHandle>(state);
}

Job<AudioBlock> DecoderPool::requestAudio(AudioRequest request, AudioCallback onDone) {
    auto state = std::make_shared<JobState<AudioBlock>>();
    state->onDone = std::move(onDone);
    if (!request.layout.isValid() || request.frames == 0) {
        state->complete(
            std::unexpected(Error{ErrorCode::InvalidArgument, "Empty audio request or bad layout"}));
        return Job<AudioBlock>(state);
    }
    Pending p;
    p.audio = true;
    p.priority = request.priority;
    p.client = request.client;
    p.epoch = request.epoch;
    p.audioRequest = std::move(request);
    p.audioState = state;
    {
        std::lock_guard lock(m_state->mutex);
        ++m_state->stats.requests;
        if (p.epoch < m_state->epochOf(p.client)) {
            ++m_state->stats.cancelled;
        } else {
            p.sequence = m_state->nextSequence++;
            m_state->queue.push_back(std::move(p));
            m_state->stats.queued = m_state->queue.size();
            m_state->signal();
            return Job<AudioBlock>(state);
        }
    }
    state->complete(std::unexpected(cancelledError()));
    return Job<AudioBlock>(state);
}

void DecoderPool::setEpoch(std::uint32_t client, std::uint64_t epoch) {
    std::vector<Pending> stale;
    {
        std::lock_guard lock(m_state->mutex);
        m_state->epochs[client] = epoch;
        auto& queue = m_state->queue;
        for (auto it = queue.begin(); it != queue.end();) {
            if (it->client == client && it->epoch < epoch) {
                stale.push_back(std::move(*it));
                it = queue.erase(it);
            } else {
                ++it;
            }
        }
        m_state->stats.cancelled += stale.size();
        m_state->stats.queued = queue.size();
        m_state->signal();
    }
    for (const Pending& p : stale) {
        p.cancel();
    }
}

std::uint64_t DecoderPool::epoch(std::uint32_t client) const {
    std::lock_guard lock(m_state->mutex);
    return m_state->epochOf(client);
}

void DecoderPool::pause() {
    std::lock_guard lock(m_state->mutex);
    m_state->paused = true;
}

void DecoderPool::resume() {
    std::lock_guard lock(m_state->mutex);
    m_state->paused = false;
    m_state->signal();
}

void DecoderPool::closeIdleSessions() {
    std::vector<std::unique_ptr<Session>> closed;
    {
        std::lock_guard lock(m_state->mutex);
        auto& sessions = m_state->sessions;
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!(*it)->busy) {
                closed.push_back(std::move(*it));
                it = sessions.erase(it);
                ++m_state->stats.sessionsClosed;
            } else {
                ++it;
            }
        }
        m_state->refreshCounts();
        m_state->signal();
    }
    closed.clear(); // decoders are destroyed outside the lock
}

DecoderPoolStats DecoderPool::stats() const {
    std::lock_guard lock(m_state->mutex);
    DecoderPoolStats s = m_state->stats;
    s.queued = m_state->queue.size();
    return s;
}

const DecoderPoolConfig& DecoderPool::config() const {
    return m_state->config;
}

} // namespace wabcut::media
