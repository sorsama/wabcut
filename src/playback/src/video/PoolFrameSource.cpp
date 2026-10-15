#include "PoolFrameSource.h"
#include "Downscale.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>

namespace wabcut::playback::detail {

namespace {

constexpr std::size_t kMaxWanted = 64;
constexpr std::size_t kMaxSamples = 1 << 16;
/// A reverse window whose frames lie further apart than this (fast reverse shuttle) would decode seconds of video
/// for a few frames: the target is asked for alone instead.
constexpr Ticks kMaxWindowStep{kTicksPerSecond / 2};

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

struct PoolFrameSource::State : std::enable_shared_from_this<State> {
    struct Entry {
        media::FrameHandle frame;
        Ticks requested{};
        bool failed = false;
    };
    struct Want {
        Ticks time;
        bool urgent = false;
    };

    std::weak_ptr<media::DecoderPool> pool;
    std::shared_ptr<PoolCallbackGate> gate;
    std::string path;
    int stream = -1;
    PoolFrameSourceOptions options;
    /// FrameRequest::owner: this source's own decoder session is the one its jumps seek (P.1).
    std::uint64_t requester = 0;

    std::mutex readyMutex; // guards onReady against the source's destruction
    std::function<void()> onReady;

    mutable std::mutex mutex;
    std::vector<Entry> cache; // ring
    std::size_t cacheNext = 0;
    std::vector<Want> wanted; // presentation order, capacity reserved
    std::optional<Ticks> inFlight;
    std::uint64_t ticket = 0;
    media::Job<media::FrameHandle> job;
    std::int64_t submittedNs = 0;
    std::uint64_t epoch = 0;
    bool stopping = false;
    std::string backend;
    std::vector<std::int64_t> samples;
    std::uint64_t decoded = 0;
    std::uint64_t errors = 0;

    bool cachedLocked(Ticks t) const {
        for (const Entry& e : cache) {
            if (e.failed ? e.requested == t : (e.frame.isValid() && e.frame.contains(t))) {
                return true;
            }
        }
        return false;
    }

    bool wantedLocked(Ticks t) const {
        return (inFlight && *inFlight == t) ||
               std::any_of(wanted.begin(), wanted.end(), [&](const Want& w) { return w.time == t; });
    }

    void storeLocked(Entry entry) {
        cache[cacheNext] = std::move(entry); // the evicted frame is released here (scheduler or pool thread)
        cacheNext = (cacheNext + 1) % cache.size();
    }

    void addWantLocked(Ticks t, bool urgent) {
        if (cachedLocked(t) || wantedLocked(t)) {
            return;
        }
        if (wanted.size() >= kMaxWanted) {
            wanted.erase(wanted.begin());
        }
        wanted.push_back({t, urgent});
    }

    /// Submits the next wanted time when nothing is in flight. Never called with `mutex` held: the pool may
    /// complete a request synchronously inside requestFrame().
    void pump() {
        media::FrameRequest request;
        std::uint64_t myTicket = 0;
        {
            std::lock_guard lock(mutex);
            if (stopping || inFlight) {
                return;
            }
            while (!wanted.empty() && cachedLocked(wanted.front().time)) {
                wanted.erase(wanted.begin());
            }
            if (wanted.empty()) {
                return;
            }
            const Want next = wanted.front();
            wanted.erase(wanted.begin());
            inFlight = next.time;
            myTicket = ++ticket;
            submittedNs = nowNs();
            request.path = path;
            request.stream = stream;
            request.time = next.time;
            request.priority = next.urgent ? media::DecodePriority::Preview : media::DecodePriority::Prefetch;
            request.client = options.client;
            request.epoch = epoch;
            request.owner = requester;
        }
        const std::shared_ptr<media::DecoderPool> owner = pool.lock();
        if (!owner) {
            std::lock_guard lock(mutex);
            inFlight.reset();
            return;
        }
        gate->enter();
        auto self = shared_from_this();
        const Ticks time = request.time;
        media::Job<media::FrameHandle> submitted = owner->requestFrame(
            std::move(request), [self, myTicket, time](const std::expected<media::FrameHandle, Error>& result) {
                self->onDone(myTicket, time, result);
            });
        std::lock_guard lock(mutex);
        if (ticket == myTicket && inFlight) {
            job = std::move(submitted); // kept for cancel() on destruction
        }
    }

