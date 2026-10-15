#include "DecodeSession.h"
#include "Downscale.h"

#include <algorithm>
#include <chrono>

namespace wabcut::playback::detail {

namespace {

constexpr std::size_t kMaxQueued = 64;
constexpr std::size_t kMaxSamples = 1 << 16;

} // namespace

DecodeSession::DecodeSession(std::string path, int containerStream, DecodeSessionOptions options,
                             std::function<void()> onReady)
    : m_path(std::move(path)), m_stream(containerStream), m_options(options), m_onReady(std::move(onReady)) {
    m_cache.resize(static_cast<std::size_t>(std::max(m_options.cacheFrames, 4)));
    m_queue.reserve(kMaxQueued);
    if (m_options.collectTimings) {
        m_samples.reserve(4096);
    }
    m_thread = std::thread(&DecodeSession::run, this);
}

DecodeSession::~DecodeSession() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool DecodeSession::cachedLocked(Ticks t) const {
    for (const Entry& e : m_cache) {
        if (e.failed ? e.requested == t : (e.frame.isValid() && e.frame.contains(t))) {
            return true;
        }
    }
    return false;
}

void DecodeSession::storeLocked(Entry entry) {
    m_cache[m_cacheNext] =
        std::move(entry); // the evicted frame is released here (worker or scheduler thread)
    m_cacheNext = (m_cacheNext + 1) % m_cache.size();
}

void DecodeSession::beginEpoch(std::uint64_t epoch) {
    std::lock_guard lock(m_mutex);
    if (epoch > m_minEpoch) {
        m_minEpoch = epoch;
        m_queue.clear();
    }
}

bool DecodeSession::request(Ticks sourceTime, std::uint64_t epoch, bool reverse, bool /*urgent*/) {
    {
        std::lock_guard lock(m_mutex);
        if (epoch < m_minEpoch) {
            return false;
        }
        if (epoch > m_minEpoch) {
            m_minEpoch = epoch;
            m_queue.clear();
        }
        if (cachedLocked(sourceTime) || m_inFlight == sourceTime) {
            return false;
        }
        for (const Request& q : m_queue) {
            if (q.time == sourceTime) {
                return false;
            }
        }
        if (m_queue.size() >= kMaxQueued) {
            m_queue.erase(m_queue.begin());
        }
        m_queue.push_back({sourceTime, epoch, reverse});
    }
    m_wake.notify_one();
    return true;
}

void DecodeSession::dropPassed(Ticks frontier, bool reverse) {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_queue, [&](const Request& q) { return reverse ? q.time > frontier : q.time < frontier; });
}

DecodeSession::Lookup DecodeSession::lookup(Ticks sourceTime, media::VideoFrame& out) const {
    std::lock_guard lock(m_mutex);
    for (const Entry& e : m_cache) {
        if (!e.failed && e.frame.isValid() && e.frame.contains(sourceTime)) {
            out = e.frame;
            return Lookup::Ready;
        }
    }
    for (const Entry& e : m_cache) {
        if (e.failed && e.requested == sourceTime) {
            return Lookup::Failed;
        }
    }
    return m_openFailed.load() ? Lookup::Failed : Lookup::Pending;
}

std::string DecodeSession::backend() const {
    std::lock_guard lock(m_mutex);
    return m_backend;
}

void DecodeSession::takeSamples(std::vector<std::int64_t>& out) {
    std::lock_guard lock(m_mutex);
    out.insert(out.end(), m_samples.begin(), m_samples.end());
    m_samples.clear();
}

std::optional<media::VideoFrame> DecodeSession::timedFrameAt(Ticks t, std::optional<Error>& error) {
    const auto start = std::chrono::steady_clock::now();
    auto frame = m_decoder->frameAt(t);
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
    std::lock_guard lock(m_mutex);
    if (m_options.collectTimings && m_samples.size() < kMaxSamples) {
        m_samples.push_back(ns.count());
    }
    const std::string_view backend = m_decoder->backendName();
    if (backend != m_backend) {
        m_backend = std::string(backend); // changes only on a hardware fallback
    }
    if (!frame) {
        error = frame.error();
        m_errors.fetch_add(1);
        return std::nullopt;
    }
    m_decoded.fetch_add(1);
    if (const int d = extraDivisor(m_options.streamHeight, frame->height, m_options.targetDivisor); d > 1) {
        return downscaleFrame(*frame, d);
    }
    return std::move(*frame);
}

