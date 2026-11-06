#pragma once

// Motion analysis for the flow mixes (P.2, docs/motion.md): Time Interpolation's Optical Flow between two frames of a
// retimed clip, and Morph Cut between the two sides of a jump cut.
//
// A MotionRequest names two pictures of media files - a source time each (the frame containing it is read) and where
// the picture lies on the canvas of the level the mix happens on - and the canvas. The analyzer decodes the ORIGINAL
// files itself on the CPU (whatever the preview decodes: proxies, reduced resolution, decoder surfaces), draws each
// picture's luma on the canvas at the analysis size (the canvas scaled so its longer side is motion::kAnalysisSide),
// and estimates the motion both ways (motion::estimateFlowPair with motion::analysisFlowOptions()). The result depends
// only on the request, so the program monitor, Render In to Out and export show the same pixels.
//
// Results are cached (LRU, bounded bytes) by the request's key. The preview asks without waiting (find, request: the
// scheduler holds the frame until the motion is there, like a frame still decoding); export and review frames compute
// on their own thread (compute). One analyzer per process (shared()), thread-safe.

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>
#include <wabcut/motion/FaceAlign.h>
#include <wabcut/motion/Flow.h>
#include <wabcut/render/PreparedComposite.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::playback {

/// One picture a motion analysis reads.
struct MotionPicture {
    std::string path;      ///< the file (UTF-8): the original, or a proxy only when the original is offline
    int stream = -1;       ///< container stream index (-1: the best video stream)
    Ticks time{};          ///< source time: the frame containing it is analysed
    /// Where the frame lies on the canvas (render::layerToCanvas; the stream's display rotation folded in, as in
    /// LayerSpec::transform), for a frame of the stream's stored size and pixel aspect below.
    render::LayerTransform transform;
    int frameWidth = 0;
    int frameHeight = 0;
    double pixelAspect = 1.0;
    std::uint64_t identity = 0; ///< the file's content fingerprint (a relinked or re-rendered file is new motion)

    bool operator==(const MotionPicture&) const = default;
};

/// 0.7 (face-aware Morph Cut): finds the faces of a picture. The analysis asks it for the two sides of a Morph Cut and, where it
/// finds the same face on both, aligns the flow inside the face by the faces' landmarks (motion/FaceAlign.h). Installed by whoever
/// owns a detector (wabcut/ai/vision: YuNet in the AI helper); none installed, or a detector that finds nothing or fails: the
/// plain flow (the old Morph Cut).
class FaceFinder {
public:
    virtual ~FaceFinder() = default;
    /// The faces in `grey` (the picture the flow is estimated on, 0..1), in its pixels, best first. Called on the analysis worker
    /// threads and on an export's thread; it may take a while (a helper process). Empty: none found, or the detector failed.
    virtual std::vector<motion::FaceFeatures> find(const motion::Plane& grey) = 0;
    /// What the results depend on (the detector's weights and decoding): part of the motion's cache key. Never empty.
    virtual std::string version() const = 0;
};

/// The process's face finder (none by default). Thread-safe.
void setFaceFinder(std::shared_ptr<FaceFinder> finder);
std::shared_ptr<FaceFinder> faceFinder();

struct MotionRequest {
    MotionPicture a;
    MotionPicture b;
    render::CanvasSize canvas{}; ///< the level the mix happens on
    /// 0.7: align the flow inside the faces both sides show (the Morph Cut's Face Aware setting). The result also depends on
    /// whether a face finder is installed and which one (key()).
    bool faceAware = false;

    bool isValid() const;
    /// A 64-bit key of everything above (the cache's).
    std::uint64_t key() const;
    bool operator==(const MotionRequest&) const = default;
};

/// What buildCompositeLayers asks for the motion of a flow mix. Null: not available (the mix then dissolves).
class MotionSource {
public:
    virtual ~MotionSource() = default;
    virtual std::shared_ptr<const motion::FlowPair> motion(const MotionRequest& request) = 0;
};

class MotionAnalyzer {
public:
    struct Stats {
        int queued = 0;
        int running = 0;
        std::uint64_t computed = 0;
        std::uint64_t failed = 0;
        std::uint64_t facesAligned = 0; ///< analyses (0.7) in which at least one face was aligned
        std::size_t cachedEntries = 0;
        std::size_t cachedBytes = 0;
        double lastMilliseconds = 0.0; ///< the last analysis, decode included
    };

    /// `workers` background threads (at least 1); the cache keeps at most `budgetBytes` of motion.
    explicit MotionAnalyzer(int workers = 2, std::size_t budgetBytes = std::size_t{256} << 20);
    ~MotionAnalyzer(); ///< Drops queued work, waits for running analyses.
    MotionAnalyzer(const MotionAnalyzer&) = delete;
    MotionAnalyzer& operator=(const MotionAnalyzer&) = delete;

    /// The process-wide analyzer (created on first use).
    static MotionAnalyzer& shared();

    /// The cached motion of `request`, or null.
    std::shared_ptr<const motion::FlowPair> find(const MotionRequest& request) const;
    /// Queues the analysis unless it is cached, queued or running (`urgent` goes before the others). `done` runs on a
    /// worker thread when it finishes, also when it fails (a picture that does not decode: find() stays null).
    void request(const MotionRequest& request, bool urgent, std::function<void()> done = {});
    /// The motion of `request` now: cached, or analysed on the calling thread (export, review frames). Errors: the
    /// pictures' decode errors, InvalidArgument for an invalid request.
    std::expected<std::shared_ptr<const motion::FlowPair>, Error> compute(const MotionRequest& request);
    /// Whether `request` is queued or running.
    bool pending(const MotionRequest& request) const;
    /// Whether the last analysis of `request` failed (a picture that does not decode): callers then mix without motion
    /// instead of waiting. Forgotten after a while (bounded).
    bool failed(const MotionRequest& request) const;
    /// Drops every queued (not yet running) request.
    void cancelQueued();
    Stats stats() const;
    /// Forgets every cached result (tests).
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// The luma of a CPU frame as a grey plane, 0..1 (8- and 10-bit planar, NV12 / P010, RGBA8). Empty for other
/// payloads.
motion::Plane lumaOf(const media::FrameHandle& frame);

/// `luma` (a frame of `picture`) drawn on the request's canvas at `width` x `height`: each pixel shows the frame point
/// under it (layerToCanvas inverted), 0 outside the frame.
motion::Plane placeOnCanvas(const motion::Plane& luma, const MotionPicture& picture, render::CanvasSize canvas, int width,
                            int height);

/// A MotionSource that analyses on the calling thread through MotionAnalyzer::shared() (export, review frames).
MotionSource& computedMotion();

} // namespace wabcut::playback
