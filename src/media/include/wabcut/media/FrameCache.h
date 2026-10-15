#pragma once

// Thread-safe LRU cache of decoded frames with separate CPU and GPU byte budgets (architecture rev 2).

#include <wabcut/media/FrameHandle.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace wabcut::media {

/// Identifies one decoded frame independently of which decoder, seek or session produced it.
struct FrameKey {
    SourceIdentity source; ///< Source fingerprint + generation (FrameHandle::source).
    int stream = 0;        ///< Container stream index.
    std::int64_t pts = 0;  ///< ORIGINAL stream PTS (FrameHandle::streamPts), not a frame index.
    std::uint32_t representation =
        0;                ///< 0 = original media; proxies/representations use their own generation.
    DeviceToken decode{}; ///< Decode path/device that produced the pixels (FrameHandle::device()).

    bool operator==(const FrameKey&) const = default;
};

/// The key of `frame` (decoded from container stream `stream`).
FrameKey frameKeyOf(const FrameHandle& frame, int stream, std::uint32_t representation = 0);

/// Stable 64-bit key for a UTF-8 path (FNV-1a). Rev-1 helper; cache keys use SourceIdentity now.
std::uint64_t mediaKeyFromPath(std::string_view utf8Path);

struct FrameCacheBudget {
    std::size_t cpuBytes = 0; ///< CpuPlanes payloads.
    std::size_t gpuBytes = 0; ///< Native payloads (estimated surface size; accounting only until they exist).
};

struct FrameCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t insertions = 0;
    std::uint64_t evictions = 0; ///< Entries dropped to stay within a budget.
    std::uint64_t rejected = 0;  ///< Frames larger than their whole budget, never stored.
    std::size_t cpuBytes = 0;    ///< Bytes of CPU frames, including pinned ones and in-flight reservations.
    std::size_t gpuBytes = 0;
    std::size_t entries = 0;  ///< Stored frames (not counting in-flight reservations).
    std::size_t pinned = 0;   ///< Entries with at least one live FramePin.
    std::size_t inFlight = 0; ///< Outstanding beginDecode() reservations.
    FrameCacheBudget budget;
};

class FrameCache;

/// Keeps one cached frame from being evicted while it is in use (e.g. the frame on screen). Move-only; the
/// cache must outlive its pins.
class FramePin {
public:
    FramePin() = default;
    FramePin(FramePin&& other) noexcept;
    FramePin& operator=(FramePin&& other) noexcept;
    FramePin(const FramePin&) = delete;
    FramePin& operator=(const FramePin&) = delete;
    ~FramePin();

    explicit operator bool() const { return m_cache != nullptr; }
    const FrameHandle& frame() const { return m_frame; }
    const FrameKey& key() const { return m_key; }
    void release();

private:
    friend class FrameCache;
    FramePin(FrameCache* cache, FrameKey key, FrameHandle frame)
        : m_cache(cache), m_key(key), m_frame(std::move(frame)) {}

    FrameCache* m_cache = nullptr;
    FrameKey m_key;
    FrameHandle m_frame;
};

/// LRU frame cache. Every member function is thread-safe (one internal mutex; nothing blocks on I/O or
/// decoding), but the render thread must not call it (Principle 8): render receives frames through
/// PreparedComposite. CPU and native frames have separate budgets. Pinned frames and in-flight reservations
/// are never evicted: when they alone exceed a budget, the cache stays over budget until they are released.
/// The cache holds shared references: an evicted frame stays alive until its last user drops it.
class FrameCache {
public:
    explicit FrameCache(FrameCacheBudget budget);
    /// CPU-only cache (no GPU budget).
    explicit FrameCache(std::size_t cpuByteBudget) : FrameCache(FrameCacheBudget{cpuByteBudget, 0}) {}

    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    /// The frame, marked most recently used, or nullopt (a miss; in-flight reservations are misses too).
    std::optional<FrameHandle> get(const FrameKey& key);
    /// Like get(), but also pins the entry until the returned pin is released. Empty pin on a miss.
    FramePin pin(const FrameKey& key);
    /// Inserts or replaces the frame as most recently used (completing a reservation), then evicts unpinned
    /// least recently used entries of the same kind until the budget holds. A frame larger than its whole
    /// budget is not stored. Replacing a pinned entry keeps it pinned.
    void put(const FrameKey& key, FrameHandle frame);

    /// Reserves `key` for a decode in progress, so concurrent requesters see it in flight instead of decoding
    /// it again. The estimated bytes count against the budget and are never evicted. Returns false (and
    /// reserves nothing) if the frame is already cached or in flight. Complete with put() or abandon with
    /// abortDecode().
    bool beginDecode(const FrameKey& key, std::size_t estimatedBytes, bool gpu = false);
    void abortDecode(const FrameKey& key);
    bool isInFlight(const FrameKey& key) const;

    bool
    contains(const FrameKey& key) const; ///< A stored frame (not a reservation); no LRU update, no stats.
    void erase(const FrameKey& key);     ///< Unpinned entries only.
    void eraseSource(std::uint64_t fingerprint); ///< Every unpinned frame of a source (e.g. after relink).
    void clear();                                ///< Every unpinned, completed entry.

    /// Changes the budgets; shrinking evicts immediately.
    void setBudget(FrameCacheBudget budget);

    FrameCacheStats stats() const;
    void resetCounters(); ///< Zeroes hits/misses/insertions/evictions/rejected.

private:
    friend class FramePin;

    struct KeyHash {
        std::size_t operator()(const FrameKey& k) const noexcept;
    };
    struct Entry {
        FrameKey key;
        std::optional<FrameHandle> frame; ///< Empty while in flight.
        std::size_t bytes = 0;
        bool gpu = false;
        int pins = 0;
    };
    using EntryList = std::list<Entry>;

    void unpin(const FrameKey& key);
    void account(const Entry& e, bool add);
    void removeLocked(EntryList::iterator it);
    void evictLocked();

    mutable std::mutex m_mutex;
    EntryList m_lru; // front = most recently used
    std::unordered_map<FrameKey, EntryList::iterator, KeyHash> m_index;
    FrameCacheStats m_stats;
};

} // namespace wabcut::media
