#pragma once

// Scheduler: turns the edit into PreparedComposites ahead of the playhead (contract rev 2, "playback";
// Threads table).
//
// Edits: when a pass acquires a new snapshot it computes the ChangeSet from the previous one
// (diffSequence/diff, which skip every node the two revisions share, so they cost what changed) and
// invalidates only what the change can affect: the held still and the composites already published ahead are
// re-evaluated when a changed range of a video track (or a video track's header, the track list, the sequence
// settings or a relinked media) touches them. Audio-only edits and edits elsewhere on the timeline leave the
// picture and the lookahead alone.
//
// On its own thread, each pass acquires the project snapshot ONCE, reads the transport clock and evaluates
// the sequence at the next target times (the sequence frame grid; every |speed|-th frame when shuttling),
// topmost enabled track first with occlusion culling. For each visible clip it computes the source time
// (sourceIn + offset) and asks that clip's decoder session for the frame. Sessions run on worker threads (one
// per active clip cursor until media's DecoderPool lands) and keep a small cache of decoded frames; reverse
// play decodes bounded forward windows and serves them backwards. When every layer of the next target is
// ready, a pooled composite is filled and published to the mailbox in presentation order, tagged with the
// current seek epoch (latest seek wins). Targets whose time has passed are abandoned (reduced cadence)
// instead of being shown late.
//
// The render thread never touches a session or the scheduler: it only reads the mailbox.

#include <wabcut/media/DecoderPool.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/playback/video/TransportClock.h>
#include <wabcut/render/ColorPipeline.h>
#include <wabcut/render/CompositeMailbox.h>
#include <wabcut/render/StillImage.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wabcut::playback {

/// Where the scheduler's frames are decoded.
enum class DecodeBackend : std::uint8_t {
    /// media::DecoderPool (shared sessions, priorities, stale-seek cancellation). The default.
    Pool,
    /// A private decoder thread per clip cursor (the M4 path), kept as a fallback.
    Sessions,
};

struct SchedulerOptions {
    /// Composites published ahead of the playhead (bounded by the mailbox capacity).
    int lookaheadFrames = 6;
    /// Soft cap on concurrent decoder sessions; idle ones are closed first.
    int maxSessions = 16;
    media::DecoderOptions decoder{};
    DecodeBackend decodeBackend = DecodeBackend::Pool;
    /// The pool to decode with (DecodeBackend::Pool); null = the scheduler creates its own from `decoder`,
    /// `maxSessions` and `poolWorkerThreads`. Share one pool per process so its caps are global.
    std::shared_ptr<media::DecoderPool> pool;
    /// Worker threads of a pool the scheduler creates; 0 = automatic (half the CPUs, 4..8).
    int poolWorkerThreads = 0;
    /// Zero-copy (docs/zero-copy.md): decode with media::OutputMode::Native so frames stay decoder surfaces on the
    /// device the renderer shared (render::shareDeviceWithDecoders). Only for the scheduler that feeds that
    /// renderer. Frames held at once are capped (lookahead, a shorter reverse window) and the decoders get that
    /// many extra surfaces. Without a shared device decoders fall back to CPU frames by themselves. When the shared
    /// device changes (media::sharedD3D11Device(): a new QRhi, a device reset), the scheduler starts a new epoch
    /// (composites of the old device are dropped, the paused frame is rendered again), closes every session and
    /// its cached frames, and reopens them on the new device; until it is shared they decode to CPU frames.
    bool zeroCopy = false;
    /// Sessions unused for this long are closed.
    int sessionIdleMs = 3000;
    /// Frames per backwards decode window (reverse playback).
    int reverseWindowFrames = 24;
    /// Composites in the pool; 0 = mailbox capacity + lookahead + 8.
    int compositePoolSize = 0;
    /// Display latency: frames are chosen for the instant they become visible.
    std::int64_t displayLatencyNs = 16'666'667;
    /// Skip layers hidden behind an opaque full-canvas layer above them.
    bool cullOccluded = true;
    /// Decode a media item's proxy (MediaLocator::proxy) when it has one: the initial PreviewSettings::useProxies
    /// (Scheduler::setPreview() changes what the preview decodes at run time).
    bool preferProxies = false;
    /// The sequence's colour pipeline (sequence colour settings arrive with a later model revision).
    render::ColorPipeline color{};
    /// Keep per-frame decode latency samples (benchmarks): takeDecodeSamples().
    bool collectTimings = false;
};

