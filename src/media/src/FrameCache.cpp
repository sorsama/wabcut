#include <wabcut/media/FrameCache.h>

#include <iterator>
#include <utility>

namespace wabcut::media {

namespace {

std::uint64_t combine(std::uint64_t h, std::uint64_t v) {
    return h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
}

} // namespace

FrameKey frameKeyOf(const FrameHandle& frame, int stream, std::uint32_t representation) {
    return FrameKey{frame.source, stream, frame.streamPts, representation, frame.device()};
}

std::uint64_t mediaKeyFromPath(std::string_view utf8Path) {
    std::uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a 64
    for (const char c : utf8Path) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// ---- FramePin
// --------------------------------------------------------------------------------------------------------

FramePin::FramePin(FramePin&& other) noexcept
    : m_cache(std::exchange(other.m_cache, nullptr)), m_key(other.m_key), m_frame(std::move(other.m_frame)) {}

FramePin& FramePin::operator=(FramePin&& other) noexcept {
    if (this != &other) {
        release();
        m_cache = std::exchange(other.m_cache, nullptr);
        m_key = other.m_key;
        m_frame = std::move(other.m_frame);
    }
    return *this;
}

FramePin::~FramePin() {
    release();
}

void FramePin::release() {
    if (m_cache) {
        m_cache->unpin(m_key);
        m_cache = nullptr;
    }
    m_frame = FrameHandle{};
}

// ---- FrameCache
// ------------------------------------------------------------------------------------------------------

std::size_t FrameCache::KeyHash::operator()(const FrameKey& k) const noexcept {
    std::uint64_t h = k.source.fingerprint * 0x9e3779b97f4a7c15ULL;
    h = combine(h, k.source.generation);
    h = combine(h, static_cast<std::uint64_t>(k.pts));
    h = combine(h, static_cast<std::uint32_t>(k.stream));
    h = combine(h, k.representation);
    h = combine(h, (std::uint64_t{k.decode.id} << 32) | k.decode.generation);
    return static_cast<std::size_t>(h);
}

FrameCache::FrameCache(FrameCacheBudget budget) {
    m_stats.budget = budget;
}

void FrameCache::account(const Entry& e, bool add) {
    std::size_t& pool = e.gpu ? m_stats.gpuBytes : m_stats.cpuBytes;
    pool = add ? pool + e.bytes : pool - e.bytes;
}

void FrameCache::removeLocked(EntryList::iterator it) {
    account(*it, false);
    if (it->frame) {
        --m_stats.entries;
    } else {
        --m_stats.inFlight;
    }
    if (it->pins > 0) {
        --m_stats.pinned;
    }
    m_index.erase(it->key);
    m_lru.erase(it);
}

std::optional<FrameHandle> FrameCache::get(const FrameKey& key) {
    std::lock_guard lock(m_mutex);
    const auto it = m_index.find(key);
    if (it == m_index.end() || !it->second->frame) {
        ++m_stats.misses;
        return std::nullopt;
    }
    ++m_stats.hits;
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    return it->second->frame;
}

FramePin FrameCache::pin(const FrameKey& key) {
    std::lock_guard lock(m_mutex);
    const auto it = m_index.find(key);
    if (it == m_index.end() || !it->second->frame) {
        ++m_stats.misses;
        return {};
    }
    ++m_stats.hits;
    Entry& e = *it->second;
    if (e.pins++ == 0) {
        ++m_stats.pinned;
    }
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    return FramePin(this, key, *e.frame);
}

void FrameCache::unpin(const FrameKey& key) {
    std::lock_guard lock(m_mutex);
    const auto it = m_index.find(key);
    if (it == m_index.end() || it->second->pins == 0) {
        return;
    }
    if (--it->second->pins == 0) {
        --m_stats.pinned;
        evictLocked(); // the pin may have been holding the cache over budget
    }
}

void FrameCache::put(const FrameKey& key, FrameHandle frame) {
    const std::size_t bytes = frame.byteSize();
    const bool gpu = frame.payloadKind() != PayloadKind::CpuPlanes;
    std::lock_guard lock(m_mutex);
    int pins = 0;
    if (const auto it = m_index.find(key); it != m_index.end()) {
        pins = it->second->pins;
        removeLocked(it->second);
    }
    const std::size_t budget = gpu ? m_stats.budget.gpuBytes : m_stats.budget.cpuBytes;
    if (bytes > budget && pins == 0) {
        ++m_stats.rejected;
        return;
    }
    m_lru.push_front(Entry{key, std::move(frame), bytes, gpu, pins});
    m_index.emplace(key, m_lru.begin());
    account(m_lru.front(), true);
    ++m_stats.entries;
    if (pins > 0) {
        ++m_stats.pinned;
    }
    ++m_stats.insertions;
    evictLocked();
}

bool FrameCache::beginDecode(const FrameKey& key, std::size_t estimatedBytes, bool gpu) {
    std::lock_guard lock(m_mutex);
    if (m_index.contains(key)) {
        return false;
    }
    m_lru.push_front(Entry{key, std::nullopt, estimatedBytes, gpu, 0});
    m_index.emplace(key, m_lru.begin());
    account(m_lru.front(), true);
    ++m_stats.inFlight;
    evictLocked();
    return true;
}

void FrameCache::abortDecode(const FrameKey& key) {
    std::lock_guard lock(m_mutex);
    if (const auto it = m_index.find(key); it != m_index.end() && !it->second->frame) {
        removeLocked(it->second);
    }
}

bool FrameCache::isInFlight(const FrameKey& key) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_index.find(key);
    return it != m_index.end() && !it->second->frame;
}

bool FrameCache::contains(const FrameKey& key) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_index.find(key);
    return it != m_index.end() && it->second->frame.has_value();
}

