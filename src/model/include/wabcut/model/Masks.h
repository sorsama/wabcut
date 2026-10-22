#pragma once

// Masks (effect and opacity masks, Resolve's power windows) and source-anchored redactions (docs/masks.md, I.6).
//
// Coordinates: a mask lives in its layer's SOURCE frame, normalised: (0, 0) top-left, (1, 1) bottom-right of the
// media's picture, so it follows the clip's Motion, survives proxies and any render size. Lengths (feather,
// expansion) are fractions of the frame HEIGHT; rotations are degrees in pixel space (the evaluator knows the
// aspect). Mask curve time is the clip's curve time = media (source) time (Parameters.h), so a mask, its path keys
// and its tracking keys stick to the media like every other keyframe.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Parameters.h> // ParamValue, Interpolation (Effects.h includes this header, not the reverse)
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

/// Matte (AI-14): an AI object matte, per-frame alpha mattes of a media stream (MaskMatte); its centre / size keys
/// hold the matte's bounding box, which is what a frame without a matte falls back to.
enum class MaskShape : std::uint8_t { Ellipse, Rectangle, Path, Matte };
/// How a mask combines with the masks before it in its list. The list starts empty when its first mask adds, full when
/// it subtracts or intersects.
enum class MaskMode : std::uint8_t { Add, Subtract, Intersect };

std::string_view toString(MaskShape shape);
std::optional<MaskShape> maskShapeFromString(std::string_view text);
std::string_view toString(MaskMode mode);
std::optional<MaskMode> maskModeFromString(std::string_view text);

/// One vertex of a path: the point and its Bezier tangents as offsets from it (zero = a corner).
struct MaskVertex {
    double x = 0.0;
    double y = 0.0;
    double inX = 0.0;
    double inY = 0.0;
    double outX = 0.0;
    double outY = 0.0;

    bool operator==(const MaskVertex&) const = default;
};

/// A path shape at one curve time. Every key of a mask has the same vertex count, so shapes interpolate vertex by
/// vertex: Hold, Linear and the Ease* interpolations (a key's leaving side and the next key's arriving side; Bezier
/// is treated as EaseInOut).
struct MaskPathKey {
    Ticks time;
    std::vector<MaskVertex> vertices;
    Interpolation interpolation = Interpolation::Linear;

    bool operator==(const MaskPathKey&) const = default;
};

/// MaskShape::Matte (AI-14, docs/masks.md "Object masks"): the matte track it shows, made from one media stream by a
/// segmentation model and kept in the analysis cache (per-frame alpha, compressed). The renderer turns the matte at a
/// frame into the shape's distance field, so feather, expansion (choke when negative), opacity, invert, the transform
/// and the combination work as for any shape. A frame without a matte (not propagated there, the cache cleared, another
/// computer) shows the bounding box (center / size keys): a redaction never shows nothing.
struct MaskMatte {
    std::string track; ///< the matte track's identity in the analysis cache (opaque, <= kMaxMatteTrack bytes)
    MediaId media;     ///< the media it was made from (invalid for a nested picture's matte: `sequence`)
    int stream = 0;    ///< its video stream (ordinal)
    TimeRange source;  ///< the media time the mattes cover (a nested picture's: nested sequence time)
    std::string model; ///< the segmentation model and version ("sam2.1-tiny-video@aa7a8542")
    /// 0.2: a matte made from what a nested or multicam clip shows (the nested sequence's canvas, a multicam source's
    /// active angle) instead of a media stream: that sequence. Exactly one of `media` and `sequence` is valid; the
    /// mattes are then at nested sequence time, the clip's curve time.
    SequenceId sequence;

    bool operator==(const MaskMatte&) const = default;
};

/// A keyframable mask parameter (the same shape as EffectParam).
struct MaskParam {
    std::string key;
    ParamValue value;

    bool operator==(const MaskParam&) const = default;
};

struct Mask {
    std::uint32_t id = 0; ///< unique within its list (assigned by the ops)
    std::string name;     ///< "Mask (1)", "Face"
    MaskShape shape = MaskShape::Ellipse;
    MaskMode mode = MaskMode::Add;
    bool inverted = false;
    bool enabled = true;
    /// Keyframable parameters, sorted by key (maskParams(); unknown keys are kept). A key not listed has its default.
    std::vector<MaskParam> params;
    /// MaskShape::Path: the closed path's keys, times strictly increasing, >= 3 vertices each, equal counts.
    std::vector<MaskPathKey> path;
    /// MaskShape::Matte: the matte track (required for that shape, absent for the others).
    std::optional<MaskMatte> matte;

    const ParamValue* find(std::string_view key) const;
    /// Sets (or adds) a parameter, keeping `params` sorted.
    void set(std::string key, ParamValue value);

    bool operator==(const Mask&) const = default;
};

