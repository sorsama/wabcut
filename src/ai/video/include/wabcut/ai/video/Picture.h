#pragma once

// A 16-bit RGB picture for the AI video effects, and the few pixel operations a chain needs. Values are the stream's own
// code values, full range (0..65535 = 0..1), whatever the matrix and range of the frame they came from: the models see
// what the display shows, not linear light. An optional alpha plane (straight alpha, 0..65535) rides along; models only
// ever see the colour.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wabcut::ai::video {

struct Picture {
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> rgb;   ///< width x height x 3, interleaved R, G, B; rows top-down
    std::vector<std::uint16_t> alpha; ///< width x height, or empty (opaque)

    std::size_t pixels() const { return static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0)); }
    bool hasAlpha() const { return !alpha.empty(); }
    bool valid() const { return width > 0 && height > 0 && rgb.size() == pixels() * 3 && (alpha.empty() || alpha.size() == pixels()); }
};

/// The picture at another size, bilinear between pixel centres (colour and alpha).
Picture resized(const Picture& in, int width, int height);
/// One plane of 16-bit values (an alpha plane) at another size, bilinear.
std::vector<std::uint16_t> resizedPlane(const std::vector<std::uint16_t>& plane, int width, int height, int newWidth, int newHeight);
/// Half the size: each pixel the mean of 2 x 2 (the last row / column repeated for an odd size). The fast 2x upscale halves
/// a 4x result so.
Picture halved(const Picture& in);
/// from + k * (to - from) per colour value (0 = `from`, 1 = `to`); the pictures must be the same size, alpha is `to`'s.
/// An invalid or mismatched pair returns `to`.
Picture mixed(const Picture& from, const Picture& to, double k);
/// The mean absolute difference of the colour values of two pictures of one size, 0..1 (tests, the no-op check).
double meanAbsDifference(const Picture& a, const Picture& b);
/// Peak signal-to-noise ratio of `b` against reference `a` over the colour values, in dB (infinity when equal; 0 for a
/// mismatched pair).
double psnr(const Picture& a, const Picture& b);

/// 0.7 (AI Refocus): a depth model's relative inverse depth (any positive scale, larger = nearer) as 0..1 with 1 = the nearest:
/// the 2nd and 98th percentiles map to 0 and 1 and outliers clamp, so one stray pixel does not flatten the map. A flat or
/// empty map gives 0.5 everywhere. Non-finite values count as the lowest.
std::vector<float> normalisedDepth(const std::vector<float>& raw);
/// A grey picture (R = G = B) from 0..1 values, width x height.
Picture greyPicture(const std::vector<float>& values, int width, int height);
/// The 8-bit depth map AI Refocus stores from a grey picture of `normalisedDepth` values (1 = nearest): 0 = nearest .. 255 =
/// farthest, the distance the effect's Focus Distance is in.
std::vector<std::uint8_t> distanceMap8(const Picture& grey);

} // namespace wabcut::ai::video
