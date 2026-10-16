#pragma once

// Internal: the hand-off of one track's PCM from the feeder thread (producer) to the audio callback
// (consumer). Audio travels in an SpscFrameRing; a parallel SpscQueue carries one header per written chunk
// saying which timeline time its first frame is and which flush epoch it belongs to.

#include <wabcut/audio/SeqLock.h>
#include <wabcut/audio/SpscQueue.h>
#include <wabcut/audio/SpscRingBuffer.h>

#include <wabcut/model/Time.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace wabcut::playback::detail {

/// Consumer-side counters, accumulated by the audio thread.
struct StreamCounters {
    std::uint64_t underrunFrames = 0; ///< requested frames with no data queued at all
    std::uint64_t skippedFrames = 0;  ///< stale frames dropped (behind the transport)
    std::uint64_t waitFrames = 0;     ///< silence while queued data starts later (after an anchor)
};

/// TrackStream's chunk header and flush marker. At namespace scope, not nested: SpscQueue and SeqLock check that
/// their element is default constructible inside TrackStream, and GCC does not see a nested class's default member
/// initializers until the enclosing class is complete.
struct TrackStreamChunk {
    std::uint64_t epoch = 0;
    std::int64_t timeline = 0;
    std::uint32_t frames = 0;
    std::uint32_t consumed = 0;
};
struct TrackStreamMarker {
    std::uint64_t epoch = 0;
    std::int64_t cutover = std::numeric_limits<std::int64_t>::min(); ///< TrackStream::kImmediate
};

/// Flush protocol. The producer bumps the epoch and publishes {epoch, cutover}:
///   * cutover = kImmediate (seek, resync): every older chunk is dropped at once;
///   * otherwise (an edit): older chunks keep playing up to the cutover time, then the new epoch's data takes
///     over; it starts exactly at the cutover, so the edit lands without a gap.
/// Chunks of one epoch are contiguous in queue order, so the consumer never looks ahead.
///
/// Wherever the output switches between data and silence, or between non-contiguous data (an epoch switch, a
/// skip), it is de-clicked like the engine does: new audio fades in and the last output decays out under it.
class TrackStream {
public:
    static constexpr std::int64_t kImmediate = std::numeric_limits<std::int64_t>::min();

    /// Allocates everything. `fadeFrames` = de-click length (0 = off).
    TrackStream(std::size_t capacityFrames, int channels, std::size_t maxChunks, int fadeFrames);

    int channels() const noexcept { return m_frames.channels(); }

    // ---- Producer (feeder thread) ---------------------------------------------------------------------
    /// Frames that can be written now (0 when no chunk header is free).
    std::size_t writableFrames() const noexcept;
    /// Appends `interleaved` (whole frames) as one chunk of `epoch` whose first frame is at `timeline`.
    /// Writes nothing and returns false if it does not fit.
    bool write(std::uint64_t epoch, Ticks timeline, std::span<const float> interleaved) noexcept;
    /// Starts a new epoch (see class comment). Epochs must increase.
    void flush(std::uint64_t epoch, std::int64_t cutover) noexcept;

    // ---- Consumer (audio callback, RT-safe) ----------------------------------------------------------------
    /// Fills `out` (frames = out.size() / channels) with this track's audio for timeline times
    /// start + k·ticksPerFrame. Returns false if the transport is somewhere the queued data cannot reach (the
    /// data starts more than `maxWaitFrames` later): the caller asks the feeder to resync there.
    bool read(Ticks start, std::span<float> out, std::int64_t ticksPerFrame, std::int64_t maxWaitFrames,
              StreamCounters& counters) noexcept;

private:
    using Chunk = TrackStreamChunk;
    using Marker = TrackStreamMarker;
    static_assert(Marker{}.cutover == kImmediate);

    void dropRest(Chunk& chunk) noexcept;
    /// De-click one run of output frames that came from data (`data`) or is silence.
    void shape(float* run, std::int64_t frames, bool data, bool discontinuous) noexcept;

    audio::SpscFrameRing m_frames;
    audio::SpscQueue<Chunk> m_chunks;
    audio::SeqLock<Marker> m_marker;

    // Consumer state.
    std::uint64_t m_epoch = 0;
    std::int64_t m_cutover = kImmediate;
    bool m_switching = false; ///< an epoch change is being applied (old chunks may still be queued)
    bool m_discontinuity = false;
    const int m_fadeFrames;
    bool m_prevData = false;
    int m_inPos = -1;
    int m_tailPos = -1;
    std::unique_ptr<float[]> m_lastOut;
    std::unique_ptr<float[]> m_tailFrom;
};

} // namespace wabcut::playback::detail