    void onDone(std::uint64_t doneTicket, Ticks time, const std::expected<media::FrameHandle, Error>& delivered) {
        // Reduced-resolution preview: downscale outside the lock.
        std::expected<media::FrameHandle, Error> result = delivered;
        if (result) {
            if (const int d = extraDivisor(options.streamHeight, result->height, options.targetDivisor); d > 1) {
                result = downscaleFrame(*result, d);
            }
        }
        {
            std::lock_guard lock(mutex);
            if (doneTicket == ticket) {
                inFlight.reset();
                job = {};
            }
            if (!stopping) {
                if (options.collectTimings && samples.size() < kMaxSamples) {
                    samples.push_back(nowNs() - submittedNs);
                }
                if (result) {
                    ++decoded;
                    const media::FrameHandle& f = *result;
                    const bool native = f.payloadKind() != media::PayloadKind::CpuPlanes;
                    const char* kind = native ? "pool/zero-copy" : f.device().isCpu() ? "pool/software" : "pool/hardware";
                    if (backend != kind) {
                        backend = kind;
                    }
                    bool duplicate = false;
                    for (const Entry& e : cache) {
                        if (e.frame.isValid() && e.frame.pts == f.pts) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        storeLocked({f, time, false});
                    }
                } else if (result.error().code != ErrorCode::Cancelled) {
                    ++errors;
                    storeLocked({{}, time, true});
                }
            }
        }
        pump();
        {
            std::lock_guard ready(readyMutex);
            if (onReady) {
                onReady();
            }
        }
        gate->exit();
    }
};

// ---- PoolCallbackGate ---------------------------------------------------------------------------------------

void PoolCallbackGate::enter() {
    std::lock_guard lock(m_mutex);
    ++m_active;
}

void PoolCallbackGate::exit() {
    {
        std::lock_guard lock(m_mutex);
        --m_active;
    }
    m_idle.notify_all();
}

bool PoolCallbackGate::waitIdle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(m_mutex);
    return m_idle.wait_for(lock, timeout, [&] { return m_active == 0; });
}

// ---- PoolFrameSource ----------------------------------------------------------------------------------------

PoolFrameSource::PoolFrameSource(std::shared_ptr<media::DecoderPool> pool, std::shared_ptr<PoolCallbackGate> gate,
                                 std::string path, int containerStream, PoolFrameSourceOptions options,
                                 std::function<void()> onReady)
    : m_path(path), m_stream(containerStream), m_state(std::make_shared<State>()) {
    m_state->pool = pool;
    m_state->gate = std::move(gate);
    m_state->path = std::move(path);
    m_state->stream = containerStream;
    m_state->options = options;
    static std::atomic<std::uint64_t> nextOwner{1};
    m_state->requester = nextOwner.fetch_add(1, std::memory_order_relaxed);
    m_state->onReady = std::move(onReady);
    m_state->cache.resize(static_cast<std::size_t>(std::max(options.cacheFrames, 4)));
    m_state->wanted.reserve(kMaxWanted);
    if (options.collectTimings) {
        m_state->samples.reserve(4096);
    }
}

PoolFrameSource::~PoolFrameSource() {
    media::Job<media::FrameHandle> running;
    {
        std::lock_guard lock(m_state->mutex);
        m_state->stopping = true;
        m_state->wanted.clear();
        running = m_state->job;
    }
    running.cancel(); // a queued request is dropped; a decoding one finishes and is discarded
    std::lock_guard ready(m_state->readyMutex);
    m_state->onReady = nullptr; // no callback reaches the scheduler after this
}

bool PoolFrameSource::request(Ticks sourceTime, std::uint64_t epoch, bool reverse, bool urgent) {
    bool added = false;
    {
        std::lock_guard lock(m_state->mutex);
        if (epoch < m_state->epoch) {
            return false;
        }
        if (epoch > m_state->epoch) {
            m_state->epoch = epoch;
            m_state->wanted.clear();
        }
        if (m_state->cachedLocked(sourceTime) || m_state->wantedLocked(sourceTime)) {
            return false;
        }
        const std::size_t before = m_state->wanted.size();
        if (reverse) {
            // A bounded forward window ending at the target, decoded in order by one session, served backwards. Its
            // times step by what the scheduler shows (targetStep), never by less than a frame period: a 240 fps
            // source in a 60 fps sequence decodes every frame of the window but delivers only every fourth.
            const Ticks framePeriod = m_state->options.frameDuration.value > 0 ? m_state->options.frameDuration
                                                                                 : Ticks{kTicksPerSecond / 30};
            const Ticks period = std::max(framePeriod, targetStep);
            const int frames = period > kMaxWindowStep ? 1 : std::max(m_state->options.reverseWindowFrames, 1);
            for (int i = frames - 1; i >= 0; --i) {
                const Ticks t = sourceTime - Ticks{period.value * i};
                if (t.value >= 0) {
                    m_state->addWantLocked(t, urgent && i == 0);
                }
            }
        } else {
            m_state->addWantLocked(sourceTime, urgent);
        }
        added = m_state->wanted.size() != before;
    }
    m_state->pump();
    return added;
}

void PoolFrameSource::beginEpoch(std::uint64_t epoch) {
    std::lock_guard lock(m_state->mutex);
    if (epoch > m_state->epoch) {
        m_state->epoch = epoch;
        m_state->wanted.clear();
    }
}

void PoolFrameSource::dropPassed(Ticks frontier, bool reverse) {
    std::lock_guard lock(m_state->mutex);
    std::erase_if(m_state->wanted,
                  [&](const State::Want& w) { return reverse ? w.time > frontier : w.time < frontier; });
}

FrameSource::Lookup PoolFrameSource::lookup(Ticks sourceTime, media::FrameHandle& out) const {
    std::lock_guard lock(m_state->mutex);
    for (const State::Entry& e : m_state->cache) {
        if (!e.failed && e.frame.isValid() && e.frame.contains(sourceTime)) {
            out = e.frame;
            return Lookup::Ready;
        }
    }
    for (const State::Entry& e : m_state->cache) {
        if (e.failed && e.requested == sourceTime) {
            return Lookup::Failed;
        }
    }
    return Lookup::Pending;
}

std::string PoolFrameSource::backend() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->backend;
}

std::uint64_t PoolFrameSource::decodedFrames() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->decoded;
}

std::uint64_t PoolFrameSource::errors() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->errors;
}

void PoolFrameSource::takeSamples(std::vector<std::int64_t>& out) {
    std::lock_guard lock(m_state->mutex);
    out.insert(out.end(), m_state->samples.begin(), m_state->samples.end());
    m_state->samples.clear();
}

} // namespace wabcut::playback::detail
