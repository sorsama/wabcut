#pragma once

// Reference Motion Blur (0.7, model/MotionBlur.h): the test oracle of the compositor's blurred layers. A layer with
// CompositeLayer::blur is drawn once per instant of the camera shutter and the draws are averaged; this is that average in
// double precision on a linear premultiplied picture (Image.h):
//
//   canvas = (1 / n) * sum over the n placements of  draw(picture, placement)
//
//   draw(picture, p)   a transparent black width x height canvas on which the picture sits with the affine placement p
//                      (picture pixel (x, y) lands on canvas pixel (a x + c y + e, b x + d y + f), y down - render::Affine2D's
//                      convention, so layerToCanvas() of the compositor gives the placements). A canvas pixel's value is the
//                      mean of kSubSamples x kSubSamples points inside it, each read from the picture texel under it (nothing
//                      where the point lies outside the picture's rectangle): the picture's own box-filtered area, which is
//                      what the compositor's bilinear edge ramp makes of an axis-aligned edge. Exact for a picture drawn at
//                      its own scale (one picture pixel per canvas pixel) or a flat colour; a picture that is magnified or
//                      shrunk is read by nearest texel here and bilinearly by the GPU, so compare those with a tolerance.
//
// Pure C++ (no Qt, no GPU), like the rest of this library.

#include <wabcut/render/reference/Image.h>

#include <span>

namespace wabcut::render::reference {

/// A picture pixel -> canvas pixel placement: x' = a x + c y + e, y' = b x + d y + f.
struct Placement2D {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;
};

/// Sub-samples per pixel side of draw().
inline constexpr int kSubSamples = 8;

/// The picture on a transparent width x height canvas with one placement (no blur). A singular placement draws nothing.
Image drawPlaced(const Image& picture, int width, int height, const Placement2D& placement);

/// The average of drawPlaced() over `placements` (equal weights); no placements: a transparent canvas.
Image motionBlurred(const Image& picture, int width, int height, std::span<const Placement2D> placements);

} // namespace wabcut::render::reference
