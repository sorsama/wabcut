#pragma once

// DecodeSession (private): one decoder on its own worker thread for one clip cursor, until media's
// DecoderPool lands.
//
// The scheduler queues source times; the worker decodes them in order with VideoDecoder::frameAt()
// (sequential fast path forward, keyframe seeks otherwise) into a bounded cache of decoded frames. Reverse
// requests decode a bounded window forward ([t - window, t]) and keep it, so playing backwards costs one GOP
// walk per window instead of one per frame; when idle the worker prefetches the previous window. A new seek
// epoch drops queued requests (latest seek wins); decoded frames stay cached because they are valid for any
// epoch. Frames are released on this thread or the scheduler's, never on the render thread.

#include "FrameSource.h"

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/Time.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wabcut::playback::detail {

struct DecodeSessionOptions {
    media::DecoderOptions decoder{};
    int cacheFrames = 16;
    int reverseWindowFrames = 24;
    bool collectTimings = false;
    /// Reduced-resolution preview: frames are box-downscaled to about streamHeight / targetDivisor (Downscale.h).
    int streamHeight = 0;
    int targetDivisor = 1;
};

class DecodeSession final : public FrameSource {
public:
    /// Opens the decoder lazily on the worker thread. `onReady` is called (on the worker) after each result.
    DecodeSession(std::string path, int containerStream, DecodeSessionOptions options,
                  std::function<void()> onReady);
    ~DecodeSession() override;
    DecodeSession(const DecodeSession&) = delete;
    DecodeSession& operator=(const DecodeSession&) = delete;

    /// Queues `sourceTime` unless it is cached, queued or known to fail. Requests of older epochs are
    /// dropped. Returns true when a new request was queued.
    bool request(Ticks sourceTime, std::uint64_t epoch, bool reverse, bool urgent = false) override;
    /// Drops queued requests of epochs before `epoch`.
    void beginEpoch(std::uint64_t epoch) override;
    /// Drops queued requests the playhead has passed: before `frontier` (forward) or after it (reverse). The
    /// scheduler abandoned those targets, so decoding them would only delay the ones still wanted.
    void dropPassed(Ticks frontier, bool reverse) override;

    /// The cached frame containing `sourceTime`.
    Lookup lookup(Ticks sourceTime, media::FrameHandle& out) const override;

    const std::string& path() const override { return m_path; }
    int stream() const override { return m_stream; }

    /// "d3d11va", "software", ... ("" until opened).
    std::string backend() const override;
    bool openFailed() const { return m_openFailed.load(); }
    std::uint64_t decodedFrames() const override { return m_decoded.load(); }
    std::uint64_t errors() const override { return m_errors.load(); }
    /// Decode latency samples (ns) since the last call.
    void takeSamples(std::vector<std::int64_t>& out) override;


private:
    struct Request {
        Ticks time;
        std::uint64_t epoch = 0;
        bool reverse = false;
    };
    struct Entry {
        media::VideoFrame frame;
        Ticks requested{};
        bool failed = false;
        bool used = false;
    };

    void run();
    bool cachedLocked(Ticks t) const;
    void storeLocked(Entry entry);
    void decodeOne(const Request& r);
    void decodeWindow(Ticks first, Ticks last);
    std::optional<media::VideoFrame> timedFrameAt(Ticks t, std::optional<Error>& error);

    const std::string m_path;
    const int m_stream;
    const DecodeSessionOptions m_options;
    std::function<void()> m_onReady;

    std::unique_ptr<media::VideoDecoder> m_decoder; // worker thread only
    Ticks m_frameDuration{kTicksPerSecond / 30};    // worker thread only
    Ticks m_window{kTicksPerSecond * 23 / 30};      // reverse window span (reverseWindowFrames - 1 frames)
    Ticks m_firstFrame{};                           // worker thread only

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<Request> m_queue; // FIFO, capacity reserved (no allocation per request)
    std::vector<Entry> m_cache;   // ring
    std::size_t m_cacheNext = 0;
    std::uint64_t m_minEpoch = 0;
    std::optional<Ticks> m_inFlight; // the request being decoded (counts as queued)
    bool m_lastReverse = false;
    bool m_stop = false;
    std::string m_backend;
    std::vector<std::int64_t> m_samples;

    std::atomic<bool> m_openFailed{false};
    std::atomic<std::uint64_t> m_decoded{0};
    std::atomic<std::uint64_t> m_errors{0};
    std::thread m_thread;
};

} // namespace wabcut::playback::detail