struct SchedulerStats {
    std::uint64_t passes = 0;
    std::uint64_t published = 0;
    std::uint64_t skippedLate =
        0; ///< Targets abandoned because their time passed before all layers were ready.
    std::uint64_t decodeRequests = 0;
    std::uint64_t decodedFrames = 0;
    std::uint64_t decodeErrors = 0;
    std::uint64_t poolExhausted = 0; ///< Passes that found no free composite (consumers hold too many).
    std::uint64_t invalidations = 0; ///< Edits that forced the still or the lookahead to be re-evaluated.
    int activeSessions = 0;
    /// Stack effects bypassed (unknown id, newer version) in the latest published composite: the monitor's
    /// warning badge.
    int bypassedEffects = 0;
    /// Decode backends in use, e.g. "d3d11va x3" or "d3d11va x2, software x1" (principle 4).
    std::string decodeBackends;
    /// What the latest published composite was decoded from (the monitor's honest quality badge).
    int proxyLayers = 0;        ///< layers decoded from proxies
    int originalLayers = 0;     ///< layers decoded from originals
    int offlineProxyLayers = 0; ///< proxies standing in for offline originals (included in proxyLayers)
    /// Coarsest decoded resolution among those layers, relative to the originals: 1 = full, 2 = half, 4 = quarter.
    int decodeDivisor = 1;
    /// Targets that missed their slot while waiting for a media item's decoder, by media id (MediaId::value),
    /// since the scheduler started: the "dropped frames in playback" signal of the automatic proxy policy.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> lateFramesByMedia;
    /// Zero-copy: times the renderer's shared device changed (a new QRhi, a device reset) and every session was
    /// reopened on the new one; native frames of an old device that still arrived and were decoded again.
    std::uint64_t deviceChanges = 0;
    std::uint64_t staleDeviceFrames = 0;
    /// The decoder pool's video work (media::DecoderPoolStats, pool-wide when SchedulerOptions::pool is shared): codec
    /// frames decoded and frames delivered. A 240 fps source in a 60 fps sequence decodes about four per frame it
    /// delivers, and only the delivered ones are read back, converted, cached and uploaded.
    std::uint64_t poolFramesDecoded = 0;
    std::uint64_t poolFramesDelivered = 0;
};

/// See Scheduler::setColorPreview().
struct ColorPreview {
    render::ColorView view;
    std::shared_ptr<const render::StillImage> reference; ///< null: no side by side

    bool operator==(const ColorPreview&) const = default;
};

/// The side-by-side comparison: every layer scaled to half size into the right half of the canvas, and `reference`
/// fitted into the left half (both centred vertically).
void placeSideBySide(std::vector<render::CompositeLayer>& layers, render::CanvasSize canvas,
                     const std::shared_ptr<const render::StillImage>& reference);

class Scheduler {
public:
    /// Starts the scheduler thread. `clock` may be null until setClock().
    Scheduler(std::shared_ptr<render::CompositeMailbox> mailbox, std::shared_ptr<TransportClock> clock,
              SchedulerOptions options = {});
    ~Scheduler(); ///< Stops the thread and closes every session.
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// Follow `session`'s current snapshot (acquired once per pass; the session must outlive the scheduler or
    /// the next setSession()/setSnapshot()). Any thread.
    void setSession(const EditSession* session, SequenceId sequence);
    /// Evaluate a fixed snapshot (export, tests, benchmarks).
    void setSnapshot(ProjectSnapshot snapshot, SequenceId sequence);
    void setClock(std::shared_ptr<TransportClock> clock);
    /// A seek or direction change started mailbox epoch `epoch`: queued decode work is dropped and scheduling
    /// restarts from the clock's position.
    void retarget(std::uint64_t epoch);
    /// Called on the scheduler thread when forward playback passes the sequence end (+1) or reverse playback
    /// reaches the start (-1), once per epoch.
    void setBoundaryCallback(std::function<void(int direction)> callback);
    /// A.8: false lets forward playback run past the sequence end without the boundary callback (a voice-over pass
    /// records past the last clip); true (the default) again: a transport already past the end stops on the next
    /// pass. Any thread.
    void setStopAtEnd(bool stop);
    bool stopAtEnd() const;
    /// Schedule a pass now (e.g. after an edit).
    void wake();
    /// What the preview decodes: proxies or originals, the decode resolution and the proxy registry. Takes effect
    /// on the next pass (sessions whose file or resolution changed are reopened; the still is re-rendered). Any
    /// thread.
    void setPreview(PreviewSettings preview);
    PreviewSettings preview() const;
    /// Burned-in captions (the caption track drawn as the top layer, CaptionSettings), or none. Takes effect on the
    /// next pass; the paused picture is rendered again. Any thread.
    void setCaptions(std::optional<CaptionSettings> captions);
    /// The Color workspace's views of the program (preview only; export never has them): the HSL secondary's matte,
    /// the before/after split (ColorView) and a reference still side by side - the reference on the left half, the
    /// program on the right. Takes effect on the next pass; the paused picture is rendered again. Any thread.
    void setColorPreview(ColorPreview preview);

    SchedulerStats stats() const;
    /// Decode latency samples in nanoseconds since the previous call (with options.collectTimings).
    std::vector<std::int64_t> takeDecodeSamples();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::playback
