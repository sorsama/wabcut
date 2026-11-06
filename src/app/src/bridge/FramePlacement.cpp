#include "FramePlacement.h"

#include <wabcut/model/FrameScaling.h>

#include <cmath>

namespace wabcut::app {

std::optional<FramePlacement> framePlacement(const ProjectState& project, const Sequence& seq, const Clip& clip,
                                             const VideoStreamInfo& v, Ticks t) {
    if (v.width <= 0 || v.height <= 0 || seq.width <= 0 || seq.height <= 0) {
        return std::nullopt;
    }
    const ClipProperties& p = clip.properties();
    render::LayerTransform transform;
    transform.position = {p.value(ParamId::PositionX, t), p.value(ParamId::PositionY, t)};
    transform.setScaleXY(p.scaleAt(t), scaleToFrameFactor(project, clip, seq.width, seq.height, sequencePixelAspect(seq))); // P.2
    transform.rotationDegrees = p.value(ParamId::Rotation, t);
    transform.anchor = {p.value(ParamId::AnchorX, t), p.value(ParamId::AnchorY, t)};
    // As playback places it (Evaluate.cpp): the stream's display orientation (rotation, mirror) before the clip's own motion.
    render::applyDisplayOrientation(transform, v.rotationDegrees / 90, v.mirrored);
    const double par = v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0;
    FramePlacement out;
    out.frameToCanvas = render::layerToCanvas(transform, v.width, v.height, par, {seq.width, seq.height, sequencePixelAspect(seq)});
    if (std::abs(out.frameToCanvas.determinant()) < 1e-12) {
        return std::nullopt;
    }
    out.canvasToFrame = out.frameToCanvas.inverted();
    out.frameWidth = v.width;
    out.frameHeight = v.height;
    out.canvasWidth = seq.width;
    out.canvasHeight = seq.height;
    out.aspect = v.width * par / v.height;
    out.curveTime = t;
    return out;
}

std::optional<FramePlacement> framePlacementOf(const ProjectState& project, const Sequence& seq, const Clip& clip, Ticks t) {
    const MediaItem* item = project.findMedia(clip.media);
    if (!item || clip.stream.index < 0 || clip.stream.index >= static_cast<int>(item->info.video.size())) {
        return std::nullopt;
    }
    return framePlacement(project, seq, clip, item->info.video[static_cast<std::size_t>(clip.stream.index)], t);
}

} // namespace wabcut::app
