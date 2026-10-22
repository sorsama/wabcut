#include <wabcut/render/reference/LibraryEffects.h>
#include <wabcut/render/reference/Transitions.h>

#include "LibraryInternal.h"

#include <algorithm>
#include <cmath>

namespace wabcut::render::reference {

namespace {

using E = LibraryEffect;

constexpr double kA709 = 1.09929682680944;
constexpr double kPi = 3.14159265358979323846;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double smoothstep(double e0, double e1, double x) {
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}
double mixD(double a, double b, double t) { return a + (b - a) * t; }

double oetfExt(double l) { return l <= 1.0 ? toEncoded(l) : kA709 * std::pow(l, 0.45) - (kA709 - 1.0); }
double eotfExt(double v) { return v <= 1.0 ? toLinear(v) : std::pow((v + kA709 - 1.0) / kA709, 1.0 / 0.45); }

Rgb hls(const Rgb& c) {
    const double mx = std::max({c[0], c[1], c[2]});
    const double mn = std::min({c[0], c[1], c[2]});
    const double l = 0.5 * (mx + mn);
    const double d = mx - mn;
    if (d <= 0.0) {
        return {0.0, l, 0.0};
    }
    const double s = l < 0.5 ? d / (mx + mn) : d / (2.0 - mx - mn);
    double h = 0.0;
    if (mx == c[0]) {
        h = (c[1] - c[2]) / d + (c[1] < c[2] ? 6.0 : 0.0);
    } else if (mx == c[1]) {
        h = (c[2] - c[0]) / d + 2.0;
    } else {
        h = (c[0] - c[1]) / d + 4.0;
    }
    return {h / 6.0, l, s};
}

double hueChannel(double m1, double m2, double h) {
    h = h - std::floor(h);
    if (h < 1.0 / 6.0) {
        return m1 + (m2 - m1) * 6.0 * h;
    }
    if (h < 0.5) {
        return m2;
    }
    if (h < 2.0 / 3.0) {
        return m1 + (m2 - m1) * (2.0 / 3.0 - h) * 6.0;
    }
    return m1;
}

Rgb fromHls(const Rgb& v) {
    const double l = v[1];
    const double s = v[2];
    if (s <= 0.0) {
        return {l, l, l};
    }
    const double m2 = l <= 0.5 ? l * (1.0 + s) : l + s - l * s;
    const double m1 = 2.0 * l - m2;
    return {hueChannel(m1, m2, v[0] + 1.0 / 3.0), hueChannel(m1, m2, v[0]), hueChannel(m1, m2, v[0] - 1.0 / 3.0)};
}

double hueDelta(double a, double b) {
    const double d = b - a;
    return d - std::floor(d + 0.5);
}

std::array<double, 2> chroma(const Rgb& c) {
    const double y = luma(c);
    return {(c[2] - y) / 1.8556, (c[0] - y) / 1.5748};
}

double colorDistance(const Rgb& c, const Rgb& k, int match) {
    if (match == 1) {
        return 2.0 * std::abs(hueDelta(hls(c)[0], hls(k)[0]));
    }
    if (match == 2) {
        const auto a = chroma(c);
        const auto b = chroma(k);
        return std::hypot(a[0] - b[0], a[1] - b[1]);
    }
    return std::sqrt((c[0] - k[0]) * (c[0] - k[0]) + (c[1] - k[1]) * (c[1] - k[1]) + (c[2] - k[2]) * (c[2] - k[2])) /
           1.7320508;
}

Rgb clipColor(Rgb c) {
    const double l = luma(c);
    const double n = std::min({c[0], c[1], c[2]});
    const double x = std::max({c[0], c[1], c[2]});
    if (n < 0.0 && l - n > 0.0) {
        for (double& v : c) {
            v = l + (v - l) * l / (l - n);
        }
    }
    if (x > 1.0 && x - l > 0.0) {
        for (double& v : c) {
            v = l + (v - l) * (1.0 - l) / (x - l);
        }
    }
    return c;
}

Rgb setLum(const Rgb& c, double l) {
    const double d = l - luma(c);
    return clipColor({c[0] + d, c[1] + d, c[2] + d});
}

double levelsOf(double v, double ib, double iw, double ob, double ow, double gamma) {
    ib /= 255.0;
    iw /= 255.0;
    ob /= 255.0;
    ow /= 255.0;
    const double x = iw > ib ? clamp01((v - ib) / (iw - ib)) : (v >= ib ? 1.0 : 0.0);
    return ob + (ow - ob) * std::pow(x, 100.0 / std::max(gamma, 1.0));
}

Rgb yiqOf(const Rgb& c) {
    return {0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2], 0.595716 * c[0] - 0.274453 * c[1] - 0.321263 * c[2],
            0.211456 * c[0] - 0.522591 * c[1] + 0.311135 * c[2]};
}

Rgb fromYiq(const Rgb& y) {
    return {y[0] + 0.9563 * y[1] + 0.6210 * y[2], y[0] - 0.2721 * y[1] - 0.6474 * y[2], y[0] - 1.1070 * y[1] + 1.7046 * y[2]};
}

double limitOf(double v, double level, double knee, double k) {
    if (k > 0.0 && v > knee) {
        return knee + (level - knee) * (1.0 - std::exp(-(v - knee) / (level - knee)));
    }
    return std::min(v, level);
}

double strobeOperator(int op, double e, double c) {
    switch (op) {
    case 0: return c;
    case 1: return std::min(1.0, e + c);
    case 2: return std::max(0.0, e - c);
    case 3: return e * c;
    case 4: return std::abs(e - c);
    case 5:
    case 6:
    case 7: {
        const auto a = static_cast<std::uint32_t>(std::clamp(std::floor(e * 255.0 + 0.5), 0.0, 255.0));
        const auto b = static_cast<std::uint32_t>(std::clamp(std::floor(c * 255.0 + 0.5), 0.0, 255.0));
        const std::uint32_t r = op == 5 ? (a & b) : op == 6 ? (a | b) : (a ^ b);
        return r / 255.0;
    }
    case 10: return std::min(e, c);
    case 11: return std::max(e, c);
    case 12: return 1.0 - (1.0 - e) * (1.0 - c);
    default: return e;
    }
}

Rgb clamp3(const Rgb& c) { return {clamp01(c[0]), clamp01(c[1]), clamp01(c[2])}; }
Rgb mix3(const Rgb& a, const Rgb& b, double t) { return {mixD(a[0], b[0], t), mixD(a[1], b[1], t), mixD(a[2], b[2], t)}; }

/// A Color-family op on one pixel: `c` the premultiplied linear input, `x` its frame position as a fraction of the width,
/// (px, py) the pixel. Returns the premultiplied linear result.
Rgba colorOp(E op, const LibraryParams& q, const Rgba& c, double x, int px, int py) {
    const double a = clamp01(c[3]);
    Rgb e{};
    if (a > 0.0) {
        e = {toEncoded(c[0] / a), toEncoded(c[1] / a), toEncoded(c[2] / a)};
    }
    Rgb r = e;
    double ra = a;
    switch (op) {
    case E::BrightnessContrast: {
        const double b = q[0] / 200.0;
        double k = std::clamp(q[1] / 100.0, -1.0, 1.0);
        k = (1.0 + k) / (1.0 - 0.99 * k);
        for (std::size_t i = 0; i < 3; ++i) {
            r[i] = clamp01((e[i] + b - 0.5) * k + 0.5);
        }
        break;
    }
    case E::Levels:
        for (std::size_t i = 0; i < 3; ++i) {
            const std::size_t o = 5 * (i + 1);
            const double v = levelsOf(e[i], q[o], q[o + 1], q[o + 2], q[o + 3], q[o + 4]);
            r[i] = levelsOf(v, q[0], q[1], q[2], q[3], q[4]);
        }
        break;
    case E::ProcAmp: {
        const bool original = q[4] > 0.5 && x >= q[5] * 1.0; // x: frame position as a fraction of the width
        if (!original) {
            const double luminance = luma(e) * q[1] / 100.0 + q[0] / 200.0;
            const auto cc = chroma(e);
            const double h = q[2] * kPi / 180.0;
            const double cb = (cc[0] * std::cos(h) - cc[1] * std::sin(h)) * (q[3] / 100.0);
            const double cr = (cc[0] * std::sin(h) + cc[1] * std::cos(h)) * (q[3] / 100.0);
            const double red = luminance + 1.5748 * cr;
            const double blue = luminance + 1.8556 * cb;
            r = clamp3({red, (luminance - 0.2126 * red - 0.0722 * blue) / 0.7152, blue});
        }
        break;
    }
    case E::Extract: {
        const double yy = luma(e) * 255.0;
        const double s = q[2] / 100.0 * 64.0;
        double m = s > 0.0 ? clamp01((yy - q[0] + s) / s) * clamp01((q[1] + s - yy) / s) : (yy >= q[0] && yy <= q[1] ? 1.0 : 0.0);
        if (q[3] > 0.5) {
            m = 1.0 - m;
        }
        r = {m, m, m};
        break;
    }
    case E::Invert: {
        const int ch = static_cast<int>(std::floor(q[0] + 0.5));
        const Rgb h = hls(e);
        const Rgb yiq = yiqOf(e);
        double alpha = a;
        if (ch == 0) {
            r = {1.0 - e[0], 1.0 - e[1], 1.0 - e[2]};
        } else if (ch >= 1 && ch <= 3) {
            r[static_cast<std::size_t>(ch - 1)] = 1.0 - e[static_cast<std::size_t>(ch - 1)];
        } else if (ch == 4) {
            r = fromHls({h[0] + 0.5, 1.0 - h[1], 1.0 - h[2]});
        } else if (ch == 5) {
            r = fromHls({h[0] + 0.5, h[1], h[2]});
        } else if (ch == 6) {
            r = fromHls({h[0], 1.0 - h[1], h[2]});
        } else if (ch == 7) {
            r = fromHls({h[0], h[1], 1.0 - h[2]});
        } else if (ch == 8) {
            r = fromYiq({1.0 - yiq[0], -yiq[1], -yiq[2]});
        } else if (ch == 9) {
            r = fromYiq({1.0 - yiq[0], yiq[1], yiq[2]});
        } else if (ch == 10) {
            r = fromYiq({yiq[0], -yiq[1], yiq[2]});
        } else if (ch == 11) {
            r = fromYiq({yiq[0], yiq[1], -yiq[2]});
        } else {
            alpha = 1.0 - a;
        }
        r = mix3(clamp3(r), e, q[1]);
        ra = mixD(alpha, a, q[1]);
        break;
    }
    case E::Tint: {
        const double l = luma(e);
        const Rgb tinted{mixD(q[0], q[3], l), mixD(q[1], q[4], l), mixD(q[2], q[5], l)};
        r = mix3(e, tinted, q[6]);
        break;
    }
    case E::ChangeColor: {
        const double d = colorDistance(e, {q[4], q[5], q[6]}, static_cast<int>(std::floor(q[9] + 0.5)));
        double m = 1.0 - smoothstep(q[7], q[7] + q[8] + 1e-5, d);
        if (q[10] > 0.5) {
            m = 1.0 - m;
        }
        if (q[0] > 0.5) {
            r = {m, m, m};
        } else {
            Rgb h = hls(e);
            h[0] = h[0] + q[1] / 360.0 * m;
            h[1] = clamp01(h[1] + q[2] / 100.0 * m);
            h[2] = clamp01(h[2] + q[3] / 100.0 * m);
            r = fromHls(h);
        }
        break;
    }
    case E::ChangeToColor: {
        const Rgb from = hls({q[0], q[1], q[2]});
        const Rgb to = hls({q[3], q[4], q[5]});
        Rgb h = hls(e);
        const double soft = 0.5 * q[11] + 1e-5;
        const double m = (1.0 - smoothstep(q[8], q[8] + soft, std::abs(hueDelta(h[0], from[0])))) *
                         (1.0 - smoothstep(q[9], q[9] + soft, std::abs(h[1] - from[1]))) *
                         (1.0 - smoothstep(q[10], q[10] + soft, std::abs(h[2] - from[2])));
        const int change = static_cast<int>(std::floor(q[6] + 0.5));
        const bool useL = change == 1 || change == 3;
        const bool useS = change == 2 || change == 3;
        if (q[12] > 0.5) {
            r = {m, m, m};
        } else {
            if (q[7] > 0.5) {
                h[0] += hueDelta(from[0], to[0]) * m;
                if (useL) {
                    h[1] = clamp01(h[1] + (to[1] - from[1]) * m);
                }
                if (useS) {
                    h[2] = clamp01(h[2] + (to[2] - from[2]) * m);
                }
            } else {
                h[0] += hueDelta(h[0], to[0]) * m;
                if (useL) {
                    h[1] = mixD(h[1], to[1], m);
                }
                if (useS) {
                    h[2] = mixD(h[2], to[2], m);
                }
            }
            r = fromHls(h);
        }
        break;
    }
    case E::LeaveColor: {
        const double d = colorDistance(e, {q[1], q[2], q[3]}, static_cast<int>(std::floor(q[6] + 0.5)));
        const double keep = 1.0 - smoothstep(q[4], q[4] + q[5] + 1e-5, d);
        const double l = luma(e);
        r = mix3(e, {l, l, l}, q[0] * (1.0 - keep));
        break;
    }
    case E::ColorBalance: {
        const double l = luma(e);
        const double ws = 1.0 - smoothstep(0.0, 0.5, l);
        const double wh = smoothstep(0.5, 1.0, l);
        const double wm = 1.0 - ws - wh;
        for (std::size_t i = 0; i < 3; ++i) {
            r[i] = clamp01(e[i] + (ws * q[i] + wm * q[3 + i] + wh * q[6 + i]) / 100.0 * 0.3);
        }
        if (q[9] > 0.5) {
            const double d = l - luma(r);
            r = clamp3({r[0] + d, r[1] + d, r[2] + d});
        }
        break;
    }
    case E::VideoLimiter: {
        const double level = 1.0 + std::floor(q[0] + 0.5) / 100.0;
        const int ci = static_cast<int>(std::floor(q[1] + 0.5));
        const double k = ci == 1 ? 0.03 : ci == 2 ? 0.05 : ci == 3 ? 0.10 : ci == 4 ? 0.20 : 0.0;
        const double knee = level * (1.0 - k);
        Rgb lin{};
        if (a > 0.0) {
            lin = {std::max(c[0] / a, 0.0), std::max(c[1] / a, 0.0), std::max(c[2] / a, 0.0)};
        }
        bool limited = false;
        Rgb outLin{};
        for (std::size_t i = 0; i < 3; ++i) {
            const double v = oetfExt(lin[i]);
            const double w = limitOf(v, level, knee, k);
            limited = limited || w != v;
            outLin[i] = eotfExt(w);
        }
        if (q[2] > 0.5 && limited) {
            outLin = {toLinear(q[3]), toLinear(q[4]), toLinear(q[5])};
        }
        return {outLin[0] * a, outLin[1] * a, outLin[2] * a, a};
    }
    case E::BlackWhite: {
        const double l = luma(e);
        r = {l, l, l};
        break;
    }
    case E::ColorPass: {
        const double sim = q[3] / 100.0;
        double keep = 1.0 - smoothstep(sim, sim + 0.004, colorDistance(e, {q[0], q[1], q[2]}, 0));
        if (q[4] > 0.5) {
            keep = 1.0 - keep;
        }
        const double l = luma(e);
        r = mix3({l, l, l}, e, keep);
        break;
    }
    case E::ColorReplace: {
        const double sim = q[6] / 100.0;
        const double m = 1.0 - smoothstep(sim, sim + 0.004, colorDistance(e, {q[0], q[1], q[2]}, 0));
        const Rgb replace{q[3], q[4], q[5]};
        r = mix3(e, q[7] > 0.5 ? replace : clamp3(setLum(replace, luma(e))), m);
        break;
    }
    case E::GammaCorrection:
        for (std::size_t i = 0; i < 3; ++i) {
            r[i] = std::pow(e[i], q[0] / 10.0);
        }
        break;
    case E::Posterize: {
        const double levels = std::max(2.0, std::floor(q[0] + 0.5));
        for (std::size_t i = 0; i < 3; ++i) {
            r[i] = std::min(std::floor(e[i] * levels), levels - 1.0) / (levels - 1.0);
        }
        break;
    }
    case E::Solarize:
        for (std::size_t i = 0; i < 3; ++i) {
            r[i] = e[i] > q[0] ? 1.0 - e[i] : e[i];
        }
        break;
    case E::Threshold: {
        const double v = luma(e) * 255.0 >= q[0] ? 1.0 : 0.0;
        r = {v, v, v};
        break;
    }
    case E::StrobeLight:
        if (q[10] > 0.5) {
            if (q[7] > 0.5) {
                ra = a * q[3];
            } else {
                const int sop = static_cast<int>(std::floor(q[8] + 0.5));
                const Rgb s{q[0], q[1], q[2]};
                Rgb o{};
                if (sop == 8 || sop == 9) {
                    const bool sLighter = luma(s) > luma(e);
                    o = (sop == 8) == sLighter ? s : e;
                } else {
                    for (std::size_t i = 0; i < 3; ++i) {
                        o[i] = strobeOperator(sop, e[i], s[i]);
                    }
                }
                r = mix3(o, e, q[3]);
            }
        }
        break;
    case E::AlphaAdjust: {
        double na = q[1] > 0.5 ? 1.0 : a;
        if (q[2] > 0.5) {
            na = 1.0 - na;
        }
        na *= q[0];
        if (q[3] > 0.5) {
            const double g = toLinear(na);
            return {g, g, g, 1.0};
        }
        if (a <= 0.0) {
            return {0.0, 0.0, 0.0, na};
        }
        return {c[0] / a * na, c[1] / a * na, c[2] / a * na, na};
    }
    case E::LumaKey: {
        const double l = luma(e);
        const double t = q[0];
        const double cut = q[1];
        const double k = t == cut ? (l >= t ? 1.0 : 0.0) : clamp01((l - cut) / (t - cut));
        ra = a * k;
        break;
    }
    case E::Noise: {
        const auto cell = static_cast<std::uint32_t>(px) + 65536u * static_cast<std::uint32_t>(py);
        const auto frame = static_cast<std::uint32_t>(std::max(0.0, std::floor(q[31] * 1000.0 + 0.5)));
        const bool color = q[1] > 0.5;
        for (std::uint32_t i = 0; i < 3; ++i) {
            const double n = libraryHash01(cell, frame, color ? i : 0u);
            const double v = e[i] + (n - 0.5) * q[0];
            r[i] = q[2] > 0.5 ? clamp01(v) : v - std::floor(v);
        }
        break;
    }
    default:
        break;
    }
    return {toLinear(r[0]) * ra, toLinear(r[1]) * ra, toLinear(r[2]) * ra, ra};
}

} // namespace

double libraryHash01(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const std::uint32_t h = transitionHash(a + transitionHash(b + transitionHash(c)));
    return static_cast<double>(h >> 8) * (1.0 / 16777216.0);
}

Image libraryEffect(const Image& in, LibraryEffect effect, const LibraryParams& params, const Image* second) {
    if (detail::isStylize(effect)) {
        return detail::stylize(in, effect, params);
    }
    if (detail::isGenerate(effect)) {
        return detail::generate(in, effect, params, second);
    }
    Image out(in.width, in.height);
    const bool distort = detail::isDistort(effect);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            const double fx = (x + 0.5) / std::max(in.width, 1); // frame position as a fraction of the width
            out.setD(x, y, distort ? detail::distortPixel(in, effect, params, x, y) : colorOp(effect, params, in.atD(x, y), fx, x, y));
        }
    }
    return out;
}

} // namespace wabcut::render::reference
