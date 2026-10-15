#pragma once

// FrameSource (private): decoded frames for one clip cursor, as the Scheduler sees them.
//
// Two implementations:
//   * PoolFrameSource (default): requests frames from the shared media::DecoderPool, one request in flight per
//     clip cursor so the pool's sequential reuse keeps a single session decoding forward; frames land in a small
//     cache the scheduler reads.
//   * DecodeSession: a private decoder on its own worker thread (the M4 path), kept as a fallback
//     (SchedulerOptions::decodeBackend).
// Both keep a bounded frame cache, drop work the playhead passed and serve reverse play from bounded forward
// windows. Frames are released on decoder/scheduler threads, never on the render thread.

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wabcut::playback::detail {

class FrameSource {
public:
    virtual ~FrameSource() = default;

    /// Queues `sourceTime` unless it is cached, queued or known to fail. Requests of older epochs are dropped.
    /// `urgent`: the frame the next composite waits for. Returns true when a new request was queued.
    virtual bool request(Ticks sourceTime, std::uint64_t epoch, bool reverse, bool urgent) = 0;
    /// Drops queued requests of epochs before `epoch`.
    virtual void beginEpoch(std::uint64_t epoch) = 0;
    /// Drops queued requests the playhead has passed: before `frontier` (forward) or after it (reverse).
    virtual void dropPassed(Ticks frontier, bool reverse) = 0;

    enum class Lookup { Ready, Pending, Failed };
    /// The cached frame containing `sourceTime`.
    virtual Lookup lookup(Ticks sourceTime, media::FrameHandle& out) const = 0;

    virtual const std::string& path() const = 0;
    virtual int stream() const = 0;
    /// "d3d11va", "cuda", "software", "pool/hardware", ... ("" until known).
    virtual std::string backend() const = 0;
    virtual std::uint64_t decodedFrames() const = 0;
    virtual std::uint64_t errors() const = 0;
    /// Decode latency samples (ns) since the last call.
    virtual void takeSamples(std::vector<std::int64_t>& out) = 0;

    /// Scheduler-thread bookkeeping.
    std::int64_t lastUsedNs = 0;
    /// The source-time distance between two frames the scheduler shows from this source while playing (one target
    /// step at the clip's speed; zero = not known yet). PoolFrameSource steps its reverse windows by it (never by less
    /// than the stream's frame period), so a 240 fps source in a 60 fps sequence decodes every frame of a window but
    /// delivers and caches only those that will be shown. DecodeSession keeps the stream's frame period.
    Ticks targetStep{};
    /// What this source decodes (PreviewSettings, resolveSource): a proxy, standing in for an offline original,
    /// and the resolution divisor frames are brought down to (relative to the original).
    bool proxy = false;
    bool originalOffline = false;
    int targetDivisor = 1;
};

} // namespace wabcut::playback::detail
