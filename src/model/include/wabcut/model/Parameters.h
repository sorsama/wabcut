#pragma once

// Clip parameters: Premiere's fixed effects (Motion, Opacity, Volume, Panner) as a table of parameters
// with stable ids, units, ranges and defaults, and the values they hold: a constant or a keyframe curve.
//
// Curve time. Keyframe times are *media time*: source ticks of the clip's media, the same axis as
// Clip::sourceIn. That is Premiere's behaviour: keyframes stick to the media, so slipping a clip moves its
// keyframes with the picture, trimming hides keyframes outside the clip without deleting them (they come
// back when the clip is extended again), and both halves of a razor cut share one curve that evaluates
// seamlessly across the cut. Clip::curveTimeAt() maps a timeline time to curve time (Clip.h); evaluators
// call Clip::parameterAt(), never valueAt(time - clip.start()).
//
// Evaluation (Curve::valueAt) is exact and deterministic: times stay integer ticks until the segment is
// normalised to [0, 1] in double; values before the first key / after the last key are clamped; a value at
// a key's time is that key's value; Bezier segments solve time by bisection on a curve whose time
// controls are clamped into the segment, so time is always monotonic.

#include <wabcut/model/Time.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace wabcut {

/// What kind of property changed (ChangeSet) or a parameter belongs to. Bit flags.
enum class PropertyRole : std::uint32_t {
    None = 0,
    Timing = 1u << 0,  ///< timeline position or length
    Source = 1u << 1,  ///< source In, media, stream, speed, direction
    Enabled = 1u << 2, ///< clip enabled/disabled
    Label = 1u << 3,   ///< name or label colour
    Link = 1u << 4,    ///< link group
    Track = 1u << 5,   ///< moved to another track
    Motion = 1u << 6,  ///< position, scale, rotation, anchor
    Opacity = 1u << 7,
    Audio = 1u << 8,   ///< volume, pan
    Effects = 1u << 9, ///< the effect stack (instances, order, their parameters)
};

/// A set of PropertyRole flags.
struct PropertyRoles {
    std::uint32_t bits = 0;

    constexpr PropertyRoles() = default;
    constexpr PropertyRoles(PropertyRole role) : bits(static_cast<std::uint32_t>(role)) {} // NOLINT: implicit by design
    constexpr bool has(PropertyRole role) const { return (bits & static_cast<std::uint32_t>(role)) != 0; }
    constexpr bool empty() const { return bits == 0; }
    constexpr PropertyRoles& operator|=(PropertyRoles o) {
        bits |= o.bits;
        return *this;
    }
    constexpr bool operator==(const PropertyRoles&) const = default;
};

constexpr PropertyRoles operator|(PropertyRoles a, PropertyRoles b) { return a |= b; }
constexpr PropertyRoles operator|(PropertyRole a, PropertyRole b) { return PropertyRoles(a) | PropertyRoles(b); }

/// Stable parameter ids. The numeric values index ClipProperties; the serialized form is the key in
/// the parameter table, so the enum may grow but existing entries never change meaning.
enum class ParamId : std::uint8_t {
    PositionX,
    PositionY,
    Scale,
    Rotation,
    AnchorX,
    AnchorY,
    Opacity,
    VolumeDb,
    Pan,
};
inline constexpr std::size_t kParamCount = 9;

enum class ParamUnit : std::uint8_t {
    Pixels,       ///< position: offset of the anchor from the canvas centre (sequence px); anchor: from the frame centre (source px)
    Factor,       ///< 1.0 = 100 %
    Degrees,      ///< clockwise
    Fraction,     ///< 0..1
    Decibels,     ///< 0 = unity gain
    Balance,      ///< -1 = full left .. +1 = full right
    Amount,       ///< a unitless slider (its range says what it means, e.g. -100..100)
    Stops,        ///< exposure stops (EV): +1 doubles linear light
    Hertz,
    Milliseconds,
    Ratio,        ///< e.g. a compressor ratio (4 = 4:1) or a filter Q
};

struct ParameterInfo {
    ParamId id;
    std::string_view key;         ///< stable serialized id, e.g. "motion.position.x"
    std::string_view displayName; ///< English UI name (translated by the app)
    ParamUnit unit;
    double defaultValue;
    double minValue;
    double maxValue;
    PropertyRole role;
};

/// The parameter table, in ParamId order.
std::span<const ParameterInfo> allParameters();
const ParameterInfo& parameterInfo(ParamId id);
/// Looks a parameter up by its serialized key.
std::optional<ParamId> parameterByKey(std::string_view key);

