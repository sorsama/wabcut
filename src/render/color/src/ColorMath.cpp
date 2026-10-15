#include <wabcut/color/ColorMath.h>

#include <cmath>
#include <numbers>

namespace wabcut::color {

Hsv toHsv(const Rgb& c) {
    const double r = std::max(c[0], 0.0), g = std::max(c[1], 0.0), b = std::max(c[2], 0.0);
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    const double d = mx - mn;
    Hsv out;
    out.v = mx;
    out.s = mx > 1e-12 ? std::min(d / mx, 1.0) : 0.0; // clamped: GPUs may divide inexactly
    if (d <= 1e-12) {
        return out;
    }
    double h = 0.0;
    if (mx == r) {
        h = (g - b) / d;
        if (h < 0.0) {
            h += 6.0;
        }
    } else if (mx == g) {
        h = (b - r) / d + 2.0;
    } else {
        h = (r - g) / d + 4.0;
    }
    out.h = h / 6.0;
    if (out.h >= 1.0) {
        out.h -= 1.0;
    }
    return out;
}

Rgb hueChroma(double turns) {
    // HSV with s = v = 1.
    double h = turns - std::floor(turns);
    h *= 6.0;
    const double x = 1.0 - std::abs(std::fmod(h, 2.0) - 1.0);
    Rgb c{};
    if (h < 1) {
        c = {1, x, 0};
    } else if (h < 2) {
        c = {x, 1, 0};
    } else if (h < 3) {
        c = {0, 1, x};
    } else if (h < 4) {
        c = {0, x, 1};
    } else if (h < 5) {
        c = {x, 0, 1};
    } else {
        c = {1, 0, x};
    }
    const double y = luma(c);
    return {c[0] - y, c[1] - y, c[2] - y};
}

Rgb rotateHue(const Rgb& c, double turns) {
    // Rodrigues rotation about the (1, 1, 1) axis, then the luma is restored.
    const double a = turns * 2.0 * std::numbers::pi;
    const double cosA = std::cos(a), sinA = std::sin(a);
    const double k = 1.0 / std::sqrt(3.0);
    const Rgb axis{k, k, k};
    const double dot = (c[0] + c[1] + c[2]) * k;
    const Rgb cross{axis[1] * c[2] - axis[2] * c[1], axis[2] * c[0] - axis[0] * c[2], axis[0] * c[1] - axis[1] * c[0]};
    Rgb r{};
    for (std::size_t i = 0; i < 3; ++i) {
        r[i] = c[i] * cosA + cross[i] * sinA + axis[i] * dot * (1.0 - cosA);
    }
    const double dy = luma(c) - luma(r);
    return {r[0] + dy, r[1] + dy, r[2] + dy};
}

Rgb mul(const Mat3& m, const Rgb& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) {
                s += a[static_cast<std::size_t>(r * 3 + k)] * b[static_cast<std::size_t>(k * 3 + c)];
            }
            out[static_cast<std::size_t>(r * 3 + c)] = s;
        }
    }
    return out;
}

Mat3 inverse(const Mat3& m) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    const double k = std::abs(det) > 1e-300 ? 1.0 / det : 0.0;
    return {A * k,
            -(b * i - c * h) * k,
            (b * f - c * e) * k,
            B * k,
            (a * i - c * g) * k,
            -(a * f - c * d) * k,
            C * k,
            -(a * h - b * g) * k,
            (a * e - b * d) * k};
}

namespace {

// Linear Rec.709 (D65) <-> CIE XYZ.
constexpr Mat3 kRgbToXyz{0.4123907992659595, 0.357584339383878,  0.1804807884018343,
                         0.21263900587151036, 0.715168678767756,  0.07219231536073371,
                         0.01933081871559185, 0.11919477979462599, 0.9505321522496606};
// Bradford cone response.
constexpr Mat3 kBradford{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};

} // namespace

Mat3 whiteBalanceMatrix(double temperature, double tint) {
    const double t = std::clamp(temperature, -100.0, 100.0) / 100.0;
    const double g = std::clamp(tint, -100.0, 100.0) / 100.0;
    // Warm = more long-wave, less short-wave response; magenta (positive tint) = less medium-wave.
    const Mat3 gains{std::exp2(0.25 * t), 0, 0, 0, std::exp2(-0.3 * g), 0, 0, 0, std::exp2(-0.5 * t)};
    const Mat3 toCone = multiply(kBradford, kRgbToXyz);
    Mat3 m = multiply(inverse(toCone), multiply(gains, toCone));
    // Keep a neutral's luminance: Y of the balanced white = 1.
    const Rgb white = mul(m, {1, 1, 1});
    const double y = kRgbToXyz[3] * white[0] + kRgbToXyz[4] * white[1] + kRgbToXyz[5] * white[2];
    if (y > 1e-9) {
        for (double& v : m) {
            v /= y;
        }
    }
    return m;
}

Rgb linearToOklab(const Rgb& c) {
    const double l = 0.4122214708 * c[0] + 0.5363325363 * c[1] + 0.0514459929 * c[2];
    const double m = 0.2119034982 * c[0] + 0.6806995451 * c[1] + 0.1073969566 * c[2];
    const double s = 0.0883024619 * c[0] + 0.2817188376 * c[1] + 0.6299787005 * c[2];
    const double l_ = std::cbrt(l), m_ = std::cbrt(m), s_ = std::cbrt(s);
    return {0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
            1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
            0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_};
}

Rgb oklabToLinear(const Rgb& lab) {
    const double l_ = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
    const double m_ = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
    const double s_ = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
    const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

} // namespace wabcut::color
