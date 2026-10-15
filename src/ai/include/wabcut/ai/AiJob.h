#pragma once

// AiJob<T>: a cancellable background AI call (docs/ai.md, "Provider contract": calls are jobs with progress, cancel
// and a result). It mirrors media::Job, but carries AiError, and wabcut_ai does not depend on the media layer.
// startAiJob() runs a blocking capability call on an executor supplied by the caller (the app passes a
// QThreadPool, tests run inline), so AI work never runs on the GUI, render or audio threads.

#include <wabcut/ai/AiError.h>

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

namespace wabcut::ai {

namespace detail {

template <typename T> struct AiJobState {
    using Result = std::expected<T, AiError>;

    std::mutex mutex;
    std::condition_variable finished;
    std::optional<Result> result;
    std::stop_source stop;
    std::atomic<float> progress{0.0f};
    std::function<void(const Result&)> onDone;

    void complete(Result r) {
        std::function<void(const Result&)> callback;
        {
            std::lock_guard lock(mutex);
            if (result) {
                return;
            }
            result.emplace(std::move(r));
            progress.store(1.0f, std::memory_order_relaxed);
            callback = std::move(onDone);
        }
        finished.notify_all();
        if (callback) {
            callback(*result); // never modified again once set
        }
    }
};

} // namespace detail

/// Shared handle to a background AI call producing T. Copies refer to the same job. Thread-safe. Dropping every
/// handle does not cancel the job; cancel() does, and the job then completes with AiErrorCode::Cancelled.
template <typename T> class AiJob {
public:
    using Result = std::expected<T, AiError>;

    AiJob() = default;
    explicit AiJob(std::shared_ptr<detail::AiJobState<T>> state) : m_state(std::move(state)) {}

    bool isValid() const { return m_state != nullptr; }
    void cancel() const {
        if (m_state) {
            m_state->stop.request_stop();
        }
    }
    float progress() const { return m_state ? m_state->progress.load(std::memory_order_relaxed) : 0.0f; }

    bool isFinished() const {
        if (!m_state) {
            return false;
        }
        std::lock_guard lock(m_state->mutex);
        return m_state->result.has_value();
    }

    /// Blocks until the job finishes.
    const Result& wait() const {
        std::unique_lock lock(m_state->mutex);
        m_state->finished.wait(lock, [&] { return m_state->result.has_value(); });
        return *m_state->result;
    }

    /// Blocks up to `timeout`; nullptr if still running.
    const Result* waitFor(std::chrono::milliseconds timeout) const {
        std::unique_lock lock(m_state->mutex);
        if (!m_state->finished.wait_for(lock, timeout, [&] { return m_state->result.has_value(); })) {
            return nullptr;
        }
        return &*m_state->result;
    }

    /// Calls `callback` with the result when the job finishes (immediately, on this thread, if it already has);
    /// otherwise on the thread that completes the job. Replaces an earlier callback.
    void onDone(std::function<void(const Result&)> callback) const {
        std::unique_lock lock(m_state->mutex);
        if (m_state->result) {
            lock.unlock();
            callback(*m_state->result);
            return;
        }
        m_state->onDone = std::move(callback);
    }

private:
    std::shared_ptr<detail::AiJobState<T>> m_state;
};

/// Runs a task somewhere else (a thread pool). Must run every task it accepts.
using AiExecutor = std::function<void(std::function<void()>)>;

/// Runs `work(stop, progress)` on `executor` and returns its handle. `work` returns std::expected<T, AiError>; an
/// exception escaping it completes the job with ProviderError. A job cancelled before it starts completes with
/// Cancelled without running.
template <typename T, typename Work> AiJob<T> startAiJob(const AiExecutor& executor, Work work) {
    auto state = std::make_shared<detail::AiJobState<T>>();
    executor([state, work = std::move(work)]() mutable {
        const std::stop_token stop = state->stop.get_token();
        if (stop.stop_requested()) {
            state->complete(std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled before start")));
            return;
        }
        auto progress = [state](float value) { state->progress.store(value, std::memory_order_relaxed); };
        try {
            state->complete(work(stop, progress));
        } catch (const std::exception& e) {
            state->complete(std::unexpected(AiError(AiErrorCode::ProviderError, e.what())));
        } catch (...) {
            state->complete(std::unexpected(AiError(AiErrorCode::ProviderError, "unknown exception")));
        }
    });
    return AiJob<T>(state);
}

} // namespace wabcut::ai
