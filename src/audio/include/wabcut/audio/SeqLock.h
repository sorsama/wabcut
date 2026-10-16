#pragma once

// Single-writer / multi-reader publication of a small trivially copyable struct, without locks on either side.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>

namespace wabcut::audio {

/// Sequence lock. One writer at a time publishes a T; any number of readers take consistent snapshots.
///
/// The writer is wait-free, so it is safe on the audio thread. Readers never block the writer: when they race
/// with a write they retry. tryLoad() makes a single attempt and is therefore also RT-safe (an audio thread
/// that loses the race keeps its previous value and tries again next block); load() retries until it gets a
/// consistent copy and is meant for non-real-time readers.
///
/// Writers must be serialised externally (one thread, or a mutex among control threads); concurrent store()
/// calls are a bug.
///
/// Memory ordering, after H.-J. Boehm, "Can Seqlocks Get Along With Programming Language Memory Models?"
/// (MSPC 2012). The payload is stored as relaxed std::atomic words, so racing reads are not data races (no UB),
/// merely possibly torn, and the sequence number detects tearing:
///   writer: seq = s+1 (relaxed); release fence; payload words (relaxed); seq = s+2 (release)
///   reader: s1 = seq (acquire); payload words (relaxed); acquire fence; s2 = seq (relaxed); valid iff s1 == s2
///           and s1 is even.
/// If a reader's relaxed payload load observes any word written after the writer's release fence, the
/// fence-fence synchronisation makes the odd sequence store visible to the reader's second load, so s2 != s1
/// and the copy is rejected. If s1 is even and comes from the writer's final release store, the acquire load
/// makes all payload words of that write visible.
template <typename T>
    requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class SeqLock {
public:
    SeqLock() { store(T{}); }
    explicit SeqLock(const T& initial) { store(initial); }

    SeqLock(const SeqLock&) = delete;
    SeqLock& operator=(const SeqLock&) = delete;

    /// Publish `value`. Wait-free, RT-safe. Single writer (see class comment).
    void store(const T& value) noexcept {
        std::array<std::uint64_t, kWords> words{};
        std::memcpy(words.data(), &value, sizeof(T));
        const std::uint64_t s = m_seq.load(std::memory_order_relaxed);
        m_seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < kWords; ++i) {
            m_words[i].store(words[i], std::memory_order_relaxed);
        }
        m_seq.store(s + 2, std::memory_order_release);
    }

    /// One attempt to read a consistent snapshot. Returns false (leaving `out` untouched) if a write was in
    /// progress. Wait-free, RT-safe.
    bool tryLoad(T& out) const noexcept {
        const std::uint64_t s1 = m_seq.load(std::memory_order_acquire);
        if ((s1 & 1U) != 0) {
            return false;
        }
        std::array<std::uint64_t, kWords> words;
        for (std::size_t i = 0; i < kWords; ++i) {
            words[i] = m_words[i].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m_seq.load(std::memory_order_relaxed) != s1) {
            return false;
        }
        // To void*: T is trivially copyable, but GCC's -Wclass-memaccess also flags types whose default constructor is
        // not trivial (default member initializers).
        std::memcpy(static_cast<void*>(&out), words.data(), sizeof(T));
        return true;
    }

    /// Read a consistent snapshot, retrying while a write is in progress. Lock-free, not RT-safe in theory
    /// (a writer preempted mid-write makes readers spin), so keep it off the audio thread.
    T load() const noexcept {
        T out{};
        for (unsigned attempt = 0; !tryLoad(out); ++attempt) {
            if (attempt >= 64) {
                std::this_thread::yield(); // the writer was probably preempted mid-write
            }
        }
        return out;
    }

    /// Number of completed writes, including the initial value. Any thread.
    std::uint64_t version() const noexcept { return m_seq.load(std::memory_order_acquire) / 2; }

private:
    static constexpr std::size_t kWords = (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

    std::atomic<std::uint64_t> m_seq{0};
    std::array<std::atomic<std::uint64_t>, kWords> m_words{};
};

} // namespace wabcut::audio
