#pragma once

// Frame interpolation with motion (P.2, docs/motion.md): the picture at a time between two pictures A and B, warped
// along their flows and blended where each side sees the point. This is the CPU reference of the compositor's flow mix
// (shaders/flowmix.frag): change both together.
//
// At time t (0 = A, 1 = B), for an output point x (fractions of the picture), with F = flow A -> B and G = flow
// B -> A sampled at x (the linear-motion approximation of Jiang et al., "Super SloMo", CVPR 2018):
//   x0 = x - (1 - t) t F(x) + t^2 G(x)          where the point was in A
//   x1 = x + (1 - t)^2 F(x) - t (1 - t) G(x)    where it is in B
//   e0 = |F(x0) + G(x0 + F(x0))|, e1 = |G(x1) + F(x1 + G(x1))|   forward-backward disagreement, in flow pixels
//   v = 1 / (1 + e^2)                           how surely the point is visible on that side (occlusion handling)
//   w0 = (1 - t) v0, w1 = t v1
//   out = (w0 A(x0) + w1 B(x1)) / (w0 + w1)     ((1 - t) A(x0) + t B(x1) when both weights vanish)
// Pictures are sampled bilinearly, clamped to the edge; flows bilinearly, clamped to the edge.

#include <wabcut/motion/Flow.h>

#include <array>
#include <cstddef>
#include <vector>

namespace wabcut::motion {

/// An RGBA picture (any four channels: the compositor's linear premultiplied light), top row first.
struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<float> px; ///< 4 floats per pixel

    RgbaImage() = default;
    RgbaImage(int w, int h) : width(w), height(h), px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0.0f) {}
    bool empty() const { return width <= 0 || height <= 0; }
    /// Bilinear at fractions of the picture (u, v in 0..1 cover it; pixel centres at (i + 0.5) / width), clamped.
    std::array<float, 4> sample(float u, float v) const;
};

/// The forward and backward vectors of `flow` at fractions (u, v), bilinear and clamped: {Fu, Fv, Gu, Gv}.
std::array<float, 4> sampleFlow(const FlowPair& flow, float u, float v);

/// The picture at `t` between `a` and `b` (same size) along `flow` (any size: sampled at the same fractions). Empty
/// when the pictures differ in size or the flow is empty.
RgbaImage interpolate(const RgbaImage& a, const RgbaImage& b, const FlowPair& flow, double t, int threads = 0);

/// Plain blending at `t` (Frame Blending, and the compositor's cross dissolve): (1 - t) a + t b.
RgbaImage blend(const RgbaImage& a, const RgbaImage& b, double t);

/// Peak signal-to-noise ratio of `test` against `reference` over the first three channels, for values 0..1 (dB;
/// infinity when equal). Only the inner part (a `margin` fraction cut off each side) when `margin` > 0.
double psnr(const RgbaImage& reference, const RgbaImage& test, double margin = 0.0);

} // namespace wabcut::motion
