#pragma once

// Internal: everything the audio thread touches. One RenderCore per AudioEngine. The device callback and
// AudioEngine::pull() both run process(), so tests on the Null backend exercise exactly the device code path.

#include <wabcut/audio/AudioSource.h>
#include <wabcut/audio/ClockSample.h>
#include <wabcut/audio/RealTime.h>
#include <wabcut/audio/SeqLock.h>
#include <wabcut/audio/TransportMap.h>

#include <wabcut/model/Time.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace wabcut::audio::detail {

/// Transport state requested by the control side (written under RenderCore::m_mutex, read by the audio thread
/// with SeqLock::tryLoad and by clock readers). The audio thread applies differences at a block boundary.
struct TransportRequest {
    std::uint64_t seekGeneration = 0; ///< Incremented by every seek.
    std::int64_t seekTarget = 0;      ///< Ticks of the latest seek.
    std::int64_t seekPreroll = 0;     ///< Ticks of preroll requested with it.
    std::uint64_t playing = 0;
    std::int32_t speedNum = 1;
    std::int32_t speedDen = 1;
    std::uint64_t loopGeneration = 0;
    std::int64_t loopStart = 0;
    std::int64_t loopEnd = 0; ///< Loop active iff loopEnd > loopStart.
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // "structure was padded due to alignment specifier": the padding is the point.
#endif

/// What the audio thread publishes once per block for readers.
struct ClockSnapshot {
    ClockSample clock;    ///< The device clock (DLL-filtered).
    TransportMap map;     ///< Timeline mapping, with `clock` installed.
    std::int64_t capFrame = 0; ///< Frames rendered so far: nothing beyond can have been heard.
    std::uint64_t appliedSeekGeneration = 0;
    std::uint32_t live = 0;
};

class RenderCore {
public:
    RenderCore() = default;
    RenderCore(const RenderCore&) = delete;
    RenderCore& operator=(const RenderCore&) = delete;

    // ---- Control side: thread-safe, not RT-safe ------------------------------------------------------------

    /// Set the stream format. Only while idle (not live).
    /// `declickFrames`: length of the fades that remove clicks at transport discontinuities (0 = off).
    void configure(int channels, int sampleRate, int maxBlockFrames, int declickFrames);
    int channels() const noexcept { return m_channels; }
    int sampleRate() const noexcept { return m_sampleRate; }

    /// Enter the live state: device frames restart at 0 (the transport position carries over) and the clock
    /// model resets. Call before the device (or the first pull) can run process().
    void goLive();
    /// Leave the live state after the device is closed (process() can no longer run): freeze the transport at
    /// its position at `nowNs` and release every retired source.
    void goIdle(std::int64_t nowNs);
    bool isLive() const noexcept { return m_live.load(std::memory_order_acquire); }

    void setSource(std::shared_ptr<AudioSource> source);
    std::shared_ptr<AudioSource> source() const;
    /// 0.5: the overlay (AudioEngine::setOverlay), rendered every block whatever the transport does and added to the
    /// output; it never touches the transport map. Same lifetime rules as the source.
    void setOverlay(std::shared_ptr<AudioSource> overlay);
    std::shared_ptr<AudioSource> overlay() const;
    /// Release retired sources the audio thread can no longer be using (also done by every control call).
    void collectRetired();

    /// Returns the seek generation.
    std::uint64_t seek(Ticks target, Ticks preroll);
    std::uint64_t appliedSeekGeneration() const noexcept { return m_snapshot.load().appliedSeekGeneration; }
    void setPlaying(bool playing);
    bool playing() const;
    void setSpeed(TransportSpeed speed);
    TransportSpeed speed() const;
    void setLoop(std::optional<TimeRange> range);
    std::optional<TimeRange> loop() const;

    /// Device-side latency (buffer + stream) and the user offset, in frames; read by the audio thread at
    /// every block.
    void setDeviceLatencyFrames(std::int64_t frames) noexcept {
        m_deviceLatencyFrames.store(frames, std::memory_order_relaxed);
    }
    void setDeviceLatencyQuality(LatencyQuality quality) noexcept {
        m_deviceLatencyQuality.store(quality, std::memory_order_relaxed);
    }
    void setOffsetFrames(std::int64_t frames) noexcept { m_offsetFrames.store(frames, std::memory_order_relaxed); }
    std::int64_t offsetFrames() const noexcept { return m_offsetFrames.load(std::memory_order_relaxed); }
    /// Ask the audio thread to re-anchor the clock model at its next block (the stream was restarted).
    void requestModelReset() noexcept { m_modelResetRequested.store(true, std::memory_order_release); }
    /// A new device epoch: reopen, switch, fallback or OS reroute. RT-safe (callable from notifications); the
    /// audio thread rebases the transport and re-anchors the clock at its next block.
    void bumpDeviceEpoch() noexcept { m_deviceEpoch.fetch_add(1, std::memory_order_acq_rel); }
    std::uint32_t requestedDeviceEpoch() const noexcept { return m_deviceEpoch.load(std::memory_order_acquire); }

    // ---- Audio thread: RT-safe -----------------------------------------------------------------------------

    /// Render `frames` frames into `out` (channels() interleaved) as of steady time `nowNs` and advance the
    /// transport and the clock. Only one thread at a time (the device callback or pull()).
    void process(float* out, std::uint32_t frames, std::int64_t nowNs) noexcept;

    // ---- Readers: any thread, lock-free -------------------------------------------------------------------

