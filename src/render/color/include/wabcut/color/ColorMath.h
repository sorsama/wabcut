#pragma once

// Small colour helpers shared by the grade, the scopes and the colour tools (docs/color.md). Double precision; the
// shaders mirror the ones the grade uses (src/render/shaders/grade.frag).

#include <wabcut/color/Lut.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::color {

/// BT.709 OETF constants (the same as render::linearToOutput and output.frag).
inline constexpr double kRec709Alpha = 1.09929682680944;
inline constexpr double kRec709Beta = 0.018053968510807;

/// Linear light -> BT.709-encoded value. Extended: linear below 0 (4.5x) and the power segment above 1, so it is
/// monotone and invertible everywhere (the grade keeps values outside [0, 1]).
inline double encode709(double l) {
    return l < kRec709Beta ? 4.5 * l : kRec709Alpha * std::pow(l, 0.45) - (kRec709Alpha - 1.0);
}
/// Inverse of encode709.
inline double decode709(double v) {
    return v < 4.5 * kRec709Beta ? v / 4.5 : std::pow((v + (kRec709Alpha - 1.0)) / kRec709Alpha, 1.0 / 0.45);
}
inline Rgb encode709(const Rgb& l) { return {encode709(l[0]), encode709(l[1]), encode709(l[2])}; }
inline Rgb decode709(const Rgb& v) { return {decode709(v[0]), decode709(v[1]), decode709(v[2])}; }

/// Rec.709 luma weights (also the ASC CDL saturation weights).
inline constexpr Rgb kLuma709{0.2126, 0.7152, 0.0722};
inline double luma(const Rgb& c) { return kLuma709[0] * c[0] + kLuma709[1] * c[1] + kLuma709[2] * c[2]; }

inline Rgb operator+(const Rgb& a, const Rgb& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Rgb operator-(const Rgb& a, const Rgb& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Rgb operator*(const Rgb& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
inline Rgb operator*(const Rgb& a, const Rgb& b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2]}; }
inline Rgb mix(const Rgb& a, const Rgb& b, double t) { return a + (b - a) * t; }

inline double smoothstep(double e0, double e1, double x) {
    if (e1 <= e0) {
        return x < e0 ? 0.0 : 1.0;
    }
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// Hue (turns, [0, 1)), saturation (chroma / max, [0, 1]) and value of an RGB triple (HSV on the given values; values
/// below 0 count as 0). Grey has hue 0 and saturation 0.
struct Hsv {
    double h = 0.0;
    double s = 0.0;
    double v = 0.0;
};
Hsv toHsv(const Rgb& c);
/// A fully saturated colour of hue `turns` with value 1, minus its luma: the chroma direction of that hue.
Rgb hueChroma(double turns);

/// Rotates an RGB value's chroma about the grey axis by `turns` (Rec.709 luma kept).
Rgb rotateHue(const Rgb& c, double turns);

/// 3x3 matrix, row major.
using Mat3 = std::array<double, 9>;
Rgb mul(const Mat3& m, const Rgb& v);
Mat3 multiply(const Mat3& a, const Mat3& b);
Mat3 inverse(const Mat3& m);

/// The white balance of the grade as one matrix on linear Rec.709: a von Kries gain in Bradford cone space
/// (temperature: L up and S down; tint: M down = magenta), normalised so a neutral keeps its luminance.
/// `temperature` and `tint` are the sliders (-100..100).
Mat3 whiteBalanceMatrix(double temperature, double tint);

// ---- Oklab (Björn Ottosson, 2020; public domain / MIT) on linear Rec.709 / sRGB primaries ----

Rgb linearToOklab(const Rgb& c);
Rgb oklabToLinear(const Rgb& lab);

} // namespace wabcut::color
