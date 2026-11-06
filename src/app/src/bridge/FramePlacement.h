#pragma once

// FramePlacement: where a clip's stored frame sits on the program canvas, the one place the editing tools that draw over
// the program monitor (masks, redactions, object masks, Track Motion) get it from. It is what playback does
// (playback/video/Evaluate.cpp): the stream's display orientation (quarter turns, mirror), the clip's Motion, Scale to
// Frame Size with the sequence's pixel aspect, then render::layerToCanvas with a canvas that carries the sequence's pixel
// aspect (applied after the turn). A tool with its own copy of this drifted twice (mirror, pixel aspect), so there is one.

#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/render/PreparedComposite.h>

#include <optional>
#include <utility>

namespace wabcut::app {

struct FramePlacement {
    render::Affine2D frameToCanvas; ///< stored frame pixels -> canvas pixels
    render::Affine2D canvasToFrame; ///< and back
    double frameWidth = 1.0;        ///< stored pixels
    double frameHeight = 1.0;
    double canvasWidth = 1.0; ///< sequence pixels
    double canvasHeight = 1.0;
    double aspect = 16.0 / 9.0; ///< the displayed shape of the unturned frame (stored size times its pixel aspect)
    Ticks curveTime;

    /// A frame point in fractions of the frame -> a canvas point in fractions of the canvas.
    std::pair<double, double> toCanvasFraction(double u, double v) const {
        const render::Vec2 p = frameToCanvas.map({u * frameWidth, v * frameHeight});
        return {p.x / canvasWidth, p.y / canvasHeight};
    }
    /// A canvas point in fractions of the canvas -> a frame point in fractions of the frame.
    std::pair<double, double> toFrameFraction(double x, double y) const {
        const render::Vec2 p = canvasToFrame.map({x * canvasWidth, y * canvasHeight});
        return {p.x / frameWidth, p.y / frameHeight};
    }
};

/// `clip` of `sequence` showing the picture of `frame` (its media stream, or a nested sequence's canvas) at `curveTime` (the
/// clip's curve time, the time its Motion is read at). nullopt when the size is unknown or the placement cannot be inverted.
std::optional<FramePlacement> framePlacement(const ProjectState& project, const Sequence& sequence, const Clip& clip,
                                             const VideoStreamInfo& frame, Ticks curveTime);

/// The same for a footage clip: the stream its media has at the clip's stream index. nullopt for no such stream.
std::optional<FramePlacement> framePlacementOf(const ProjectState& project, const Sequence& sequence, const Clip& clip,
                                               Ticks curveTime);

} // namespace wabcut::app
