#pragma once

// PoolFrameSource (private): one clip cursor's frames from the shared media::DecoderPool.
//
// The pool serves a request with an idle session whose cursor sits just before the requested time (sequential
// reuse); a request it cannot place that way opens another session, which seeks. So this source keeps exactly one
// request in flight: the next wanted time is submitted from the completion callback of the previous one, and the
// clip's frames come out of one session decoding forward. Wanted times are kept in presentation order; reverse play
// queues a bounded forward window [t - window, t] and serves it backwards. The window's times step by what the
// scheduler shows (FrameSource::targetStep), at least the stream's frame period: a high-frame-rate source in a
// slower sequence decodes every frame of the window but delivers only the ones that will be shown.
//
// Threading: request/beginEpoch/dropPassed/lookup are called by the scheduler thread; completions arrive on pool
// worker threads. Shared state lives in a ref-counted block the callbacks hold, so a source can be destroyed while a
// request is still decoding (that request is cancelled and its frame dropped on the worker thread).

#include "FrameSource.h"

#include <wabcut/media/DecoderPool.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace wabcut::playback::detail {

/// Counts pool requests whose completion callback has not finished. The scheduler waits for it to drain before it
/// releases its pool reference, so the last reference is never dropped on a pool worker thread (which would join
/// itself).
class PoolCallbackGate {
public:
    void enter();
    void exit();
    /// True when no callback is pending or running.
    bool waitIdle(std::chrono::milliseconds timeout);

private:
    std::mutex m_mutex;
    std::condition_variable m_idle;
    int m_active = 0;
};

struct PoolFrameSourceOptions {
    int cacheFrames = 16;
    int reverseWindowFrames = 24;
    /// The stream's nominal frame period (from MediaInfo): the smallest step of a reverse window.
    Ticks frameDuration{kTicksPerSecond / 30};
    /// Pool epoch namespace of the owning scheduler.
    std::uint32_t client = 0;
    bool collectTimings = false;
    /// Reduced-resolution preview: frames are box-downscaled to about streamHeight / targetDivisor (Downscale.h).
    int streamHeight = 0;
    int targetDivisor = 1;
};

class PoolFrameSource final : public FrameSource {
public:
    /// Holds `pool` weakly: the owner (the scheduler) keeps it alive and drains `gate` before releasing it.
    PoolFrameSource(std::shared_ptr<media::DecoderPool> pool, std::shared_ptr<PoolCallbackGate> gate, std::string path,
                    int containerStream, PoolFrameSourceOptions options, std::function<void()> onReady);
    ~PoolFrameSource() override;
    PoolFrameSource(const PoolFrameSource&) = delete;
    PoolFrameSource& operator=(const PoolFrameSource&) = delete;

    bool request(Ticks sourceTime, std::uint64_t epoch, bool reverse, bool urgent) override;
    void beginEpoch(std::uint64_t epoch) override;
    void dropPassed(Ticks frontier, bool reverse) override;
    Lookup lookup(Ticks sourceTime, media::FrameHandle& out) const override;
    const std::string& path() const override { return m_path; }
    int stream() const override { return m_stream; }
    std::string backend() const override;
    std::uint64_t decodedFrames() const override;
    std::uint64_t errors() const override;
    void takeSamples(std::vector<std::int64_t>& out) override;

private:
    struct State;

    const std::string m_path;
    const int m_stream;
    std::shared_ptr<State> m_state;
};

} // namespace wabcut::playback::detail
