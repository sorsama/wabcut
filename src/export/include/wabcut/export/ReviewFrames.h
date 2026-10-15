#pragma once

// Review package thumbnails (C.9, docs/internal/c9-history-review-handoff.md "The frame source"): command::ReviewFrameSource
// rendered by the export path. The sequence at a note's time is evaluated by the playback evaluator, every visible
// layer decoded from its original (never a proxy), composited by render::OffscreenRenderer with the export output
// transform - the same pixels an export shows, redactions included, nested and multicam clips too - scaled to at most
// the requested width at the sequence's aspect, and encoded as JPEG (PNG when no JPEG writer is available).
//
// Fail closed: a frame that cannot be rendered as an export would show it (an offline layer, a decode error, a
// compositor error) is an error, so review.exportPackage leaves that note without a thumbnail and warns; it never
// falls back to a picture without its redactions.
//
// Threading: frame() runs on the command engine's thread, synchronously. The renderer (a headless QRhi) is created on
// first use on that thread and reused; calls are serialised. Needs a QGuiApplication in the process, like an export.

#include <wabcut/command/ReviewPackage.h>
#include <wabcut/render/ColorPipeline.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <QImage>

#include <memory>
#include <mutex>

namespace wabcut::exporter {

struct ReviewFrameOptions {
    /// Decode with hardware decoders when they work (off: deterministic software decoding, as exports).
    bool hardwareDecode = false;
    /// JPEG quality 1..100.
    int jpegQuality = 85;
    render::RhiBackendRequest backend = render::RhiBackendRequest::PlatformDefault;
    render::ColorPipeline color{};
};

class RenderedReviewFrames final : public command::ReviewFrameSource {
public:
    explicit RenderedReviewFrames(ReviewFrameOptions options = {});
    ~RenderedReviewFrames() override;
    RenderedReviewFrames(const RenderedReviewFrames&) = delete;
    RenderedReviewFrames& operator=(const RenderedReviewFrames&) = delete;

    /// Errors: NotFound (no such sequence, offline media), DeviceError (no renderer), DecoderError / IoError.
    std::expected<command::ReviewFrame, Error> frame(const ProjectState& state, SequenceId sequence, Ticks time,
                                                     int width) const override;
    /// The picture frame() encodes (RGBA8888, at most `width` wide at the sequence's aspect). Same errors.
    std::expected<QImage, Error> picture(const ProjectState& state, SequenceId sequence, Ticks time, int width) const;

private:
    ReviewFrameOptions m_options;
    mutable std::mutex m_mutex;
    mutable std::unique_ptr<render::OffscreenRenderer> m_renderer;
};

} // namespace wabcut::exporter
