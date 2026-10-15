// HDR game captures tone-mapped into SDR (Hdr.h).

#include <wabcut/color/Hdr.h>

#include <algorithm>
#include <cmath>

namespace wabcut::color {

namespace {

constexpr double kPqM1 = 2610.0 / 16384.0;
constexpr double kPqM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kPqC1 = 3424.0 / 4096.0;
constexpr double kPqC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kPqC3 = 2392.0 / 4096.0 * 32.0;
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892; // 1 - 4a
constexpr double kHlgC = 0.55991073; // 0.5 - a ln(4a)

double lumaRec2020(const Rgb3& c) { return 0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2]; }

Matrix3 mul(const Matrix3& a, const Matrix3& b) {
    Matrix3 r{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            double s = 0.0;
            for (std::size_t k = 0; k < 3; ++k) {
                s += a[i * 3 + k] * b[k * 3 + j];
            }
            r[i * 3 + j] = s;
        }
    }
    return r;
}

Matrix3 inverse(const Matrix3& m) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    const double k = det != 0.0 ? 1.0 / det : 0.0;
    return {A * k, -(b * i - c * h) * k, (b * f - c * e) * k, B * k, (a * i - c * g) * k, -(a * f - c * d) * k,
            C * k, -(a * h - b * g) * k, (a * e - b * d) * k};
}

struct Chromaticities {
    double rx, ry, gx, gy, bx, by, wx, wy;
};
constexpr Chromaticities kRec709{0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290};
constexpr Chromaticities kRec2020{0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};

Rgb3 xyzOf(double x, double y) { return {x / y, 1.0, (1.0 - x - y) / y}; }

Matrix3 rgbToXyz(const Chromaticities& c) {
    const Rgb3 r = xyzOf(c.rx, c.ry), g = xyzOf(c.gx, c.gy), b = xyzOf(c.bx, c.by);
    const Matrix3 p{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const Rgb3 s = color::apply(inverse(p), xyzOf(c.wx, c.wy));
    return {p[0] * s[0], p[1] * s[1], p[2] * s[2], p[3] * s[0], p[4] * s[1], p[5] * s[2], p[6] * s[0], p[7] * s[1], p[8] * s[2]};
}

} // namespace

Rgb3 apply(const Matrix3& m, const Rgb3& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

double pqToNits(double code) {
    const double e = std::pow(std::clamp(code, 0.0, 1.0), 1.0 / kPqM2);
    return 10000.0 * std::pow(std::max(e - kPqC1, 0.0) / (kPqC2 - kPqC3 * e), 1.0 / kPqM1);
}

double nitsToPq(double nits) {
    const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), kPqM1);
    return std::pow((kPqC1 + kPqC2 * y) / (1.0 + kPqC3 * y), kPqM2);
}

double hlgOetf(double e) {
    e = std::max(e, 0.0);
    return e <= 1.0 / 12.0 ? std::sqrt(3.0 * e) : kHlgA * std::log(12.0 * e - kHlgB) + kHlgC;
}

double hlgInverseOetf(double v) {
    v = std::max(v, 0.0);
    return v <= 0.5 ? v * v / 3.0 : (std::exp((v - kHlgC) / kHlgA) + kHlgB) / 12.0;
}

double hlgSystemGamma(double peakNits) { return 1.2 + 0.42 * std::log10(std::max(peakNits, 1.0) / 1000.0); }

Rgb3 hlgToDisplayNits(const Rgb3& signal, double peakNits) {
    const Rgb3 scene{hlgInverseOetf(signal[0]), hlgInverseOetf(signal[1]), hlgInverseOetf(signal[2])};
    const double ys = lumaRec2020(scene);
    const double k = ys > 0.0 ? peakNits * std::pow(ys, hlgSystemGamma(peakNits) - 1.0) : 0.0;
    return {scene[0] * k, scene[1] * k, scene[2] * k};
}

Rgb3 toneMapToSdr(const Rgb3& rec2020, double referenceWhiteNits, double peakNits) {
    const Rgb3 c{std::max(rec2020[0], 0.0), std::max(rec2020[1], 0.0), std::max(rec2020[2], 0.0)};
    const double y = lumaRec2020(c);
    double ratio = 1.0;
    if (y > 0.0 && peakNits > referenceWhiteNits) {
        // BT.2390 EETF in PQ, the source range [0, peak] mastered onto [0, reference white].
        const double nits = y * referenceWhiteNits;
        const double pqPeak = nitsToPq(peakNits);
        const double e1 = std::min(nitsToPq(nits) / pqPeak, 1.0);
        const double maxLum = nitsToPq(referenceWhiteNits) / pqPeak;
        const double ks = 1.5 * maxLum - 0.5;
        double e2 = e1;
        if (e1 >= ks && ks < 1.0) {
            const double t = (e1 - ks) / (1.0 - ks);
            const double t2 = t * t, t3 = t2 * t;
            e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1.0 - ks) + (-2 * t3 + 3 * t2) * maxLum;
        }
        ratio = nits > peakNits ? referenceWhiteNits / nits // above the master's peak: held at SDR white
                                : pqToNits(e2 * pqPeak) / nits;
    }
    const Rgb3 r709 = color::apply(rec2020ToRec709(), {c[0] * ratio, c[1] * ratio, c[2] * ratio});
    return {std::clamp(r709[0], 0.0, 1.0), std::clamp(r709[1], 0.0, 1.0), std::clamp(r709[2], 0.0, 1.0)};
}

std::optional<Rgb3> hdrCaptureToSdr(TransferFunction transfer, const Rgb3& code) {
    const Rgb3 c{std::clamp(code[0], 0.0, 1.0), std::clamp(code[1], 0.0, 1.0), std::clamp(code[2], 0.0, 1.0)};
    Rgb3 nits;
    if (transfer == TransferFunction::PQ) {
        nits = {pqToNits(c[0]), pqToNits(c[1]), pqToNits(c[2])};
    } else if (transfer == TransferFunction::HLG) {
        nits = hlgToDisplayNits(c, kCapturePeakNits);
    } else {
        return std::nullopt;
    }
    const double white = kCaptureReferenceWhiteNits;
    return toneMapToSdr({nits[0] / white, nits[1] / white, nits[2] / white}, white, kCapturePeakNits);
}

Matrix3 rec2020ToRec709() {
    static const Matrix3 m = mul(inverse(rgbToXyz(kRec709)), rgbToXyz(kRec2020)); // same white: no adaptation
    return m;
}

} // namespace wabcut::color