void DecodeSession::decodeWindow(Ticks first, Ticks last) {
    // Bounded: at most reverseWindowFrames + 2 frames per window.
    Ticks t = first;
    const int budget = std::max(m_options.reverseWindowFrames, 1) + 2;
    for (int i = 0; i < budget && t <= last; ++i) {
        {
            std::lock_guard lock(m_mutex);
            if (m_stop) {
                return;
            }
            bool skipped = false;
            for (const Entry& e : m_cache) {
                if (!e.failed && e.frame.isValid() && e.frame.contains(t)) {
                    t = e.frame.pts + std::max(e.frame.duration, Ticks{1});
                    skipped = true;
                    break;
                }
            }
            if (skipped) {
                continue;
            }
        }
        std::optional<Error> error;
        auto frame = timedFrameAt(t, error);
        std::lock_guard lock(m_mutex);
        if (!frame) {
            storeLocked({{}, last, true, false});
            return;
        }
        const Ticks next = frame->pts + (frame->duration.value > 0 ? frame->duration : m_frameDuration);
        const bool done = frame->contains(last) || frame->pts > last;
        storeLocked({std::move(*frame), t, false, false});
        if (done) {
            return;
        }
        t = std::max(next, t + Ticks{1});
    }
}

void DecodeSession::decodeOne(const Request& r) {
    if (r.reverse) {
        const Ticks window = m_window;
        const Ticks first = std::max(m_firstFrame, r.time - window);
        decodeWindow(first, r.time);
        return;
    }
    std::optional<Error> error;
    auto frame = timedFrameAt(r.time, error);
    std::lock_guard lock(m_mutex);
    if (frame) {
        storeLocked({std::move(*frame), r.time, false, false});
    } else {
        storeLocked({{}, r.time, true, false});
    }
}

void DecodeSession::run() {
    std::optional<Ticks> prefetchBefore; // reverse: decode the window preceding this time when idle
    for (;;) {
        Request r;
        bool prefetch = false;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stop || !m_queue.empty() || prefetchBefore.has_value(); });
            if (m_stop) {
                break;
            }
            m_inFlight.reset();
            if (!m_queue.empty()) {
                r = m_queue.front();
                m_queue.erase(m_queue.begin());
                if (r.epoch < m_minEpoch || cachedLocked(r.time)) {
                    continue;
                }
                m_inFlight = r.time;
                m_lastReverse = r.reverse;
                if (!r.reverse) {
                    prefetchBefore.reset();
                }
            } else {
                prefetch = true;
            }
        }

        if (!m_decoder && !m_openFailed.load()) {
            media::DecoderOptions options = m_options.decoder;
            options.snapToStreamTicks = true; // sequence frames: a rounded timestamp still shows its frame (I.9)
            auto decoder = media::VideoDecoder::open(m_path, m_stream, options);
            if (!decoder) {
                m_openFailed.store(true);
                m_errors.fetch_add(1);
            } else {
                m_decoder = std::move(*decoder);
                const Rate rate = m_decoder->streamInfo().frameRate;
                const int windowFrames = std::max(m_options.reverseWindowFrames - 1, 0);
                // Checked conversions (Principle 1); a stream without a usable rate keeps the 30 fps
                // defaults.
                m_frameDuration = framesToTicks(1, rate, Rounding::Floor).value_or(m_frameDuration);
                m_window = framesToTicks(windowFrames, rate, Rounding::Floor)
                               .value_or(Ticks{m_frameDuration.value * windowFrames});
                m_firstFrame = m_decoder->firstFrameTime();
                std::lock_guard lock(m_mutex);
                m_backend = std::string(m_decoder->backendName());
            }
        }
        if (!m_decoder) {
            prefetchBefore.reset();
            if (m_onReady) {
                m_onReady(); // lets the scheduler see the failure
            }
            continue;
        }

        if (prefetch) {
            const Ticks before = *prefetchBefore;
            prefetchBefore.reset();
            if (before > m_firstFrame) {
                const Ticks last = before - Ticks{1};
                const Ticks window = m_window;
                decodeWindow(std::max(m_firstFrame, last - window), last);
            }
            continue;
        }

        decodeOne(r);
        if (r.reverse) {
            const Ticks window = m_window;
            prefetchBefore = std::max(m_firstFrame, r.time - window);
        }
        if (m_onReady) {
            m_onReady();
        }
    }
    // Decoder resources and cached frames are released on this thread.
    m_decoder.reset();
    std::lock_guard lock(m_mutex);
    for (Entry& e : m_cache) {
        e = Entry{};
    }
}

} // namespace wabcut::playback::detail
