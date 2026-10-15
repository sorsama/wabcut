#pragma once

// Lock-free single-producer / single-consumer queue of small trivially copyable records (e.g. chunk headers
// that accompany audio in an SpscFrameRing).

#include <wabcut/audio/RealTime.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace wabcut::audio {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // "structure was padded due to alignment specifier": the padding is the point.
#endif

/// Bounded SPSC FIFO, wait-free on both sides; the constructor is the only allocation. Same memory-ordering
/// scheme as SpscFrameRing: the producer stores the record, then publishes it with a release store of its
/// counter; the consumer acquires that counter before reading, and hands the slot back with a release store
/// of its own counter after copying it out.
template <typename T>
    requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t capacity)
        : m_capacity(std::bit_ceil(std::max<std::size_t>(capacity, 1))), m_mask(m_capacity - 1),
          m_items(std::make_unique<T[]>(m_capacity)) {}

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    std::size_t capacity() const noexcept { return m_capacity; }

    /// Producer: append `item`; false when full.
    bool push(const T& item) noexcept {
        const std::uint64_t w = m_write.load(std::memory_order_relaxed);
        if (w - m_cachedRead == m_capacity) {
            m_cachedRead = m_read.load(std::memory_order_acquire);
            if (w - m_cachedRead == m_capacity) {
                return false;
            }
        }
        m_items[static_cast<std::size_t>(w) & m_mask] = item;
        m_write.store(w + 1, std::memory_order_release);
        return true;
    }

    /// Producer: free slots right now.
    std::size_t freeSlots() const noexcept {
        return m_capacity - static_cast<std::size_t>(m_write.load(std::memory_order_relaxed) -
                                                     m_read.load(std::memory_order_acquire));
    }

    /// Consumer: the oldest record without removing it, or nullptr when empty.
    const T* front() noexcept {
        const std::uint64_t r = m_read.load(std::memory_order_relaxed);
        if (r == m_cachedWrite) {
            m_cachedWrite = m_write.load(std::memory_order_acquire);
            if (r == m_cachedWrite) {
                return nullptr;
            }
        }
        return &m_items[static_cast<std::size_t>(r) & m_mask];
    }

    /// Consumer: mutable access to the oldest record (consumer-side bookkeeping, e.g. a partial read).
    T* frontMutable() noexcept { return const_cast<T*>(front()); }

    /// Consumer: remove the oldest record (requires front() != nullptr).
    void pop() noexcept { m_read.store(m_read.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    /// Either side: records queued right now (approximate when racing).
    std::size_t size() const noexcept {
        return static_cast<std::size_t>(m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire));
    }

private:
    const std::size_t m_capacity;
    const std::size_t m_mask;
    const std::unique_ptr<T[]> m_items;

    alignas(kCacheLineSize) std::atomic<std::uint64_t> m_write{0};
    std::uint64_t m_cachedRead = 0;
    alignas(kCacheLineSize) std::atomic<std::uint64_t> m_read{0};
    std::uint64_t m_cachedWrite = 0;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace wabcut::audio
