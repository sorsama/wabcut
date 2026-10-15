#pragma once

// Lock-free single-producer / single-consumer ring buffer of interleaved float frames. It is the hand-off
// from a decoder thread (producer) to the audio thread (consumer), or the other way round for capture.

#include <wabcut/audio/RealTime.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace wabcut::audio {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // "structure was padded due to alignment specifier": the padding is the point.
#endif

/// Bounded SPSC queue of audio frames (`channels` floats per frame), wait-free on both sides.
///
/// Exactly one thread may call the producer functions and exactly one (other) thread the consumer functions;
/// which threads those are may change only while both sides are quiescent (e.g. playback stopped). Only
/// whole frames are transferred. The constructor allocates; nothing else does, so every member except the
/// constructor and destructor is RT-safe.
///
/// Memory ordering. Each side owns one monotonically increasing 64-bit frame counter (they never wrap in
/// practice: 2^64 frames is millions of years at 192 kHz). The producer copies samples into free slots and
/// then publishes them with a release store of the write counter; the consumer's acquire load of that counter
/// therefore happens-after the sample stores, so it reads complete data. Symmetrically, the consumer releases
/// slots with a release store of the read counter after it has copied the samples out, and the producer's
/// acquire load of the read counter guarantees those reads completed before it overwrites the slots. Each side
/// caches the other side's counter and only re-reads it when the cached value says there is not enough room
/// or data, which keeps the shared cache lines quiet in steady state.
class SpscFrameRing {
public:
    /// `capacityFrames` is rounded up to a power of two (minimum 1); `channels` is clamped to at least 1.
    SpscFrameRing(std::size_t capacityFrames, int channels)
        : m_channels(static_cast<std::size_t>(std::max(channels, 1))),
          m_capacity(std::bit_ceil(std::max<std::size_t>(capacityFrames, 1))), m_mask(m_capacity - 1),
          m_data(std::make_unique<float[]>(m_capacity * m_channels)) {}

    SpscFrameRing(const SpscFrameRing&) = delete;
    SpscFrameRing& operator=(const SpscFrameRing&) = delete;

    int channels() const noexcept { return static_cast<int>(m_channels); }
    std::size_t capacityFrames() const noexcept { return m_capacity; }

    // ---- Producer side --------------------------------------------------------------------------------------

    /// Frames that can be written right now. Producer thread only.
    std::size_t writableFrames() const noexcept {
        const std::uint64_t w = m_write.load(std::memory_order_relaxed);
        const std::uint64_t r = m_read.load(std::memory_order_acquire);
        return m_capacity - static_cast<std::size_t>(w - r);
    }

    /// Appends as many whole frames of `interleaved` as fit and returns the number of frames written.
    /// A trailing partial frame in `interleaved` is ignored. Producer thread only.
    std::size_t write(std::span<const float> interleaved) noexcept {
        const std::size_t wanted = interleaved.size() / m_channels;
        const std::uint64_t w = m_write.load(std::memory_order_relaxed); // our own counter
        if (m_capacity - static_cast<std::size_t>(w - m_cachedRead) < wanted) {
            m_cachedRead = m_read.load(std::memory_order_acquire); // see class comment
        }
        const std::size_t n = std::min(wanted, m_capacity - static_cast<std::size_t>(w - m_cachedRead));
        if (n == 0) {
            return 0;
        }
        const std::size_t start = static_cast<std::size_t>(w) & m_mask;
        const std::size_t first = std::min(n, m_capacity - start);
        std::copy_n(interleaved.data(), first * m_channels, m_data.get() + start * m_channels);
        std::copy_n(interleaved.data() + first * m_channels, (n - first) * m_channels, m_data.get());
        m_write.store(w + n, std::memory_order_release); // publish the samples written above
        return n;
    }

    // ---- Consumer side --------------------------------------------------------------------------------------

    /// Frames that can be read right now. Consumer thread only.
    std::size_t readableFrames() const noexcept {
        const std::uint64_t r = m_read.load(std::memory_order_relaxed);
        const std::uint64_t w = m_write.load(std::memory_order_acquire);
        return static_cast<std::size_t>(w - r);
    }

    /// Copies up to interleaved.size() / channels frames out and returns the number of frames read. The rest
    /// of `interleaved` is left untouched (callers usually zero-fill it as an underrun). Consumer thread only.
    std::size_t read(std::span<float> interleaved) noexcept {
        const std::size_t wanted = interleaved.size() / m_channels;
        const std::uint64_t r = m_read.load(std::memory_order_relaxed); // our own counter
        if (static_cast<std::size_t>(m_cachedWrite - r) < wanted) {
            m_cachedWrite = m_write.load(std::memory_order_acquire); // see class comment
        }
        const std::size_t n = std::min(wanted, static_cast<std::size_t>(m_cachedWrite - r));
        if (n == 0) {
            return 0;
        }
        const std::size_t start = static_cast<std::size_t>(r) & m_mask;
        const std::size_t first = std::min(n, m_capacity - start);
        std::copy_n(m_data.get() + start * m_channels, first * m_channels, interleaved.data());
        std::copy_n(m_data.get(), (n - first) * m_channels, interleaved.data() + first * m_channels);
        m_read.store(r + n, std::memory_order_release); // hand the slots back only after copying them out
        return n;
    }

    /// Drops up to `frames` frames without copying them; returns the number dropped. Consumer thread only.
    std::size_t discard(std::size_t frames) noexcept {
        const std::uint64_t r = m_read.load(std::memory_order_relaxed);
        m_cachedWrite = m_write.load(std::memory_order_acquire);
        const std::size_t n = std::min(frames, static_cast<std::size_t>(m_cachedWrite - r));
        m_read.store(r + n, std::memory_order_release);
        return n;
    }

    /// Drops everything currently readable (e.g. after a seek). Consumer thread only.
    std::size_t discardAll() noexcept { return discard(m_capacity); }

    /// Total frames ever written / read. Monotonic; handy for underrun accounting and tests. Any thread
    /// (the value may be stale by the time it is used).
    std::uint64_t totalWritten() const noexcept { return m_write.load(std::memory_order_acquire); }
    std::uint64_t totalRead() const noexcept { return m_read.load(std::memory_order_acquire); }

private:
    const std::size_t m_channels;
    const std::size_t m_capacity; // frames, power of two
    const std::size_t m_mask;
    const std::unique_ptr<float[]> m_data;

    // Producer-owned cache line: its counter plus its cached copy of the consumer's counter.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> m_write{0};
    std::uint64_t m_cachedRead = 0;
    // Consumer-owned cache line.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> m_read{0};
    std::uint64_t m_cachedWrite = 0;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace wabcut::audio
