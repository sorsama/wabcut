#pragma once

// Real-time rules for wabcut_audio, small shared constants, and the audit hooks that let tests prove them.
//
// Functions documented as "RT-safe" may be called on the audio callback thread: they are wait-free (or
// lock-free with a bounded retry count), never allocate or free memory, never lock a mutex, never perform a
// system call that can block, never log and never throw. Everything else is "control side" and must stay
// off the audio thread.
//
// Data crosses between the two worlds only through:
//   * std::atomic scalars (parameters, meters, counters),
//   * SpscFrameRing / SpscQueue (bulk audio and small records, one producer, one consumer),
//   * SeqLock<T> (small structs published by a single writer to many readers),
//   * raw pointers whose lifetime is guaranteed by the control side (see AudioEngine::setSource).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace wabcut::audio {

/// Alignment that keeps independently written atomics on separate cache lines (avoids false sharing).
/// Apple Silicon uses 128-byte lines; everything else Wabcut targets uses 64.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLineSize = 128;
#else
inline constexpr std::size_t kCacheLineSize = 64;
#endif

/// Real-time audit hooks. The engine marks its callback (and AudioEngine::pull) as a real-time section;
/// code that must never run there can check rt::inRealtimeSection(), and tests can count violations (a test
/// binary may replace global operator new to count allocations made inside a section).
namespace rt {

/// True on the current thread while it is inside an audio callback. RT-safe (one thread_local read).
bool inRealtimeSection() noexcept;

/// Marks the current thread as inside a real-time section for its lifetime. Nests. RT-safe.
class ScopedRealtimeSection {
public:
    ScopedRealtimeSection() noexcept;
    ~ScopedRealtimeSection();
    ScopedRealtimeSection(const ScopedRealtimeSection&) = delete;
    ScopedRealtimeSection& operator=(const ScopedRealtimeSection&) = delete;
};

/// Flushes denormals to zero on the current thread for its lifetime (x86-64: MXCSR's flush-to-zero and
/// denormals-are-zero bits; the thread's own mode comes back afterwards). A filter ringing down in a long silence
/// reaches the denormal range, where x86 spends about 100 cycles an operation: the audio callback, the feeder and
/// export rendering run with it. Elsewhere it does nothing. Nests. RT-safe.
class ScopedDenormalsOff {
public:
    ScopedDenormalsOff() noexcept;
    ~ScopedDenormalsOff();
    ScopedDenormalsOff(const ScopedDenormalsOff&) = delete;
    ScopedDenormalsOff& operator=(const ScopedDenormalsOff&) = delete;

private:
    unsigned int m_saved = 0; ///< the two mode bits as they were
};

/// Number of CheckedMutex locks taken inside a real-time section since process start (should stay 0).
std::uint64_t lockViolations() noexcept;
void recordLockViolation() noexcept;

/// std::mutex that records a violation when it is locked inside a real-time section. Use it for every mutex
/// in audio/playback code, so tests can prove the callback never takes one. Meets Lockable.
class CheckedMutex {
public:
    void lock() {
        if (inRealtimeSection()) {
            recordLockViolation();
        }
        m_mutex.lock();
    }
    bool try_lock() {
        if (inRealtimeSection()) {
            recordLockViolation();
        }
        return m_mutex.try_lock();
    }
    void unlock() { m_mutex.unlock(); }

private:
    std::mutex m_mutex;
};

} // namespace rt

} // namespace wabcut::audio
