#pragma once

// Point and planar tracking for masks and clip motion (I.6, docs/masks.md "Tracking"). Pure C++: the host supplies
// the pictures (TrackingServices, MaskCommands.h) as luma at a working resolution.
//
// Per frame the region tracker
//   1. picks good features (Shi-Tomasi: the smaller eigenvalue of the structure tensor) inside the region on the
//      previous picture;
//   2. follows each with pyramidal Lucas-Kanade (Bouguet: coarse to fine, iterative, sub-pixel) and back again,
//      keeping those whose round trip lands within `forwardBackward` pixels;
//   3. fits the region's motion (translation, similarity or affine) to the survivors with RANSAC and refines it on
//      the inliers by least squares;
//   4. reports a confidence (the share of features that agree, discounted by their residual); a frame whose
//      confidence or inlier count falls below the options' floor is LOST and the tracker stops there.
//
// Deterministic: the same pictures give the same motion (RANSAC draws from a fixed seed).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wabcut::masks {

struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

/// A luma picture, values 0..1, top row first, square pixels.
struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<float> px;

    GrayImage() = default;
    GrayImage(int w, int h) : width(w), height(h), px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0.0f) {}
    bool empty() const { return width <= 0 || height <= 0; }
    float at(int x, int y) const; ///< clamped to the edge
    /// Bilinear at pixel coordinates (pixel centres at integers), clamped to the edge.
    float sample(double x, double y) const;
};

enum class MotionModel : std::uint8_t { Translation, Similarity, Affine };

/// A 2x3 affine in pixels: (x', y') = (a x + b y + c, d x + e y + f).
struct Motion {
    double a = 1.0, b = 0.0, c = 0.0;
    double d = 0.0, e = 1.0, f = 0.0;

    Point2 map(Point2 p) const { return {a * p.x + b * p.y + c, d * p.x + e * p.y + f}; }
    /// `this` after `first`: p -> this(first(p)).
    Motion after(const Motion& first) const;
    std::optional<Motion> inverted() const;
    double determinant() const { return a * e - b * d; }
};

struct TrackerOptions {
    MotionModel model = MotionModel::Similarity;
    int window = 15;               ///< Lucas-Kanade window side (odd), pixels
    int levels = 4;                ///< pyramid levels (1 = full resolution only)
    int iterations = 30;           ///< per level
    double epsilon = 0.003;        ///< pixels: a level converged
    int maxPoints = 120;
    double featureQuality = 0.001; ///< relative to the strongest corner (low: one hard corner must not starve the rest)
    double minDistance = 5.0;      ///< pixels between features
    double forwardBackward = 0.5;  ///< pixels: largest round-trip error kept
    double ransacThreshold = 0.75; ///< pixels
    int ransacIterations = 300;
    double minConfidence = 0.3;    ///< below: lost
    int minInliers = 6;
};

/// One frame's result.
struct TrackStep {
    Motion motion;           ///< from the previous picture to this one
    double confidence = 0.0; ///< 0..1
    int features = 0;        ///< picked on the previous picture
    int inliers = 0;
    bool lost = false;
};

// ---- Building blocks (also used by the tests) ------------------------------------------------------------------

/// Level 0 is `image`; each next level halves it (a 5-tap binomial filter, then every other pixel).
std::vector<GrayImage> buildPyramid(const GrayImage& image, int levels);
/// Up to `maxPoints` corners inside `polygon` (pixels; empty = the whole picture), strongest first, at least
/// `minDistance` apart.
std::vector<Point2> goodFeatures(const GrayImage& image, std::span<const Point2> polygon, int maxPoints, double quality,
                                 double minDistance);

struct PointTrack {
    Point2 to;
    bool ok = false;
};
/// Pyramidal Lucas-Kanade from `from` to `to` (pyramids of the same size) for each point.
std::vector<PointTrack> trackPoints(const std::vector<GrayImage>& from, const std::vector<GrayImage>& to,
                                    std::span<const Point2> points, const TrackerOptions& options);

/// The motion of `model` taking `a[i]` to `b[i]`, robust to outliers (RANSAC, then least squares on the inliers);
/// `inliers` (optional) marks them. Nullopt when too few points fit.
std::optional<Motion> fitMotion(std::span<const Point2> a, std::span<const Point2> b, MotionModel model, double threshold,
                                int iterations, std::vector<char>* inliers = nullptr);

bool insidePolygon(std::span<const Point2> polygon, Point2 p);

// ---- The region tracker ------------------------------------------------------------------------------------------

class RegionTracker {
public:
    explicit RegionTracker(TrackerOptions options = {});

    /// Starts on `first` with the region (a closed polygon, pixels of the picture).
    void start(const GrayImage& first, std::vector<Point2> region);
    /// The region's motion from the previous picture to `next`. A lost step leaves the tracker where it was.
    TrackStep step(const GrayImage& next);

    /// The region as tracked so far (the start polygon moved by every step).
    const std::vector<Point2>& region() const noexcept { return m_region; }
    /// Every step's motion composed: start picture -> latest picture.
    const Motion& total() const noexcept { return m_total; }
    const TrackerOptions& options() const noexcept { return m_options; }

private:
    TrackerOptions m_options;
    std::vector<GrayImage> m_pyramid;
    std::vector<Point2> m_region;
    Motion m_total;
};

} // namespace wabcut::masks
