#pragma once

// A shared_ptr that threads load and store atomically: std::atomic<std::shared_ptr<T>> (C++20, P0718) where the
// standard library has it (MSVC's, libstdc++), and the same load / store over a plain shared_ptr guarded by a spin lock
// where it does not yet (libc++ as of LLVM 23). Header-only; wabcut_target_defaults() puts src/platform/include on every
// Wabcut target's include path.

#include <atomic>
#include <memory>
#include <utility>

namespace wabcut::platform {

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L

template <class T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;

#else

template <class T>
class AtomicSharedPtr {
public:
    AtomicSharedPtr() noexcept = default;
    AtomicSharedPtr(std::shared_ptr<T> value) noexcept : m_value(std::move(value)) {} // NOLINT: like std::atomic's
    AtomicSharedPtr(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

    std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const noexcept {
        lock();
        std::shared_ptr<T> copy = m_value;
        unlock();
        return copy;
    }
    void store(std::shared_ptr<T> value, std::memory_order = std::memory_order_seq_cst) noexcept {
        lock();
        m_value.swap(value);
        unlock();
        // `value` now holds the old pointer: released here, outside the lock.
    }

private:
    void lock() const noexcept {
        while (m_lock.test_and_set(std::memory_order_acquire)) {
            m_lock.wait(true, std::memory_order_relaxed);
        }
    }
    void unlock() const noexcept {
        m_lock.clear(std::memory_order_release);
        m_lock.notify_one();
    }

    mutable std::atomic_flag m_lock;
    std::shared_ptr<T> m_value;
};

#endif

} // namespace wabcut::platform
