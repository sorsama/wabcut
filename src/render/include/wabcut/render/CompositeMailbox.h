#pragma once

// CompositeMailbox: the bounded hand-off from the playback scheduler to the render thread (contract rev 2).
//
// The scheduler publishes immutable PreparedComposites a few frames ahead. The render thread calls acquire()
// once per refresh: it never blocks (a contended lock just keeps the current picture), never decodes, and
// picks the composite that is due at the moment the refreshed frame becomes visible, according to the
// PresentationClock the playback controller installed (the audio master clock plus display latency).
// Superseded composites are dropped.
//
// Seek epochs (latest seek wins): beginSeek() starts a new epoch, and composites of older epochs are
// discarded wherever they are. What stays on screen meanwhile is the seek's choice:
//   * SeekDisplay::Black: nothing of an older epoch may stay visible (a jump; the contract's "black only
//   after a seek
//     with nothing decoded").
//   * SeekDisplay::HoldPrevious: the current picture stays until the new epoch delivers (scrubbing, frame
//   steps),
//     so scrubbing never flashes black.
// Within an epoch an underrun holds the last frame.
//
// Release discipline: dropping a composite reference on the render thread must not free decoder memory, so
// producers publish pooled composites (CompositePool keeps one reference to each, see wabcut/playback).

#include <wabcut/model/Time.h>
#include <wabcut/render/PreparedComposite.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace wabcut::render {

/// Maps the steady clock to the timeline for presentation. Implementations are lock-free (the render thread
/// calls them on every refresh).
class PresentationClock {
public:
    virtual ~PresentationClock() = default;
    /// Timeline time to show in a frame that becomes visible at steady-clock time `hostNs`.
    virtual Ticks presentationTime(std::int64_t hostNs) const noexcept = 0;
    /// Sign of the transport speed: > 0 forward, < 0 reverse, 0 paused/stopped.
    virtual int direction() const noexcept = 0;
};

enum class SeekDisplay : std::uint8_t { Black, HoldPrevious };

struct MailboxStats {
    std::uint64_t published = 0; ///< Accepted by publish().
    std::uint64_t presented = 0; ///< Composites that went on screen.
    std::uint64_t repeated = 0; ///< Refreshes that re-showed the current composite while the transport moved.
    std::uint64_t dropped = 0;  ///< Published but superseded before they were shown (or evicted when full).
    std::uint64_t stale = 0;    ///< Discarded because a newer seek epoch began.
    std::uint64_t late = 0;     ///< Shown after the end of their own frame interval.
    std::uint64_t blackFrames = 0; ///< Refreshes that showed black after a Black seek.
    std::uint64_t contended = 0;   ///< acquire() calls that found the lock busy and kept the current picture.
    std::int64_t lastFrameAgeNs = 0; ///< Steady time from publish to the refresh that first showed it.
    std::int64_t maxFrameAgeNs = 0;
    std::int64_t lastLatenessTicks = 0; ///< Presentation time minus the end of the shown frame (> 0 = late).
};

class CompositeMailbox {
public:
    using Ptr = std::shared_ptr<const PreparedComposite>;

    /// `capacity` bounds the composites waiting to be shown (the scheduler's lookahead).
    explicit CompositeMailbox(int capacity = 8);
    CompositeMailbox(const CompositeMailbox&) = delete;
    CompositeMailbox& operator=(const CompositeMailbox&) = delete;

    int capacity() const noexcept { return m_capacity; }

    // ---- Producer side (scheduler / controller threads) ----
    /// Starts a new seek epoch and returns it. Pending composites of older epochs are discarded.
    std::uint64_t beginSeek(SeekDisplay display);
    std::uint64_t epoch() const noexcept { return m_epoch.load(std::memory_order_acquire); }
    /// Queues `composite`. Rejected (false, counted stale) when its epoch is older than the current one. When
    /// the queue is full the oldest pending composite is dropped.
    bool publish(Ptr composite);
    /// Drops every pending composite (they were never shown).
    void clearPending();
    int pendingCount() const;
    /// Installs the presentation clock (null: always show the newest composite).
    void setClock(std::shared_ptr<const PresentationClock> clock);
    /// Called on the publishing thread after every successful publish(), e.g. to schedule a monitor refresh.
    /// Must be cheap and thread-safe.
    void setPublishCallback(std::function<void()> callback);

    // ---- Consumer side (render thread): never blocks ----
    struct Selection {
        Ptr composite;               ///< What to draw; null means black (or nothing has been published yet).
        bool changed = false;        ///< The picture differs from the previous acquire().
        bool black = false;          ///< Black because of a seek with nothing decoded yet.
        bool keepRefreshing = false; ///< Pending composites or a moving transport: refresh again next frame.
    };
    /// The composite to show in a refresh that becomes visible at steady-clock time `presentHostNs`.
    Selection acquire(std::int64_t presentHostNs);
    /// The composite currently on screen (render thread).
    const Ptr& current() const noexcept { return m_current; }

    MailboxStats stats() const;

private:
    void dropPendingBefore(std::size_t count);

    const int m_capacity;
    mutable std::mutex m_mutex;
    std::vector<Ptr> m_pending; // publish order; capacity reserved up front
    std::shared_ptr<const PresentationClock> m_clock;
    std::shared_ptr<const std::function<void()>> m_onPublish;
    std::atomic<std::uint64_t> m_epoch{0};
    std::uint64_t m_displayFloor = 0; // composites below this epoch may not stay on screen

    // Consumer state (render thread; also touched under m_mutex by acquire()).
    Ptr m_current;
    bool m_currentBlack = false;

    struct Counters {
        std::atomic<std::uint64_t> published{0}, presented{0}, repeated{0}, dropped{0}, stale{0}, late{0},
            blackFrames{0}, contended{0};
        std::atomic<std::int64_t> lastFrameAgeNs{0}, maxFrameAgeNs{0}, lastLatenessTicks{0};
    } m_counters;
};

} // namespace wabcut::render
