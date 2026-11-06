#pragma once

// AI Eye Contact (0.7, docs/ai.md "AI Eye Contact"): what the effect knows about each eye of each face and how its settings turn
// that into a correction. A landmark model (the 478-point face mesh with iris landmarks) measures every eye of every face once per
// frame; the measurements are kept in the per-clip cache like the other AI maps (wabcut/ai/video). Nothing in a measurement depends on
// the effect's settings, so moving a slider never makes them again: the evaluator turns the measurements of the frame on screen and the
// settings at that time into the few numbers the compositor needs (bakeEyeContact), one correction per eye.
//
// The correction is a redirection of the iris inside the eye's opening, not a model that repaints the eye: the iris disc moves by
// `shift` towards where it sits when the person looks into the lens (centred between the eye's corners, at the middle of the
// opening), the sclera on the iris's far side fills what it left, the lids never move and nothing outside the opening is touched
// (render/reference/LibraryDistort.cpp "EyeContact", shaders/fx_distort.frag). No gaze-redirection network with a licence that allows
// this use exists (checked 2026-11-05); the landmarks are the only learned part.
//
// Geometry. An eye lives in its own frame: the origin is the midpoint of its two corners, x runs along the line from one corner to
// the other (left to right in the picture, turned by `angle`), y runs down. The opening between the lids is the lens
//   |x| < halfWidth,  -upper (1 - u^2) < y < lower (1 - u^2),  u = x / halfWidth
// (a lid rises to `upper` above the corner line at the middle of the eye, the lower one falls to `lower` below it). Lengths are fractions
// of the measured picture's width (so a map made at a reduced size serves the full frame), positions fractions of its width and height.

#include <array>
#include <cstddef>
#include <string_view>

namespace wabcut {

inline constexpr std::string_view kAiEyeContactEffect = "wabcut.ai.eyeContact";
inline bool isAiEyeContactEffect(std::string_view effect) { return effect == kAiEyeContactEffect; }

/// How many faces a frame's measurements hold and how many the effect corrects (the nearest of them: the largest).
inline constexpr int kGazeMaxFaces = 4;

struct GazeEye {
    double cx = 0.0;         ///< the midpoint of the eye's corners: a fraction of the picture's width
    double cy = 0.0;         ///< ... and of its height
    double angle = 0.0;      ///< radians: the corner line's turn (0 = level, x to the right, y down)
    double halfWidth = 0.0;  ///< half the distance between the corners (a fraction of the picture's width)
    double upper = 0.0;      ///< how far the upper lid rises above the corner line at the middle of the eye
    double lower = 0.0;      ///< how far the lower lid falls below it
    double irisX = 0.0;      ///< the iris's centre in the eye's frame
    double irisY = 0.0;
    double irisRadius = 0.0;

    /// A usable measurement: a visible opening and an iris that fits in the eye.
    bool valid() const;
    bool operator==(const GazeEye&) const = default;
};

struct GazeFace {
    std::array<GazeEye, 2> eyes{}; ///< the eye on the picture's left, then the one on its right
    double turn = 0.0;             ///< 0 = facing the camera .. 1 = in profile (the nose's place between the cheeks)
    double score = 1.0;            ///< how sure the landmark model is that this is a face (0..1)
    double size = 0.0;             ///< the face's width, a fraction of the picture's width (which face is the main one)

    bool operator==(const GazeFace&) const = default;
};

/// The effect's settings that decide which gazes are redirected (all in percent of an eye's width; Amount in percent).
struct EyeContactSettings {
    double amount = 100.0;
    double downFrom = 5.0;    ///< looking down by less than this already looks into the lens: left alone
    double downTo = 45.0;     ///< looking down by more than this is a different look (the keyboard, the floor): left alone
    double sideLimit = 25.0;  ///< looking to a side by more than this is deliberate: left alone
};

/// What the settings make of one eye.
struct EyeCorrection {
    bool active = false;
    double gazeX = 0.0;       ///< where the iris is, in eye widths from the lens-looking place: to the right, and
    double gazeY = 0.0;       ///< down (positive)
    double openness = 0.0;    ///< the opening's height over the eye's width
    double weight = 0.0;      ///< 0..1: the gaze ranges, blink and head turn together (before Amount)
    double shiftX = 0.0;      ///< the iris moves by this in the eye's frame (a fraction of the picture's width)
    double shiftY = 0.0;
};

/// The correction of `eye` of a face turned by `turn` (GazeFace::turn). Blinks (an opening under 7 % of the eye's width, fully
/// above 14 %), turned heads (from a turn of 0.30, fully off at 0.55), gazes outside the ranges and a shift of more than 1.1 iris radii
/// are handled here: they fade out smoothly, never switch.
EyeCorrection correctEye(const GazeEye& eye, double turn, const EyeContactSettings& settings);

/// The layout of the compositor effect's parameters (render::LibraryOp::EyeContact, render/EffectLibrary.h keys): the descriptor's own
/// values come first (amount, downFrom, downTo, sideLimit, faces, view), then the baked eyes of one face.
inline constexpr std::size_t kEyeParamView = 5;   ///< 0 the corrected picture, 1 the eye map
inline constexpr std::size_t kEyeParamCount = 6;  ///< how many eyes the baked part holds (0..2)
inline constexpr std::size_t kEyeParamFirst = 8;  ///< the first eye; each takes kEyeParamStride values:
inline constexpr std::size_t kEyeParamStride = 11; ///< cx, cy, angle, halfWidth, upper, lower, irisX, irisY, irisRadius, shiftX, shiftY

/// Writes `face`'s eyes with their corrections into `params` (positions as fractions of the frame, lengths as fractions of its width) and
/// their number into params[kEyeParamCount]. Returns false, leaving the count at 0, when no eye of the face needs a correction (the
/// picture stays as it is). `params` must hold the descriptor's values already; they are not changed.
bool bakeEyeContact(std::array<double, 32>& params, const GazeFace& face, const EyeContactSettings& settings);

/// The settings from an effect's parameters (the descriptor's order: amount, downFrom, downTo, sideLimit, ...).
EyeContactSettings eyeContactSettingsOf(const std::array<double, 32>& params);

} // namespace wabcut
