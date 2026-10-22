#pragma once

// Video stabilization (C.15 parked, finished in P.2; docs/motion.md), after Premiere Pro's Warp Stabilizer: analyse once,
// smooth as often as the settings change.
//
// 1. analyzeMotion() measures the global motion of every analysed frame against the previous one: features spread over a
//    grid of cells (Shi-Tomasi, so flat cells do not starve the rest), pyramidal Lucas-Kanade with a forward-backward
//    check (the I.6 tracker, wabcut/masks/Tracker.h), then a robust fit (RANSAC + least squares) of a translation
//    (Method::Position), a similarity (Method::Similarity: position, scale, rotation) or a homography
//    (Method::Perspective). Frames are tracked against a keyframe for as long as it overlaps well, so per-frame errors do
//    not accumulate into drift; a frame that cannot be matched (a blank frame, a cut) is LOST: identity motion, and the
//    next frames are matched against the last good picture again. Deterministic.
// 2. The analysis (MotionAnalysis, JSON) is cached per media fingerprint by the caller.
// 3. stabilize() composes the camera path, smooths it (Gaussian or the L1-optimal path of auto reframe) or locks it
//    (No Motion), and returns the compensating transform per frame plus the framing (crop edges, auto scale).
//
// Coordinates: pixels, x right, y down, pixel centres at integers (the tracker's convention), so the frame centre of a
// w x h picture is ((w - 1) / 2, (h - 1) / 2) and its corners are (-0.5, -0.5) .. (w - 0.5, h - 0.5).

#include <wabcut/masks/Tracker.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::stabilize {

enum class Method : std::uint8_t { Position, Similarity, Perspective }; // Premiere: Position; Position, Scale, Rotation; Perspective
enum class Smoothing : std::uint8_t { Gaussian, L1 };
/// Premiere's Framing. SynthesizeEdges compensates like StabilizeOnly; the renderer fills the uncovered borders.
enum class Framing : std::uint8_t { StabilizeOnly, Crop, CropAutoScale, SynthesizeEdges };
/// P.2, Premiere's Advanced > Rolling Shutter Ripple (Automatic Reduction, Enhanced Reduction); None for no explicit
/// de-skew.
enum class RippleReduction : std::uint8_t { None, Automatic, Enhanced };

/// A 3x3 projective transform in pixels (row-major, m[8] normalised to 1). Affine motions have m[6] = m[7] = 0.
struct Homography {
    std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};

    masks::Point2 map(masks::Point2 p) const;
    Homography after(const Homography& first) const; // p -> this(first(p))
    std::optional<Homography> inverted() const;
    static Homography fromMotion(const masks::Motion& motion);
    bool operator==(const Homography&) const = default;
};

/// RANSAC over 4-point samples + normalised-DLT refit on the inliers; nullopt when fewer than 8 inliers.
std::optional<Homography> fitHomography(std::span<const masks::Point2> a, std::span<const masks::Point2> b, double threshold,
                                        int iterations, std::vector<char>* inliers = nullptr);

/// The homography taking the four points `from` exactly to `to` (a corner pin); nullopt when three of either are collinear.
std::optional<Homography> homographyFromCorners(const std::array<masks::Point2, 4>& from, const std::array<masks::Point2, 4>& to);

/// One analysed frame: how the picture moved from the previous analysed frame to this one (identity for the first).
struct FrameMotion {
    Ticks time;            ///< media time of the frame
    Homography motion;     ///< previous picture -> this picture, analysis pixels
    double confidence = 0; ///< 0..1 (inliers / features)
    bool lost = false;     ///< too few inliers: motion is identity (a cut, a blank frame)
    bool operator==(const FrameMotion&) const = default;
};

/// The raw global-motion analysis of a clip's media (cached per media fingerprint by the caller). Smoothing is NOT in
/// here: changing smoothness reuses the analysis (as Warp Stabilizer does).
///
/// A frame after a lost one may be measured against an older picture (the last good one): its motion is then relative to
/// the camera position the lost frames held, so composing the motions in order always gives the camera path.
struct MotionAnalysis {
    static constexpr std::string_view kAnalyzer = "wabcut.stabilize";
    static constexpr int kVersion = 1;
    int width = 0; ///< analysis picture size (pixels)
    int height = 0;
    Method method = Method::Similarity;
    std::vector<FrameMotion> frames;
    bool operator==(const MotionAnalysis&) const = default;
};

inline constexpr std::size_t kMaxAnalysisFrames = 200'000;

/// Compact JSON: {"schema": "wabcut.stabilize", "version": 1, "width", "height", "method": "position" | "similarity" |
/// "perspective", "frames": [{"t": ticks, "m": [9 numbers], "c": confidence, "lost": bool}]}. Round-trips exactly.
std::string toJson(const MotionAnalysis& analysis);
/// Untrusted input (a cache file): bounded (at most kMaxAnalysisFrames frames, finite numbers, increasing times, sizes
/// 1..16384, no deep nesting), every problem an Error (ErrorCode::ParseError for malformed or mistyped input,
/// ErrorCode::InvalidArgument for values out of range), never a crash.
std::expected<MotionAnalysis, Error> motionAnalysisFromJson(std::string_view json);

using FrameReader = std::function<std::expected<masks::GrayImage, Error>(Ticks mediaTime)>;
/// Called with 0..1 after each frame; returning false cancels (the analysis then fails with ErrorCode::Cancelled).
using Progress = std::function<bool(double fraction)>;

struct AnalyzeOptions {
    Method method = Method::Similarity;
    masks::TrackerOptions tracker; ///< tracker.model is chosen from `method` (Translation for Position, Similarity...)
    int gridFeatures = 200;        ///< features per frame, spread over a grid of cells (not only the strongest corners)
    /// P.2, Advanced > Detailed Analysis: two and a half times the features and a tighter forward-backward check (slower,
    /// steadier on busy or low-contrast pictures).
    bool detailed = false;
};

