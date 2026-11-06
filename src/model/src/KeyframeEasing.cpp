#include <wabcut/model/KeyframeEasing.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::edit {

namespace {

// Control points as published by easings.net (https://easings.net/) and the CSS Easing Functions Level 1 keywords
// (https://www.w3.org/TR/css-easing-1/#cubic-bezier-easing-functions).
constexpr std::array<EasingPreset, 32> kPresets{{
    {"linear", "Linear", "Basic", 1.0 / 3.0, 1.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0},
    {"ease", "Ease", "Basic", 0.25, 0.1, 0.25, 1.0},
    {"easeIn", "Ease In", "Basic", 0.42, 0.0, 1.0, 1.0},
    {"easeOut", "Ease Out", "Basic", 0.0, 0.0, 0.58, 1.0},
    {"easeInOut", "Ease In Out", "Basic", 0.42, 0.0, 0.58, 1.0},
    {"easeInSine", "Ease In Sine", "Sine", 0.12, 0.0, 0.39, 0.0},
    {"easeOutSine", "Ease Out Sine", "Sine", 0.61, 1.0, 0.88, 1.0},
    {"easeInOutSine", "Ease In Out Sine", "Sine", 0.37, 0.0, 0.63, 1.0},
    {"easeInQuad", "Ease In Quad", "Quad", 0.11, 0.0, 0.5, 0.0},
    {"easeOutQuad", "Ease Out Quad", "Quad", 0.5, 1.0, 0.89, 1.0},
    {"easeInOutQuad", "Ease In Out Quad", "Quad", 0.45, 0.0, 0.55, 1.0},
    {"easeInCubic", "Ease In Cubic", "Cubic", 0.32, 0.0, 0.67, 0.0},
    {"easeOutCubic", "Ease Out Cubic", "Cubic", 0.33, 1.0, 0.68, 1.0},
    {"easeInOutCubic", "Ease In Out Cubic", "Cubic", 0.65, 0.0, 0.35, 1.0},
    {"easeInQuart", "Ease In Quart", "Quart", 0.5, 0.0, 0.75, 0.0},
    {"easeOutQuart", "Ease Out Quart", "Quart", 0.25, 1.0, 0.5, 1.0},
    {"easeInOutQuart", "Ease In Out Quart", "Quart", 0.76, 0.0, 0.24, 1.0},
    {"easeInQuint", "Ease In Quint", "Quint", 0.64, 0.0, 0.78, 0.0},
    {"easeOutQuint", "Ease Out Quint", "Quint", 0.22, 1.0, 0.36, 1.0},
    {"easeInOutQuint", "Ease In Out Quint", "Quint", 0.83, 0.0, 0.17, 1.0},
    {"easeInExpo", "Ease In Expo", "Expo", 0.7, 0.0, 0.84, 0.0},
    {"easeOutExpo", "Ease Out Expo", "Expo", 0.16, 1.0, 0.3, 1.0},
    {"easeInOutExpo", "Ease In Out Expo", "Expo", 0.87, 0.0, 0.13, 1.0},
    {"easeInCirc", "Ease In Circ", "Circ", 0.55, 0.0, 1.0, 0.45},
    {"easeOutCirc", "Ease Out Circ", "Circ", 0.0, 0.55, 0.45, 1.0},
    {"easeInOutCirc", "Ease In Out Circ", "Circ", 0.85, 0.0, 0.15, 1.0},
    {"easeInBack", "Ease In Back", "Back", 0.36, 0.0, 0.66, -0.56},
    {"easeOutBack", "Ease Out Back", "Back", 0.34, 1.56, 0.64, 1.0},
    {"easeInOutBack", "Ease In Out Back", "Back", 0.68, -0.6, 0.32, 1.6},
    // Whip pans and punch-ins: a hard start that lands softly, and the reverse (not in the web tables).
    {"fastOutSlowIn", "Fast Out Slow In", "Basic", 0.4, 0.0, 0.2, 1.0},
    {"slowOutFastIn", "Slow Out Fast In", "Basic", 0.8, 0.0, 0.6, 1.0},
    {"snap", "Snap", "Basic", 0.9, 0.0, 0.1, 1.0},
}};

double cubic(double p0, double p1, double p2, double p3, double s) {
    const double r = 1.0 - s;
    return r * r * r * p0 + 3.0 * r * r * s * p1 + 3.0 * r * s * s * p2 + s * s * s * p3;
}

constexpr double kMinInfluence = 0.01; ///< a handle never sits exactly on its key (it could not be grabbed)

/// The handle that reproduces the side of key `i` towards its neighbour as its interpolation draws it now.
BezierHandle sideOf(const Curve& curve, std::size_t i, bool out) {
    const Keyframe& k = curve.keys[i];
    const bool exists = out ? i + 1 < curve.keys.size() : i > 0;
    if (!exists) {
        return BezierHandle{};
    }
    if (k.interpolation == Interpolation::Bezier) {
        return out ? k.out : k.in;
    }
    const Keyframe& other = curve.keys[out ? i + 1 : i - 1];
    const double seconds = std::abs((other.time - k.time).toSeconds());
    const double secant = seconds > 0.0 ? (out ? other.value - k.value : k.value - other.value) / seconds : 0.0;
    const bool flat = k.interpolation == Interpolation::EaseInOut ||
                      (out ? k.interpolation == Interpolation::EaseOut : k.interpolation == Interpolation::EaseIn);
    return BezierHandle{flat ? 0.0 : secant, 1.0 / 3.0};
}

/// Makes key `i` a Bezier key that draws exactly what it drew (both sides as handles).
void promote(Curve& curve, std::size_t i) {
    Keyframe& k = curve.keys[i];
    if (k.interpolation == Interpolation::Bezier) {
        return;
    }
    const BezierHandle in = sideOf(curve, i, false);
    const BezierHandle out = sideOf(curve, i, true);
    k.in = in;
    k.out = out;
    k.interpolation = Interpolation::Bezier;
}

} // namespace

