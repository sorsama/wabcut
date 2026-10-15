#pragma once

// Handle to a cancellable background job (thumbnails, waveform peaks, ...).

#include <wabcut/model/Error.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace wabcut::media {

namespace detail {

template <typename T> struct JobState {
    using Result = std::expected<T, Error>;
    using Callback = std::function<void(const Result&)>;

    mutable std::mutex mutex;
    mutable std::condition_variable finished;
    std::optional<Result> result;
    std::stop_source stop;
    std::atomic<float> progress{0.0f};
    Callback onDone;

    /// Publishes the result exactly once, then calls the completion callback (outside the lock).
    void complete(Result r) {
        Callback cb;
        {
            std::lock_guard lock(mutex);
            if (result) {
                return;
            }
            result.emplace(std::move(r));
            progress.store(1.0f, std::memory_order_relaxed);
            cb = std::move(onDone);
        }
        finished.notify_all();
        if (cb) {
            cb(*result); // result is never modified again once set
        }
    }
};

} // namespace detail

/// Shared handle to a background job producing T. Copies refer to the same job. Thread-safe.
///
/// Dropping every handle does not cancel the job; call cancel() for that. A job that is cancelled (or whose
/// service shuts down) before it finishes completes with ErrorCode::Cancelled.
template <typename T> class Job {
public:
    using Result = std::expected<T, Error>;

    Job() = default;
    explicit Job(std::shared_ptr<detail::JobState<T>> state) : m_state(std::move(state)) {}

    bool isValid() const { return m_state != nullptr; }

    /// Requests cancellation. Has no effect once the job has finished.
    void cancel() const {
        if (m_state) {
            m_state->stop.request_stop();
        }
    }

    bool isFinished() const {
        if (!m_state) {
            return false;
        }
        std::lock_guard lock(m_state->mutex);
        return m_state->result.has_value();
    }

    /// Progress in [0, 1]; 1 once finished.
    float progress() const { return m_state ? m_state->progress.load(std::memory_order_relaxed) : 0.0f; }

    /// Blocks until the job finishes and returns its result. The reference stays valid while a handle exists.
    /// The completion callback runs after the result is published, so it may still be running (or not have
    /// started) when wait() returns; synchronise with the callback itself if that matters.
    const Result& wait() const& {
        std::unique_lock lock(m_state->mutex);
        m_state->finished.wait(lock, [&] { return m_state->result.has_value(); });
        return *m_state->result;
    }
    /// On a temporary handle (`pool.requestFrame(r).wait()`) the result is returned by value, because the
    /// handle, possibly the last owner of the result, dies at the end of the expression.
    Result wait() && { return static_cast<const Job&>(*this).wait(); }

    /// Waits up to `timeout`; true if the job has finished.
    bool waitFor(std::chrono::milliseconds timeout) const {
        std::unique_lock lock(m_state->mutex);
        return m_state->finished.wait_for(lock, timeout, [&] { return m_state->result.has_value(); });
    }

private:
    std::shared_ptr<detail::JobState<T>> m_state;
};

} // namespace wabcut::media