void FrameCache::erase(const FrameKey& key) {
    std::lock_guard lock(m_mutex);
    if (const auto it = m_index.find(key);
        it != m_index.end() && it->second->pins == 0 && it->second->frame) {
        removeLocked(it->second);
    }
}

void FrameCache::eraseSource(std::uint64_t fingerprint) {
    std::lock_guard lock(m_mutex);
    for (auto it = m_lru.begin(); it != m_lru.end();) {
        const auto next = std::next(it);
        if (it->key.source.fingerprint == fingerprint && it->pins == 0 && it->frame) {
            removeLocked(it);
        }
        it = next;
    }
}

void FrameCache::clear() {
    std::lock_guard lock(m_mutex);
    for (auto it = m_lru.begin(); it != m_lru.end();) {
        const auto next = std::next(it);
        if (it->pins == 0 && it->frame) {
            removeLocked(it);
        }
        it = next;
    }
}

void FrameCache::setBudget(FrameCacheBudget budget) {
    std::lock_guard lock(m_mutex);
    m_stats.budget = budget;
    evictLocked();
}

FrameCacheStats FrameCache::stats() const {
    std::lock_guard lock(m_mutex);
    return m_stats;
}

void FrameCache::resetCounters() {
    std::lock_guard lock(m_mutex);
    m_stats.hits = 0;
    m_stats.misses = 0;
    m_stats.insertions = 0;
    m_stats.evictions = 0;
    m_stats.rejected = 0;
}

void FrameCache::evictLocked() {
    const auto over = [&](bool gpu) {
        return gpu ? m_stats.gpuBytes > m_stats.budget.gpuBytes : m_stats.cpuBytes > m_stats.budget.cpuBytes;
    };
    // Walk from least to most recently used, dropping evictable entries of an over-budget pool.
    auto it = m_lru.end();
    while (it != m_lru.begin() && (over(false) || over(true))) {
        --it;
        if (it->pins > 0 || !it->frame || !over(it->gpu)) {
            continue; // pinned, in flight, or its pool is within budget
        }
        const auto victim = it;
        ++it; // erasing a list node does not invalidate the others
        removeLocked(victim);
        ++m_stats.evictions;
    }
}

} // namespace wabcut::media