    /// Transport time heard at `nowNs`, capped at the rendered audio and monotonic across readers.
    Ticks clockAt(std::int64_t nowNs) const noexcept;
    ClockSample clockSample() const noexcept { return m_snapshot.load().clock; }
    TransportMap transportMap() const noexcept { return m_snapshot.load().map; }

    std::uint64_t callbacks() const noexcept { return m_callbacks.load(std::memory_order_relaxed); }
    std::uint64_t framesRendered() const noexcept { return m_framesRendered.load(std::memory_order_relaxed); }
    std::uint64_t clockResyncs() const noexcept { return m_resyncs.load(std::memory_order_relaxed); }
    std::uint32_t lastCallbackFrames() const noexcept { return m_lastFrames.load(std::memory_order_relaxed); }
    /// Device clock vs steady clock, parts per million (positive: the device runs fast).
    double driftPpm() const noexcept { return m_driftPpm.load(std::memory_order_relaxed); }
    /// Steady time of the latest process() call, for the stall watchdog.
    std::int64_t lastCallbackNs() const noexcept { return m_lastCallbackNs.load(std::memory_order_relaxed); }

private:
    struct Retired {
        std::shared_ptr<AudioSource> source;
        std::uint64_t blockCounter; ///< m_blockCounter right after the swap.
    };

    // Control side helpers (m_mutex held).
    void collectRetiredLocked();
    void storeRequestLocked();

    // Shared by the audio thread and, while idle, the control side (which then is the only writer).
    void applyRequest(const TransportRequest& request) noexcept;
    void publish() noexcept;

    // Audio thread helpers.
    void updateModel(double observedFrame, std::int64_t nowNs, std::uint32_t frames,
                     std::int64_t latencyFrames) noexcept;
    void resync(double observedFrame, std::int64_t nowNs) noexcept;
    ClockSample makeClockSample(std::int64_t latencyFrames) const noexcept;

    // Format (written only while idle).
    int m_channels = 2;
    int m_sampleRate = 48000;
    int m_maxBlockFrames = 4096;
    int m_declickFrames = 96;
    std::int64_t m_ticksPerFrame = Rate{48000, 1}.ticksPerUnit();

    // ---- Control-side state, guarded by m_mutex.
    mutable rt::CheckedMutex m_mutex;
    std::shared_ptr<AudioSource> m_source;
    std::vector<Retired> m_retired;
    TransportRequest m_requestValue; ///< Last value stored into m_request.
    std::atomic<bool> m_live{false};

    // ---- Shared with the audio thread.
    SeqLock<TransportRequest> m_request;
    SeqLock<ClockSnapshot> m_snapshot;

    // Source hand-off (see RenderCore.cpp, "Source lifetime"). The overlay is handed over the same way.
    std::atomic<AudioSource*> m_sourcePtr{nullptr};
    std::shared_ptr<AudioSource> m_overlay; ///< m_mutex
    std::atomic<AudioSource*> m_overlayPtr{nullptr};
    /// One block of the overlay before it is added (maxBlockFrames x channels; sized by configure(), while idle).
    std::vector<float> m_overlayScratch;
    alignas(kCacheLineSize) std::atomic<std::uint64_t> m_blockCounter{0};

    std::atomic<std::int64_t> m_deviceLatencyFrames{0};
    std::atomic<LatencyQuality> m_deviceLatencyQuality{LatencyQuality::Estimated};
    std::atomic<std::int64_t> m_offsetFrames{0};
    std::atomic<bool> m_modelResetRequested{false};
    std::atomic<std::uint32_t> m_deviceEpoch{0};

    /// Highest presented device time (ticks) any reader has returned: the monotonic guard.
    alignas(kCacheLineSize) mutable std::atomic<std::int64_t> m_presentedHighWater{0};

    // Statistics (single writer: the audio thread).
    std::atomic<std::uint64_t> m_callbacks{0};
    std::atomic<std::uint64_t> m_framesRendered{0};
    std::atomic<std::uint64_t> m_resyncs{0};
    std::atomic<std::uint32_t> m_lastFrames{0};
    std::atomic<std::int64_t> m_lastCallbackNs{0};
    std::atomic<double> m_driftPpm{0.0};

    // ---- Audio-thread state (touched by the control side only while idle, under m_mutex).
    std::int64_t m_deviceFrame = 0;
    TransportMap m_map{};
    std::uint64_t m_appliedSeekGeneration = 0;
    std::uint64_t m_appliedLoopGeneration = 0;
    std::int64_t m_latencyFrames = 0; ///< Latency used for the latest clock sample.
    // Clock model (delay-locked loop). Floating point is confined to the estimator's internal state; everything
    // it publishes (ClockSample) and all transport arithmetic is integer.
    bool m_modelValid = false;
    double m_baseFrame = 0.0; ///< Frame being heard at m_baseNs.
    std::int64_t m_baseNs = 0;
    double m_framesPerSecond = 48000.0;
    double m_avgBlockFrames = 0.0;
    int m_lateBlocks = 0;
    // De-click state (see declickPiece()).
    static constexpr int kMaxChannels = 32;
    bool m_prevAudible = false;
    AudioSource* m_prevSource = nullptr;
    std::int64_t m_expectedTimeline = 0;
    TransportSpeed m_prevSpeed;
    float m_lastOut[kMaxChannels]{};
    float m_tailFrom[kMaxChannels]{};
    int m_tailPos = -1;   ///< Frames into the decaying tail, -1 when none.
    int m_fadeInPos = -1; ///< Frames into the fade-in, -1 when none.

    void declickPiece(float* chunk, std::uint32_t frames, bool audible, AudioSource* source, Ticks timeline,
                      TransportSpeed speed) noexcept;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace wabcut::audio::detail
