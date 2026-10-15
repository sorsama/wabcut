#pragma once

// IVideoFrameSource: where an export's video frames come from.
//
// The production source (Phase 2) evaluates the fixed snapshot at each output time with the playback
// Scheduler's evaluator and renders it with render::AsyncOffscreenRenderer (the same Compositor and
// ColorPipeline as the program monitor, Principle 5), reading frames back asynchronously behind a bounded
// queue. Tests and benchmarks use SyntheticVideoSource. The export pipeline only sees this interface.

#include <wabcut/export/ExportSettings.h>
#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace wabcut::exporter {

/// What the export asks a source for. Output frame k shows sequence time start + k periods of `frameRate`.
struct VideoSourceFormat {
    int width = 0; ///< render size (the export crops/pads it afterwards, see ScaleMode)
    int height = 0;
    Rate frameRate{};
    Ticks start{};
    std::int64_t frameCount = 0;
    /// Captions to burn into the picture (CaptionExport::BurnIn), drawn as the preview draws them.
    std::optional<CaptionExportSettings> burnInCaptions;
};

/// A pipelined, in-order frame producer.
///
/// Protocol (one export thread): request(0), request(1), … at most maxInFlight() ahead of next(); next()
/// returns the frames in request order and blocks until the oldest one is ready. That is the backpressure:
/// memory stays bounded by maxInFlight() frames. stop() drops outstanding work (cancel, error), is idempotent
/// and may be called from another thread: a next() blocked in the export thread then returns Cancelled.
///
/// Frames carry a CPU payload (media::CpuPlanes) at the format size, RGBA8 from the renderer
/// (display-referred, BT.709/sRGB as the export output transform defines it) or any media::PixelFormat.
class IVideoFrameSource {
public:
    virtual ~IVideoFrameSource() = default;

    /// For logs and the result, e.g. "OffscreenRenderer D3D11 · NVIDIA GeForce RTX 3060" or "synthetic".
    virtual std::string description() const = 0;
    virtual int maxInFlight() const = 0;
    virtual std::expected<void, Error> request(std::int64_t frameIndex) = 0;
    virtual std::expected<media::FrameHandle, Error> next() = 0;
    virtual void stop() = 0;
};

/// Deterministic test pattern with three layers, rendered on its own thread (like the real renderer):
///   1. a background gradient that drifts with the frame index;
///   2. an opaque box moving left to right;
///   3. a 50 % translucent box moving top to bottom, blended over the others;
/// plus the frame number as a 16-bit barcode in the top-left corner (cells of 32x32 px, bit b at x = 32b,
/// white = 1), the same code the media test clips carry, so decoded exports can be checked frame by frame.
/// The number drawn for output frame k is the sequence frame number, round(start / frame period) + k.
struct SyntheticPattern {
    bool barcode = true;
    int maxInFlight = 3;
};

std::unique_ptr<IVideoFrameSource> makeSyntheticVideoSource(const VideoSourceFormat& format,
                                                            SyntheticPattern pattern = {});

/// Draws frame `frameIndex` of the synthetic pattern into an RGBA8 buffer (tightly packed, width * 4 per
/// row). The source uses exactly this, so tests can rebuild the reference frames.
void drawSyntheticFrame(std::uint8_t* rgba, int width, int height, std::int64_t frameIndex,
                        bool barcode = true);

} // namespace wabcut::exporter
