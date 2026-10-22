#pragma once

// Pictures for motion estimation (P.2, docs/motion.md): grey planes, pyramids and the parallel loop the estimators
// share. Pure C++ (no Qt, no FFmpeg): the host decodes and hands over luma.
//
// Coordinates are pixels, x right, y down, pixel centres at integers (as in wabcut/masks/Tracker.h).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace wabcut::motion {

/// A grey picture, values 0..1 (any finite value is allowed), top row first.
struct Plane {
    int width = 0;
    int height = 0;
    std::vector<float> px;

    Plane() = default;
    Plane(int w, int h, float fill = 0.0f)
        : width(w), height(h), px(static_cast<std::size_t>(w > 0 ? w : 0) * static_cast<std::size_t>(h > 0 ? h : 0), fill) {}

    bool empty() const { return width <= 0 || height <= 0; }
    float& operator()(int x, int y) { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]; }
    float operator()(int x, int y) const { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]; }
    /// Clamped to the edge.
    float at(int x, int y) const;
    /// Bilinear at pixel coordinates, clamped to the edge.
    float sample(float x, float y) const;
};

/// `image` halved `levels - 1` times: level 0 is `image`, each next level a 5-tap binomial filter of the one before
/// and every other pixel (sizes rounded up). Stops early when a level would be smaller than 2 x 2.
std::vector<Plane> pyramid(const Plane& image, int levels);

/// `image` resampled to `width` x `height` (an area average when shrinking, bilinear when growing).
Plane resize(const Plane& image, int width, int height);

/// Central-difference gradients (x and y) of `image`, clamped at the edges.
void gradients(const Plane& image, Plane& gx, Plane& gy);

/// The analysis size of a `width` x `height` picture: scaled down (never up) so that its longer side is at most
/// `maxSide`, keeping the aspect (at least 1 x 1).
void analysisSize(int width, int height, int maxSide, int& outWidth, int& outHeight);

/// Runs body(i) for i in [0, count) on up to `threads` threads (0: the hardware's, at most 8). Each index must write
/// only its own outputs, so the result does not depend on the number of threads (deterministic).
void parallelFor(int count, int threads, const std::function<void(int)>& body);

} // namespace wabcut::motion
