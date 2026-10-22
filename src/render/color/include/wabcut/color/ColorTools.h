#pragma once

// Smart colour tools without ML (docs/color.md, "Auto and Color Match"). They return ordinary, editable grade
// parameters (Basic), never a baked result: the grade's CPU reference (Grade.h) is the model they fit against, so
// what they compute is exactly what the compositor will show.
//
//   autoCorrect   white balance from a grey-world / white-patch hybrid on near-neutral pixels (iteratively re-picked,
//                 so a dominant colour - grass, sky, a red wall - does not count as a cast), solved for temperature
//                 and tint; then exposure, whites and blacks fitted to histogram percentiles (99.5 % -> 92 %, 0.5 %
//                 pulled down to at most 4 %, the median drawn towards mid grey; small moves preferred).
//   colorMatch    statistics transfer in Oklab: the mean and standard deviation of L, a and b of the current clip's
//                 graded pixels are moved towards the reference's (by `strength`, the deviation ratio clamped to
//                 0.5..2 and the mean shift to sane limits), by fitting exposure, contrast, temperature, tint and
//                 saturation. Skin priority weighs skin-hued pixels 4x in both pictures; `currentWeights` /
//                 `referenceWeights` are the seam for face-region weighting (a face detector can fill them).
//   faceWeights   0.2 face priority (Premiere's Color Match "Face Detection"): per-pixel weights that make the
//                 faces' skin count most in both pictures, so the match brings the skin tones together first.

#include <wabcut/color/Grade.h>

#include <array>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::color {

/// A picture for the tools: display-referred (BT.709-encoded) straight RGB plus coverage alpha, rows of width * 4.
struct ColorImage {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;
};

struct ColorSample {
    Rgb display{0, 0, 0};
    double weight = 1.0;
};

/// Up to `maxSamples` pixels on a regular grid (alpha weights; transparent pixels skipped). `weights` (one per pixel,
/// optional) multiply in.
std::vector<ColorSample> samplePixels(const ColorImage& image, int maxSamples = 4096, const std::vector<float>* weights = nullptr);

/// A face on a picture: a box in normalised picture coordinates (0..1).
struct FaceBox {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/// Per-pixel weights of `image` (row-major, one per pixel) for face priority: 1 outside the faces; inside the ellipse
/// inscribed in each box 1 + (faceWeight - 1) x (0.25 + 0.75 x the pixel's skin likelihood), so hair, eyes and the
/// background in a box count less than skin. Empty when `faces` is.
std::vector<float> faceWeights(const ColorImage& image, std::span<const FaceBox> faces, double faceWeight = 12.0);

struct OklabStats {
    Rgb mean{0, 0, 0};
    Rgb deviation{0, 0, 0};
};

/// Weighted Oklab statistics of linear-light samples.
OklabStats oklabStats(const std::vector<Rgb>& linear, const std::vector<double>& weights);
/// The weight of a skin-hued pixel (display-referred): 1 inside the skin hue band with moderate saturation, 0 away.
double skinLikelihood(const Rgb& display);

struct AutoResult {
    GradeParams params;      ///< `start` with temperature, tint, exposure, whites and blacks set
    Rgb illuminant{1, 1, 1}; ///< the estimated cast (linear, luminance 1)
    double castBefore = 0;   ///< Oklab chroma of the estimated illuminant before / after the balance
    double castAfter = 0;
    std::string summary;
};

/// `samples` are the grade's input (the clip without this grade), display-referred. `base` carries the grade's other
/// stages (curves, LUTs) so the fit sees the whole look; its params are the starting point.
AutoResult autoCorrect(const std::vector<ColorSample>& samples, const GradeSpec& base);

struct MatchOptions {
    double strength = 1.0;    ///< 0..1
    bool skinPriority = false;
    /// Optional per-sample weights (same length as the samples): face regions, a drawn mask. The seam for a detector.
    std::vector<double> currentWeights;
    std::vector<double> referenceWeights;
};

struct MatchResult {
    GradeParams params;
    OklabStats current;   ///< the current clip as graded by `base`
    OklabStats reference;
    OklabStats target;    ///< what the fit aimed at (reference scaled by strength, clamped)
    OklabStats achieved;  ///< the current clip with the fitted parameters
    std::string summary;
};

/// `current`: the grade's input pixels of the clip to change; `reference`: the reference frame as shown.
MatchResult colorMatch(const std::vector<ColorSample>& current, const std::vector<ColorSample>& reference, const GradeSpec& base,
                       const MatchOptions& options = {});

/// Nelder-Mead minimisation inside a box (exposed for tests). Returns the best point.
std::vector<double> minimize(const std::function<double(const std::vector<double>&)>& cost, std::vector<double> start,
                             const std::vector<double>& step, const std::vector<double>& lo, const std::vector<double>& hi,
                             int maxEvaluations);

} // namespace wabcut::color
