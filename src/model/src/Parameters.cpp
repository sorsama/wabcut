#include <wabcut/model/Parameters.h>

#include <algorithm>
#include <cmath>

namespace wabcut {

namespace {

constexpr double kMaxPixels = 1e7;
constexpr double kMaxDegrees = 1e6;

// Keys are part of the .wabcut format: never rename an entry, only add new ones.
constexpr std::array<ParameterInfo, kParamCount> kParameters{{
    {ParamId::PositionX, "motion.position.x", "Position X", ParamUnit::Pixels, 0.0, -kMaxPixels, kMaxPixels,
     PropertyRole::Motion},
    {ParamId::PositionY, "motion.position.y", "Position Y", ParamUnit::Pixels, 0.0, -kMaxPixels, kMaxPixels,
     PropertyRole::Motion},
    {ParamId::Scale, "motion.scale", "Scale", ParamUnit::Factor, 1.0, 0.0, 100.0, PropertyRole::Motion},
    {ParamId::Rotation, "motion.rotation", "Rotation", ParamUnit::Degrees, 0.0, -kMaxDegrees, kMaxDegrees,
     PropertyRole::Motion},
    {ParamId::AnchorX, "motion.anchor.x", "Anchor Point X", ParamUnit::Pixels, 0.0, -kMaxPixels, kMaxPixels,
     PropertyRole::Motion},
    {ParamId::AnchorY, "motion.anchor.y", "Anchor Point Y", ParamUnit::Pixels, 0.0, -kMaxPixels, kMaxPixels,
     PropertyRole::Motion},
    {ParamId::Opacity, "opacity.opacity", "Opacity", ParamUnit::Fraction, 1.0, 0.0, 1.0, PropertyRole::Opacity},
    {ParamId::VolumeDb, "volume.level", "Level", ParamUnit::Decibels, 0.0, -144.0, 24.0, PropertyRole::Audio},
    {ParamId::Pan, "panner.balance", "Balance", ParamUnit::Balance, 0.0, -1.0, 1.0, PropertyRole::Audio},
}};

constexpr bool tableIsInIdOrder() {
    for (std::size_t i = 0; i < kParameters.size(); ++i) {
        if (static_cast<std::size_t>(kParameters[i].id) != i) {
            return false;
        }
    }
    return true;
}
static_assert(tableIsInIdOrder(), "kParameters must be listed in ParamId order");

} // namespace

std::span<const ParameterInfo> allParameters() { return kParameters; }

const ParameterInfo& parameterInfo(ParamId id) { return kParameters[static_cast<std::size_t>(id)]; }

std::optional<ParamId> parameterByKey(std::string_view key) {
    for (const ParameterInfo& p : kParameters) {
        if (p.key == key) {
            return p.id;
        }
    }
    return std::nullopt;
}

ClipProperties::ClipProperties() {
    for (const ParameterInfo& p : kParameters) {
        (*this)[p.id] = ParamValue(p.defaultValue);
    }
}

// --- Keyframes --------------------------------------------------------------------------------------------

namespace {

constexpr std::array<std::pair<Interpolation, std::string_view>, 6> kInterpolations{{
    {Interpolation::Hold, "hold"},
    {Interpolation::Linear, "linear"},
    {Interpolation::Bezier, "bezier"},
    {Interpolation::EaseIn, "easeIn"},
    {Interpolation::EaseOut, "easeOut"},
    {Interpolation::EaseInOut, "easeInOut"},
}};

/// One side of a segment in normalised time: the control point's time (0..1 of the segment from the side's
/// key) and its value offset from the key.
struct Side {
    double influence;
    double delta;
};

/// A's leaving side of the segment A -> B.
Side leaving(const Keyframe& a, const Keyframe& b, double seconds) {
    switch (a.interpolation) {
    case Interpolation::EaseOut:
    case Interpolation::EaseInOut:
        return {1.0 / 3.0, 0.0};
    case Interpolation::Bezier: {
        const double influence = std::clamp(a.out.influence, 0.0, 1.0);
        return {influence, a.out.slope * influence * seconds};
    }
    default:
        return {1.0 / 3.0, (b.value - a.value) / 3.0};
    }
}

/// B's arriving side of the segment A -> B (the offset is subtracted from B's value).
Side arriving(const Keyframe& a, const Keyframe& b, double seconds) {
    switch (b.interpolation) {
    case Interpolation::EaseIn:
    case Interpolation::EaseInOut:
        return {1.0 / 3.0, 0.0};
    case Interpolation::Bezier: {
        const double influence = std::clamp(b.in.influence, 0.0, 1.0);
        return {influence, b.in.slope * influence * seconds};
    }
    default:
        return {1.0 / 3.0, (b.value - a.value) / 3.0};
    }
}

bool linearSide(Interpolation leavingOrArriving, bool leavingSide) {
    switch (leavingOrArriving) {
    case Interpolation::Linear:
    case Interpolation::Hold:
        return true;
    case Interpolation::EaseIn:
        return leavingSide; // flat only on arrival
    case Interpolation::EaseOut:
        return !leavingSide; // flat only on departure
    default:
        return false;
    }
}

double cubic(double p0, double p1, double p2, double p3, double s) {
    const double r = 1.0 - s;
    return r * r * r * p0 + 3.0 * r * r * s * p1 + 3.0 * r * s * s * p2 + s * s * s * p3;
}

} // namespace

std::string_view toString(Interpolation interpolation) {
    for (const auto& [value, name] : kInterpolations) {
        if (value == interpolation) {
            return name;
        }
    }
    return "linear";
}

std::optional<Interpolation> interpolationFromString(std::string_view text) {
    for (const auto& [value, name] : kInterpolations) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

std::size_t Curve::insertionIndex(Ticks time) const {
    return static_cast<std::size_t>(
        std::lower_bound(keys.begin(), keys.end(), time, [](const Keyframe& k, Ticks t) { return k.time < t; }) -
        keys.begin());
}

std::optional<std::size_t> Curve::keyAt(Ticks time) const {
    const std::size_t i = insertionIndex(time);
    if (i < keys.size() && keys[i].time == time) {
        return i;
    }
    return std::nullopt;
}

double Curve::valueAt(Ticks time) const {
    if (keys.empty()) {
        return 0.0;
    }
    if (time <= keys.front().time) {
        return keys.front().value;
    }
    if (time >= keys.back().time) {
        return keys.back().value;
    }
    // keys[i] is the last key at or before `time`; keys[i + 1] lies after it.
    const std::size_t next = static_cast<std::size_t>(
        std::upper_bound(keys.begin(), keys.end(), time, [](Ticks t, const Keyframe& k) { return t < k.time; }) -
        keys.begin());
    const Keyframe& a = keys[next - 1];
    const Keyframe& b = keys[next];
    if (time == a.time || a.interpolation == Interpolation::Hold) {
        return a.value;
    }
    // The only step from integer time to floating point: the position inside the segment.
    const std::int64_t length = (b.time - a.time).value;
    const double u = static_cast<double>((time - a.time).value) / static_cast<double>(length);
    if (linearSide(a.interpolation, true) && linearSide(b.interpolation, false)) {
        return a.value + (b.value - a.value) * u;
    }
    const double seconds = static_cast<double>(length) / static_cast<double>(kTicksPerSecond);
    const Side out = leaving(a, b, seconds);
    const Side in = arriving(a, b, seconds);
    // Time controls inside [0, 1] keep x(s) monotonic, so bisection finds the unique s with x(s) = u.
    const double x1 = out.influence;
    const double x2 = 1.0 - in.influence;
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 64; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (cubic(0.0, x1, x2, 1.0, mid) < u) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const double s = 0.5 * (lo + hi);
    return cubic(a.value, a.value + out.delta, b.value - in.delta, b.value, s);
}

bool Curve::isWellFormed() const {
    if (keys.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const Keyframe& k = keys[i];
        if (i > 0 && !(keys[i - 1].time < k.time)) {
            return false;
        }
        for (const BezierHandle& h : {k.in, k.out}) {
            if (!std::isfinite(h.slope) || !std::isfinite(h.influence) || h.influence < 0.0 || h.influence > 1.0) {
                return false;
            }
        }
    }
    return true;
}

bool ParamValue::isValid(double minValue, double maxValue) const {
    const auto inRange = [&](double v) { return std::isfinite(v) && v >= minValue && v <= maxValue; };
    if (const Curve* c = curve()) {
        return c->isWellFormed() && std::all_of(c->keys.begin(), c->keys.end(), [&](const Keyframe& k) { return inRange(k.value); });
    }
    return inRange(std::get<double>(data));
}

bool ClipProperties::isDefault(ParamId id) const {
    const ParamValue& v = (*this)[id];
    return v.isConstant() && v.constant() == parameterInfo(id).defaultValue;
}

PropertyRoles ClipProperties::differences(const ClipProperties& other) const {
    PropertyRoles roles;
    for (const ParameterInfo& p : kParameters) {
        if (!((*this)[p.id] == other[p.id])) {
            roles |= p.role;
        }
    }
    return roles;
}

std::optional<ParamId> ClipProperties::firstInvalid() const {
    for (const ParameterInfo& p : kParameters) {
        if (!(*this)[p.id].isValid(p.minValue, p.maxValue)) {
            return p.id;
        }
    }
    return std::nullopt;
}

} // namespace wabcut
