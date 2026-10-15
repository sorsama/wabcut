#pragma once

// The saliency fallback of auto reframe: where to look when no face or person is found. Spectral residual saliency
// (Hou & Zhang, CVPR 2007; the method reimplemented) on a 64-pixel-wide grey picture, combined with motion energy (what
// changed since the previous sample), gives one salient region per sample. Pure C++, deterministic.

#include <wabcut/ai/vision/Detection.h>

#include <optional>
#include <vector>

namespace wabcut::ai::vision {

struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<float> pixels; ///< 0..1, row by row

    bool valid() const { return width > 0 && height > 0 && pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height); }
};

/// A small grey version of `image`, `width` pixels wide (aspect kept, at least 1 x 1).
GrayImage toGray(const Image& image, int width = 64);

/// Spectral residual saliency of `image` (same size, 0..1, 1 = most salient). Empty for an invalid picture.
std::vector<float> spectralResidual(const GrayImage& image);

/// Mean absolute difference of two pictures of one size (0..1); 0 when they cannot be compared.
float motionEnergy(const GrayImage& a, const GrayImage& b);

struct SalientRegion {
    Box box;               ///< normalised 0..1
    float strength = 0.0f; ///< how much the region stands out (peak over mean)
};

/// The most salient region of `current`: spectral residual, plus the motion since `previous` (weighted by
/// `motionWeight`), thresholded at a multiple of the mean and boxed around the strongest part. Nullopt for a flat
/// picture.
std::optional<SalientRegion> salientRegion(const GrayImage& current, const GrayImage* previous, float motionWeight = 0.5f);

} // namespace wabcut::ai::vision