/// Global motion per frame: features on a grid of cells of the previous picture (the keyframe), pyramidal LK,
/// forward-backward check, then a robust fit (fitMotion for Position/Similarity, fitHomography for Perspective).
/// `frameTimes` strictly increasing, 1..kMaxAnalysisFrames of them (else InvalidArgument). Pictures of one analysis must
/// all be the same size, 1..16384 pixels a side (else InvalidArgument). A reader error ends the analysis with that error.
std::expected<MotionAnalysis, Error> analyzeMotion(std::span<const Ticks> frameTimes, const FrameReader& read,
                                                   const AnalyzeOptions& options = {}, const Progress& progress = {});

struct SmoothOptions {
    double smoothness = 0.5; ///< Premiere's Smoothness / 100 (50 % = 0.5), 0..10; a time constant, independent of the frame rate
    Smoothing smoothing = Smoothing::Gaussian;
    bool noMotion = false; ///< Result "No Motion": lock the camera to the first frame
    Framing framing = Framing::CropAutoScale;
    double maxScale = 1.5;        ///< cap of the auto scale (Premiere's Maximum Scale, 150 %)
    double additionalScale = 1.0; ///< multiplies the result scale (every framing)
    double frameRate = 30.0;      ///< frames per second of the analysed frames (for the smoothing time constant)
    /// P.2, Preserve Scale: the camera's moves toward and away from the scene (the path's scale) are kept, not smoothed.
    bool preserveScale = false;
    /// P.2, Auto-scale > Action-Safe Margin: a fraction (0..0.49) of the frame at each edge the auto scale does not try to
    /// fill.
    double actionSafeMargin = 0.0;
    /// P.2, Advanced > Crop Less <-> Smooth More (0..1, Premiere's 50 % = 0.5): scales the smoothing when the framing
    /// crops or scales (x0.5 at 0, x1.5 at 1): less smoothing leaves smaller borders to crop.
    double cropLessSmoothMore = 0.5;
    /// P.2, Advanced > Rolling Shutter Ripple: the per-line skew of a rolling-shutter camera comes out with the
    /// compensation, at the scan share rollingShutterShares() measures in the analysis (none for a global shutter).
    /// Only Perspective's corner pins can carry a skew; the other methods ignore it. None (the default) leaves the skew
    /// to the path smoothing, which takes out its jitter like any other.
    RippleReduction rippleReduction = RippleReduction::None;
};

/// P.2: the picture's motion per analysed frame (source pixels per frame, the frame centre's), from the analysis: what
/// Rolling Shutter Repair keys (central differences of the camera path; lost frames hold the neighbours').
struct FrameVelocity {
    std::vector<Ticks> times;
    std::vector<double> x, y;
};
FrameVelocity velocities(const MotionAnalysis& analysis, int sourceWidth, int sourceHeight);

/// P.2: the share of the frame time a rolling-shutter camera's scan took (0..1), per analysed frame, measured in a
/// Perspective analysis: the regression of the camera path's skew (each picture's lines sheared in proportion to the
/// motion) on the frame's motion. A global-shutter camera, or too little motion to tell, gives 0. Automatic: one share
/// for the whole clip; Enhanced: a share per frame over a window of a second each side (where that window moves too
/// little, the clip's). Empty for None or another method.
std::vector<double> rollingShutterShares(const MotionAnalysis& analysis, RippleReduction mode, double frameRate);

/// The compensating transform per analysed frame, to apply to the source picture (about its centre) BEFORE the clip's
/// Motion so the camera path becomes the smoothed one. Lengths in source pixels (the analysis is scaled up by
/// sourceWidth / analysis.width).
///
/// Position / Similarity fill x, y, rotation and scale (and leave `corners` empty): a source pixel p goes to
/// centre + (x, y) + scale * R(rotation) * (p - centre). Perspective fills `corners` (and leaves the others empty).
struct Compensation {
    std::vector<Ticks> times;
    // Position / Similarity: translate (x right, y down, pixels), rotate (degrees clockwise) and scale (factor) about the
    // frame centre; scale includes autoScale and additionalScale when framing is CropAutoScale.
    std::vector<double> x, y, rotation, scale;
    // Perspective: the compensating corner pin, as offsets (pixels) of the four frame corners tl, tr, br, bl (x, y each).
    std::vector<std::array<double, 8>> corners;
    double autoScale = 1.0; ///< the uniform scale (>= 1) that hides every uncovered border, capped at maxScale
    double cropLeft = 0, cropTop = 0, cropRight = 0, cropBottom = 0; ///< fractions of the frame ever uncovered (Framing::Crop)
    bool scaleCapped = false; ///< the needed scale was above maxScale (borders may show)
    double rollingShutterShare = 0.0; ///< P.2: the mean scan share the ripple reduction took out (0 when none)
};

/// Smooths (or locks) the camera path of `analysis`. Lost frames do not pull the smoothed path. autoScale, the crop
/// edges and scaleCapped are reported for every framing; only CropAutoScale applies autoScale to the transforms (Crop
/// leaves cropping to the caller). Empty when the analysis has no frames or a size is not positive.
Compensation stabilize(const MotionAnalysis& analysis, const SmoothOptions& options, int sourceWidth, int sourceHeight);

/// The compensating transform of frame i as one homography in source pixels (for the tests and the render handoff);
/// identity when i is out of range.
Homography compensationAt(const Compensation& c, std::size_t i, int sourceWidth, int sourceHeight);

} // namespace wabcut::ai::stabilize