// --- Keyframes ------------------------------------------------------------------------------------------

/// How a key shapes the curve around it. The segment between keys A and B is shaped by A's leaving side
/// and B's arriving side (Premiere's temporal interpolation):
///   Hold       A's value until B (a step); B's arriving side is ignored.
///   Linear     straight on this key's sides.
///   EaseIn     flat arrival into this key, linear departure.
///   EaseOut    linear arrival, flat departure from this key.
///   EaseInOut  flat on both sides.
///   Bezier     both sides use this key's handles (slope and influence).
/// A segment whose two sides are linear is a straight line; any other is a cubic Bezier.
enum class Interpolation : std::uint8_t { Hold, Linear, Bezier, EaseIn, EaseOut, EaseInOut };

std::string_view toString(Interpolation interpolation);
std::optional<Interpolation> interpolationFromString(std::string_view text);

/// One side of a Bezier key, like After Effects' speed/influence: the control point lies `influence` of the
/// way along the segment (0..1) and leaves/arrives at `slope` value units per second.
struct BezierHandle {
    double slope = 0.0;
    double influence = 1.0 / 3.0;

    bool operator==(const BezierHandle&) const = default;
};

struct Keyframe {
    Ticks time;    ///< curve time (media time; see the file comment)
    double value = 0.0;
    Interpolation interpolation = Interpolation::Linear;
    BezierHandle in;  ///< arriving side, used when interpolation is Bezier
    BezierHandle out; ///< leaving side, used when interpolation is Bezier

    bool operator==(const Keyframe&) const = default;
};

/// A keyframed value. Keys are sorted by strictly increasing time; a curve has at least one key.
struct Curve {
    std::vector<Keyframe> keys;

    /// The value at a curve time: clamped outside the keys, exact at a key, interpolated between keys.
    double valueAt(Ticks time) const;
    /// Index of the key at exactly `time`, if any.
    std::optional<std::size_t> keyAt(Ticks time) const;
    /// Index a key at `time` would have (the first key at or after it).
    std::size_t insertionIndex(Ticks time) const;
    /// Keys sorted, times distinct, values and handles finite, influences in [0, 1].
    bool isWellFormed() const;

    bool operator==(const Curve&) const = default;
};

/// The value of one parameter: a constant or a keyframe curve.
struct ParamValue {
    std::variant<double, Curve> data{0.0};

    ParamValue() = default;
    ParamValue(double constant) : data(constant) {} // NOLINT: a constant is a value
    ParamValue(Curve curve) : data(std::move(curve)) {} // NOLINT: so is a curve

    bool isConstant() const { return std::holds_alternative<double>(data); }
    bool isCurve() const { return std::holds_alternative<Curve>(data); }
    /// The curve, or null for a constant.
    const Curve* curve() const { return std::get_if<Curve>(&data); }
    /// The constant value (for a curve: its value at curve time 0).
    double constant() const { return isConstant() ? std::get<double>(data) : std::get<Curve>(data).valueAt({}); }
    /// The value at a curve time (Clip::curveTimeAt). A constant ignores the time.
    double valueAt(Ticks curveTime) const {
        return isConstant() ? std::get<double>(data) : std::get<Curve>(data).valueAt(curveTime);
    }
    /// Every value it can take is finite and within [min, max] (all keys for a curve), and a curve is well formed.
    bool isValid(double minValue, double maxValue) const;

    bool operator==(const ParamValue&) const = default;
};

/// All parameters of a clip, defaults included.
struct ClipProperties {
    std::array<ParamValue, kParamCount> values;

    ClipProperties();

    const ParamValue& operator[](ParamId id) const { return values[static_cast<std::size_t>(id)]; }
    ParamValue& operator[](ParamId id) { return values[static_cast<std::size_t>(id)]; }
    /// Value at a curve time (Clip::curveTimeAt; evaluators use Clip::parameterAt).
    double value(ParamId id, Ticks curveTime = {}) const { return (*this)[id].valueAt(curveTime); }
    void set(ParamId id, double constant) { (*this)[id] = ParamValue(constant); }
    bool isDefault(ParamId id) const;
    /// Roles of the parameters that differ between the two.
    PropertyRoles differences(const ClipProperties& other) const;
    /// The first parameter that is not finite, outside its range or a malformed curve, if any.
    std::optional<ParamId> firstInvalid() const;

    bool operator==(const ClipProperties&) const = default;
};

} // namespace wabcut