/// The parameters of a mask: the shape (ellipse and rectangle), the edge and the transform the tracker writes.
///   center.x, center.y   0.5      shape centre                   size.x, size.y       0.5    bounding box width, height
///   rotation             0 deg    shape rotation                 roundness            0      rectangle corners, 0..1
///   feather              0        soft edge width (frame heights) expansion           0      grow (+) / shrink (-)
///   opacity              1        0..1
///   transform.x/.y       0        offset                         transform.scaleX/.scaleY    1
///   transform.rotation   0 deg    transform.skew       0 deg     transform.anchorX/.anchorY  0.5 (about this point)
struct MaskParamInfo {
    std::string_view key;
    double defaultValue;
    double minValue;
    double maxValue;
};
std::span<const MaskParamInfo> maskParams();
const MaskParamInfo* maskParam(std::string_view key);
/// A parameter's value at a curve time (its default when the mask does not list it; 0 for an unknown key).
double maskValue(const Mask& mask, std::string_view key, Ticks curveTime);

/// The path vertices at a curve time (MaskShape::Path), before the transform: keys interpolated vertex by vertex.
std::vector<MaskVertex> maskPathAt(const Mask& mask, Ticks curveTime);
/// The mask's outline at a curve time: a closed cubic Bezier path in normalised source-frame coordinates (ellipse: 4
/// arcs; rectangle: 4 edges, rounded corners; path: the interpolated keys) with the transform applied. `aspect` =
/// frame width / height (rotation and roundness are isotropic in pixels). Empty when the shape has no area.
std::vector<MaskVertex> maskOutlineAt(const Mask& mask, Ticks curveTime, double aspect);
/// The same, without the transform.* parameters (the tracker's transform is applied by whoever samples the shape).
std::vector<MaskVertex> maskShapeAt(const Mask& mask, Ticks curveTime, double aspect);

/// The 2x3 affine of the transform.* parameters in normalised coordinates: (x', y') = (a x + b y + c, d x + e y + f).
struct MaskAffine {
    double a = 1.0, b = 0.0, c = 0.0;
    double d = 0.0, e = 1.0, f = 0.0;

    bool operator==(const MaskAffine&) const = default;
};
MaskAffine maskTransformAt(const Mask& mask, Ticks curveTime, double aspect);

/// Bounds (project files are untrusted input).
inline constexpr std::size_t kMaxMasks = 16;           ///< per effect instance
inline constexpr std::size_t kMaxMaskVertices = 256;
inline constexpr std::size_t kMaxMaskPathKeys = 10'000;
inline constexpr std::size_t kMaxMaskName = 1024;
inline constexpr std::size_t kMaxMatteTrack = 256; ///< MaskMatte::track and ::model bytes
/// The problem with a mask, if any: a path with < 3 or > kMaxMaskVertices vertices or keys with different counts,
/// times not increasing, non-finite numbers, too many keys, parameters out of range or malformed curves.
std::optional<std::string> maskProblem(const Mask& mask);
/// The same for a list, plus its size (kMaxMasks) and unique non-zero ids.
std::optional<std::string> masksProblem(std::span<const Mask> masks);

// ---- Source-anchored redactions (privacy) ----------------------------------------------------------------------

enum class RedactionStyle : std::uint8_t { Blur, Mosaic, Solid };
enum class RedactionAudio : std::uint8_t { None, Mute, Bleep };

std::string_view toString(RedactionStyle style);
std::optional<RedactionStyle> redactionStyleFromString(std::string_view text);
std::string_view toString(RedactionAudio audio);
std::optional<RedactionAudio> redactionAudioFromString(std::string_view text);

/// A region of a media item marked once in SOURCE time. It applies to every use of that source range in every
/// sequence (trims, speed changes, reverse, reuse), before any effect of the clip, in preview, export and burn-in,
/// except on the clips listed in `exceptions` ("don't redact here").
struct Redaction {
    RedactionId id;
    MediaId media;
    std::string name; ///< "Licence plate"
    bool enabled = true;
    TimeRange source; ///< the media time it covers (the picture, and the sound unless `audioRanges` are given)
    Mask region;      ///< the area (curve time = media time); its `mode` is ignored
    RedactionStyle style = RedactionStyle::Blur;
    double strength = 0.05; ///< blur radius / mosaic cell, in frame heights
    double colorR = 0.0;    ///< Solid: display-referred 0..1
    double colorG = 0.0;
    double colorB = 0.0;
    RedactionAudio audio = RedactionAudio::None;
    /// Audio: the media times bleeped or muted (transcript word ranges); empty = `source`.
    std::vector<TimeRange> audioRanges;
    /// Clips it does not apply to. A clip an edit makes from one of them (a razor half) is redacted again until it is
    /// excepted too: the safe direction. Sorted, unique.
    std::vector<ClipId> exceptions;

    bool appliesTo(ClipId clip) const;
    bool operator==(const Redaction&) const = default;
};

inline constexpr std::size_t kMaxRedactions = 4096;
inline constexpr std::size_t kMaxRedactionRanges = 4096;
std::optional<std::string> redactionProblem(const Redaction& redaction);

} // namespace wabcut
