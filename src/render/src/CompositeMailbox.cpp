#include <wabcut/render/CompositeMailbox.h>

#include <algorithm>
#include <chrono>

namespace wabcut::render {

namespace {

std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

CompositeMailbox::CompositeMailbox(int capacity) : m_capacity(std::max(capacity, 1)) {
    m_pending.reserve(static_cast<std::size_t>(m_capacity));
}

std::uint64_t CompositeMailbox::beginSeek(SeekDisplay display) {
    // Producers publish pooled composites, so releasing them here is a reference-count decrement.
    std::lock_guard lock(m_mutex);
    const std::uint64_t next = m_epoch.load(std::memory_order_relaxed) + 1;
    m_epoch.store(next, std::memory_order_release);
    if (display == SeekDisplay::Black) {
        m_displayFloor = next;
    }
    m_counters.stale.fetch_add(m_pending.size(), std::memory_order_relaxed);
    m_pending.clear();
    return next;
}

bool CompositeMailbox::publish(Ptr composite) {
    if (!composite) {
        return false;
    }
    Ptr evicted;
    std::shared_ptr<const std::function<void()>> callback; // copied without allocating
    {
        std::lock_guard lock(m_mutex);
        if (composite->seekEpoch < m_epoch.load(std::memory_order_relaxed)) {
            m_counters.stale.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (static_cast<int>(m_pending.size()) >= m_capacity) {
            evicted = std::move(m_pending.front());
            m_pending.erase(m_pending.begin());
            m_counters.dropped.fetch_add(1, std::memory_order_relaxed);
        }
        m_pending.push_back(std::move(composite));
        m_counters.published.fetch_add(1, std::memory_order_relaxed);
        callback = m_onPublish;
    }
    if (callback && *callback) {
        (*callback)();
    }
    return true;
}

void CompositeMailbox::clearPending() {
    std::lock_guard lock(m_mutex);
    m_counters.dropped.fetch_add(m_pending.size(), std::memory_order_relaxed);
    m_pending.clear();
}

int CompositeMailbox::pendingCount() const {
    std::lock_guard lock(m_mutex);
    return static_cast<int>(m_pending.size());
}

void CompositeMailbox::setClock(std::shared_ptr<const PresentationClock> clock) {
    std::lock_guard lock(m_mutex);
    m_clock = std::move(clock);
}

void CompositeMailbox::setPublishCallback(std::function<void()> callback) {
    auto shared = std::make_shared<const std::function<void()>>(std::move(callback));
    std::lock_guard lock(m_mutex);
    m_onPublish = std::move(shared);
}

void CompositeMailbox::dropPendingBefore(std::size_t count) {
    // Called with m_mutex held. Shifts at most `capacity` pointers; never reallocates.
    m_counters.dropped.fetch_add(count, std::memory_order_relaxed);
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(count));
}

CompositeMailbox::Selection CompositeMailbox::acquire(std::int64_t presentHostNs) {
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        m_counters.contended.fetch_add(1, std::memory_order_relaxed);
        return {m_current, false, m_currentBlack, true};
    }

    const std::uint64_t epoch = m_epoch.load(std::memory_order_relaxed);
    // Stale epochs never reach the screen.
    std::size_t staleCount = 0;
    while (staleCount < m_pending.size() && m_pending[staleCount]->seekEpoch < epoch) {
        ++staleCount;
    }
    if (staleCount > 0) {
        m_counters.stale.fetch_add(staleCount, std::memory_order_relaxed);
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(staleCount));
    }

    const PresentationClock* clock = m_clock.get();
    const int direction = clock ? clock->direction() : 0;
    const Ticks presentTime = clock ? clock->presentationTime(presentHostNs) : Ticks{};

    // Choose among pending composites (publish order).
    std::ptrdiff_t chosen = -1;
    if (!m_pending.empty()) {
        if (!clock || direction == 0) {
            chosen = static_cast<std::ptrdiff_t>(m_pending.size()) - 1; // paused / no clock: the newest
        } else {
            for (std::size_t i = 0; i < m_pending.size(); ++i) {
                const PreparedComposite& c = *m_pending[i];
                const bool reached = direction > 0 ? c.targetTime <= presentTime
                                                   : c.targetTime + c.frameDuration > presentTime;
                if (reached) {
                    chosen = static_cast<std::ptrdiff_t>(i); // later in publish order = later in travel order
                }
            }
        }
    }

    Selection out;
    if (chosen >= 0) {
        Ptr next = std::move(m_pending[static_cast<std::size_t>(chosen)]);
        dropPendingBefore(static_cast<std::size_t>(chosen)); // superseded, never shown
        m_pending.erase(m_pending.begin());                  // the moved-from chosen slot
        const PreparedComposite& c = *next;
        m_counters.presented.fetch_add(1, std::memory_order_relaxed);
        if (c.publishedNs > 0) {
            const std::int64_t age = std::max<std::int64_t>(0, steadyNowNs() - c.publishedNs);
            m_counters.lastFrameAgeNs.store(age, std::memory_order_relaxed);
            if (age > m_counters.maxFrameAgeNs.load(std::memory_order_relaxed)) {
                m_counters.maxFrameAgeNs.store(age, std::memory_order_relaxed);
            }
        }
        if (clock && direction != 0) {
            const std::int64_t lateness = direction > 0
                                              ? presentTime.value - (c.targetTime + c.frameDuration).value
                                              : c.targetTime.value - presentTime.value;
            m_counters.lastLatenessTicks.store(lateness, std::memory_order_relaxed);
            if (lateness >= 0) {
                m_counters.late.fetch_add(1, std::memory_order_relaxed);
            }
        }
        out.changed = next != m_current || m_currentBlack;
        m_current = std::move(next);
        m_currentBlack = false;
    } else if (m_current && m_current->seekEpoch < m_displayFloor) {
        // A jump seek with nothing decoded for it yet: the old picture may not stay.
        m_current.reset();
        m_currentBlack = true;
        out.changed = true;
    } else if (m_current && direction != 0) {
        m_counters.repeated.fetch_add(1, std::memory_order_relaxed);
    }
    if (m_currentBlack) {
        m_counters.blackFrames.fetch_add(1, std::memory_order_relaxed);
    }
    out.composite = m_current;
    out.black = m_currentBlack;
    out.keepRefreshing = !m_pending.empty() || direction != 0;
    return out;
}

MailboxStats CompositeMailbox::stats() const {
    MailboxStats s;
    s.published = m_counters.published.load(std::memory_order_relaxed);
    s.presented = m_counters.presented.load(std::memory_order_relaxed);
    s.repeated = m_counters.repeated.load(std::memory_order_relaxed);
    s.dropped = m_counters.dropped.load(std::memory_order_relaxed);
    s.stale = m_counters.stale.load(std::memory_order_relaxed);
    s.late = m_counters.late.load(std::memory_order_relaxed);
    s.blackFrames = m_counters.blackFrames.load(std::memory_order_relaxed);
    s.contended = m_counters.contended.load(std::memory_order_relaxed);
    s.lastFrameAgeNs = m_counters.lastFrameAgeNs.load(std::memory_order_relaxed);
    s.maxFrameAgeNs = m_counters.maxFrameAgeNs.load(std::memory_order_relaxed);
    s.lastLatenessTicks = m_counters.lastLatenessTicks.load(std::memory_order_relaxed);
    return s;
}

} // namespace wabcut::render
