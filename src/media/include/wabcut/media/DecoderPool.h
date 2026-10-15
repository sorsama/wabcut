#pragma once

// DecoderPool: shared, capped decoder sessions for the playback scheduler and background jobs (architecture
// rev 2).

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/FrameHandle.h>
#include <wabcut/media/Job.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::media {

/// Higher priorities are served first; requests of equal priority are served in submission order.
enum class DecodePriority : std::uint8_t {
    Background = 0, ///< Thumbnails, peaks, proxies, export pre-analysis.
    Prefetch = 1,   ///< Read-ahead the scheduler may still need.
    Audio = 2,      ///< Audio feeding the playback rings.
    Preview = 3,    ///< The frame the monitor is waiting for.
};

struct DecoderPoolConfig {
    int maxVideoSessions =
        6; ///< Open VideoDecoders at most; idle least-recently-used ones are closed to make room.
    int maxAudioSessions = 8; ///< Open AudioDecoders at most.
    int workerThreads = 4;    ///< Threads executing requests (each runs one request at a time).
    /// FFmpeg software decode threads across all open software video sessions. 0 = the CPU count. Each new
    /// software session gets an equal share of what is left (at least 1); hardware sessions do not count.
    int softwareThreadBudget = 0;
    /// A session whose cursor is at most this far before a requested time serves it by decoding forward
    /// instead of seeking (sequential reuse).
    Ticks reuseWindow = Ticks::fromSeconds(2);
    /// Video decoder options for new sessions (threadCount is replaced by the pool's share).
    DecoderOptions decoderOptions{};
};

/// One video frame wanted at `time` (source time) of stream `stream` of `path`.
struct FrameRequest {
    std::string path;
    int stream = -1;
    Ticks time{};
    DecodePriority priority = DecodePriority::Preview;
    std::uint32_t client = 0; ///< Epoch namespace, e.g. one per scheduler.
    std::uint64_t epoch = 0;  ///< Requests older than the client's current epoch are cancelled (stale seeks).
    /// The requester: one frame source (a clip of one scheduler); 0 = none. A request of an owner that jumped (a
    /// scrub, a step back, a loop) seeks the idle session that owner used last instead of opening the file again;
    /// another owner's session is never taken for it while under the session cap, so layers playing at once never
    /// share a decoder (P.1).
    std::uint64_t owner = 0;
};

/// One block of `frames` audio sample frames starting at `time`.
struct AudioRequest {
    std::string path;
    int stream = -1;
    Ticks time{};
    std::size_t frames = 0;
    Rate outputRate{48000, 1};
    ChannelLayout layout = ChannelLayout::stereo();
    DecodePriority priority = DecodePriority::Audio;
    std::uint32_t client = 0;
    std::uint64_t epoch = 0;
};

struct AudioBlock {
    std::vector<float> interleaved; ///< frames * channels samples (silence where the result says so).
    AudioReadResult result;
};

struct DecoderPoolStats {
    std::uint64_t requests = 0;
    std::uint64_t completed = 0; ///< Delivered a frame/block.
    std::uint64_t failed = 0;    ///< Completed with a decoder error.
    std::uint64_t cancelled = 0; ///< Stale epoch, Job::cancel() or pool shutdown.
    std::uint64_t sessionsCreated = 0;
    std::uint64_t sessionsClosed = 0;   ///< Closed to stay within the caps.
    std::uint64_t sequentialReuses = 0; ///< Served by a session whose cursor was just before the request.
    std::uint64_t seekReuses = 0;       ///< Served by the requester's own idle session after a jump (it seeked).
    std::size_t videoSessions = 0;      ///< Open now.
    std::size_t audioSessions = 0;
    std::size_t peakVideoSessions = 0;
    std::size_t peakAudioSessions = 0;
    std::size_t queued = 0;
    int softwareThreadsInUse = 0; ///< FFmpeg threads of open software video sessions.
    int peakSoftwareThreads = 0;
    int softwareThreadBudget = 0;
    /// What the video sessions' decoders did (VideoDecoderStats, summed over every session, closed ones included).
    /// More decoded than delivered = frames decoded only to step over them: a seek's walk from the keyframe, or a
    /// 240 fps source played in a 60 fps sequence (about four decoded per frame delivered). Read-backs and
    /// conversions happen for delivered frames only (QSV aside, see VideoDecoderStats::framesReadBack).
    std::uint64_t videoFramesDecoded = 0;
    std::uint64_t videoFramesDelivered = 0;
    std::uint64_t videoFramesReadBack = 0;
    std::uint64_t videoFramesConverted = 0;
};

/// A pool of decoder sessions keyed by (source, stream, cursor), executing frame and audio requests on its
/// own worker threads by priority.
///
/// - Sessions: a request is served, in this order of preference, by (1) an idle session of the same source
/// and
///   stream whose cursor (where it stopped decoding) is at most reuseWindow before the requested time, so
///   sequential playback never seeks; (2) a new session, while under the cap; (3) an idle session of the same
///   source, which seeks; (4) a new session replacing the least recently used idle one. If every session is
///   busy at the cap, the request waits.
/// - Each session is used by one request at a time (decoders are single-thread-affine); different sessions
/// decode
///   in parallel. Hardware sessions share the process-wide GPU device contexts.
/// - Stale-seek cancellation: setEpoch(client, e) completes every queued request of that client with an older
///   epoch as Cancelled at once; a request already decoding finishes but is reported Cancelled.
/// - Job::cancel() on a queued request removes it; on a running one it is reported Cancelled when it
/// finishes.
///
/// Threading: all member functions are thread-safe. Completion callbacks run on worker threads. The
/// destructor cancels everything queued, waits for running requests and closes all sessions.
class DecoderPool {
public:
    using FrameCallback = std::function<void(const std::expected<FrameHandle, Error>&)>;
    using AudioCallback = std::function<void(const std::expected<AudioBlock, Error>&)>;

    explicit DecoderPool(DecoderPoolConfig config = {});
    ~DecoderPool();
    DecoderPool(const DecoderPool&) = delete;
    DecoderPool& operator=(const DecoderPool&) = delete;

    Job<FrameHandle> requestFrame(FrameRequest request, FrameCallback onDone = {});
    Job<AudioBlock> requestAudio(AudioRequest request, AudioCallback onDone = {});

    /// Starts a new epoch for `client`: queued requests of that client with an older epoch are cancelled.
    void setEpoch(std::uint32_t client, std::uint64_t epoch);
    std::uint64_t epoch(std::uint32_t client) const;

    /// Holds back new work (running requests finish); resume() continues in priority order.
    void pause();
    void resume();

    /// Closes every idle session (e.g. before a relink or to release GPU memory).
    void closeIdleSessions();

    DecoderPoolStats stats() const;
    const DecoderPoolConfig& config() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace wabcut::media