std::span<const EasingPreset> easingPresets() {
    return kPresets;
}

const EasingPreset* findEasingPreset(std::string_view id) {
    for (const EasingPreset& p : kPresets) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

double easingValue(const EasingPreset& preset, double u) {
    if (!(u > 0.0)) {
        return 0.0;
    }
    if (!(u < 1.0)) {
        return 1.0;
    }
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 48; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (cubic(0.0, preset.x1, preset.x2, 1.0, mid) < u) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return cubic(0.0, preset.y1, preset.y2, 1.0, 0.5 * (lo + hi));
}

bool applyEasing(Curve& curve, std::size_t index, const EasingPreset& preset) {
    if (index + 1 >= curve.keys.size()) {
        return false;
    }
    const double seconds = (curve.keys[index + 1].time - curve.keys[index].time).toSeconds();
    if (!(seconds > 0.0)) {
        return false;
    }
    const double change = curve.keys[index + 1].value - curve.keys[index].value;
    const double x1 = std::clamp(preset.x1, kMinInfluence, 1.0);
    const double x2 = std::clamp(preset.x2, 0.0, 1.0 - kMinInfluence);
    promote(curve, index);
    Keyframe& a = curve.keys[index];
    a.out = BezierHandle{(preset.y1 * change) / (x1 * seconds), x1};
    // A Hold key stays a step: as the arriving key it cannot take a handle.
    if (curve.keys[index + 1].interpolation == Interpolation::Hold) {
        return true;
    }
    promote(curve, index + 1);
    Keyframe& b = curve.keys[index + 1];
    const double arrive = 1.0 - x2;
    b.in = BezierHandle{((1.0 - preset.y2) * change) / (arrive * seconds), arrive};
    return true;
}

std::size_t applyEasing(Curve& curve, std::span<const std::size_t> indices, const EasingPreset& preset) {
    std::size_t done = 0;
    for (const std::size_t i : indices) {
        done += applyEasing(curve, i, preset) ? 1 : 0;
    }
    return done;
}

} // namespace wabcut::edit
