#pragma once

// Dense optical flow (P.2, docs/motion.md): the motion of every pixel between two pictures, for Time Interpolation's
// Optical Flow, Morph Cut and the stabilizers.
//
// The estimator is Dense Inverse Search (Kroeger, Timofte, Dai and Van Gool, "Fast Optical Flow using Dense Inverse
// Search", ECCV 2016), coarse to fine over a pyramid of each picture:
//   1. a grid of overlapping square patches on the "from" picture;
//   2. each patch's displacement found by inverse compositional Gauss-Newton on the patch (zero-mean, so a change of
//      brightness does not pull it), starting from the coarser level's field; a patch whose error grows or that runs
//      away by more than its own size keeps its starting value;
//   3. densification: each pixel takes the average of the patches covering it, weighted by how well each one's
//      displacement matches the pixel (1 / max(1, |error| in 8-bit steps));
//   4. a 3 x 3 vector median on the field (cheap regularisation of outliers at motion boundaries).
// Deterministic: the same pictures give the same field on any number of threads.

#include <wabcut/motion/Image.h>

#include <cstddef>
#include <vector>

namespace wabcut::motion {

/// For each pixel x of the picture it was estimated from, where that pixel lies in the other picture: x + (u, v), in
/// pixels of this field.
struct FlowField {
    int width = 0;
    int height = 0;
    std::vector<float> uv; ///< interleaved u, v; row-major

    FlowField() = default;
    FlowField(int w, int h) : width(w), height(h), uv(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 2, 0.0f) {}
    bool empty() const { return width <= 0 || height <= 0; }
    float& u(int x, int y) { return uv[2 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x))]; }
    float& v(int x, int y) { return uv[2 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) + 1]; }
    float u(int x, int y) const { return uv[2 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x))]; }
    float v(int x, int y) const { return uv[2 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) + 1]; }
    /// Bilinear at pixel coordinates, clamped to the edge.
    void sample(float x, float y, float& outU, float& outV) const;
    /// The field resampled to `w` x `h` (bilinear), its vectors scaled by the size ratio.
    FlowField resized(int w, int h) const;
};

struct FlowOptions {
    int patchSize = 8;      ///< pixels, at every level
    int patchStride = 4;    ///< pixels between patch origins (patchSize / 2: each pixel in about four patches)
    int iterations = 12;    ///< Gauss-Newton steps per patch
    int coarsestSide = 16;  ///< the pyramid stops above a level whose shorter side would be below this
    int finestLevel = 0;    ///< the finest level estimated (0: full size)
    int medianPasses = 1;   ///< component-wise 3 x 3 median passes after each level
    int threads = 0;        ///< parallelFor's threads (0: the hardware's)
};

/// The flow from `from` to `to` (same size; empty field otherwise): from(x) ~ to(x + flow(x)). Same size as the
/// pictures, upsampled from options.finestLevel when that is coarser.
FlowField estimateFlow(const Plane& from, const Plane& to, const FlowOptions& options = {});
/// The same at the finest level estimated (the pictures' size halved options.finestLevel times, rounded up).
FlowField estimateFlowAtFinestLevel(const Plane& from, const Plane& to, const FlowOptions& options = {});

/// Motion both ways between pictures A and B, as the compositor samples it (render::MotionField): for each pixel, the
/// forward vector (A to B) and the backward one (B to A), as fractions of the picture's width and height.
struct FlowPair {
    int width = 0;
    int height = 0;
    std::vector<float> data; ///< 4 floats per pixel, row-major: forward u, v, backward u, v (fractions)

    bool empty() const { return width <= 0 || height <= 0; }
};

/// Both flows between `a` and `b` (estimateFlowAtFinestLevel each way: the pair has that level's size), packed as
/// fractions.
FlowPair estimateFlowPair(const Plane& a, const Plane& b, const FlowOptions& options = {});

/// The settings Time Interpolation, Morph Cut and the stabilizers analyse with (one quality for preview and export):
/// pictures whose longer side is at most kAnalysisSide pixels, patches of 8 every 4 pixels down to half that size.
inline constexpr int kAnalysisSide = 960;
FlowOptions analysisFlowOptions();

/// The mean forward-backward disagreement of `pair` over its pixels, in pixels of the pair: |F(x) + B(x + F(x))|.
/// About 0 where both flows agree (no occlusion, a good estimate).
double meanConsistencyError(const FlowPair& pair);

} // namespace wabcut::motion
