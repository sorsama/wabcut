// The Retouch effects' CPU twins (0.7): Skin Smoothing and AI Face Retouch, for render/reference/LibraryEffects.h. The same
// constants, tap layouts and formulas as shaders/fx_generate.frag (OP_SKIN_SMOOTHING, OP_FACE_RETOUCH), in double precision.
//
// Both work on the straight colour encoded with the BT.709 curve and change a pixel only by the difference they make, so a
// pixel they leave alone is returned exactly (also above 1.0 in linear light). Radii are in sequence pixels; here the
// picture is the canvas (divisor 1), so a pixel is a sequence pixel.
//
// Skin tone (skinTone): Y' and (Cb, Cr) of BT.709; the angle of (Cb, Cr) in degrees, measured from +Cb towards +Cr, within
// `halfWidth` of Skin Tone (the edge is softened by 3..20 degrees by Softness), times enough chroma (0.015..0.045 up, 0.16..0.26
// down: not grey, not a saturated primary) times a luma that is neither black nor white (0.08..0.2 up, 0.93..0.995 down).
// Half width = 8..45 degrees by Tone Range.
//
// Smoothing (smoothSkin): 24 taps on a Vogel disc (radius sqrt((i + 1/2) / 24) R, angle i 2.39996323) of R texels, snapped to
// texels; weight = exp(-2 (r / R)^2) (a Gaussian of sigma R / 2) x the tap's skin weight x exp(-|c - c0|^2 / (2 0.08^2)); the
// pixel is a tap of weight s0. The smoothed colour S is the weighted mean; the skin weight averaged over the disc (spatial
// weights only, the pixel included) softens the mask's edge: m = s0 smoothstep(0.15, 0.75, that). Result = mix(c0, S +
// Detail (c0 - S), Amount m). Skin Smoothing takes the skin weight from the tap's own colour; AI Face Retouch from the face
// map's red channel (m = the map's red at the pixel).
//
// Spot healing (healBlemish, Face Retouch): 16 taps on a disc of Blemish Size over the map's skin; the mean of those taps,
// then the mean again with each tap weighted by exp(-|c - mean|^2 / (2 0.08^2)) (a robust mean of the skin round the
// pixel); agree = smoothstep(0.30, 0.65, the second weights' sum over the first's) says how much of the skin agrees on one
// colour (a spot in clean skin leaves most of it, the edge of a feature leaves two colours), and b is multiplied by it. The pixel, averaged with its four neighbours (low), and the pixel itself both differ from it; dev is the smaller of
// |low - mean| and |c0 - mean|; the change is b = smoothstep(t0, t0 + 0.06, dev) (1 - smoothstep(0.30, 0.50, dev)), t0 =
// mix(0.10, 0.025, strength): small spots go, features stay, a pixel beside a feature is not taken for a spot. The pixel becomes
// mean + clamp(c0 - low, -0.04, 0.04) (its own fine texture kept, never an edge's worth) by b min(1, 1.5 strength), and the healed
// colour is mixed in by the pixel's own skin weight (the map's red) like the smoothing is: the real parser's skin probability fades
// over a few pixels beyond the edge of the head, and healing by the face's colour there smeared skin tone onto the background.
//
// Teeth: k = Teeth t_mouth smoothstep(0.30, 0.50, Y) (1 - smoothstep(0.35, 0.60, sat)) (sat = (max - min) / max): the colour moves
// 0.85 k to its luma, then 0.28 k towards white. Eyes: the same with the map's blue, smoothstep(0.35, 0.55, Y),
// (1 - smoothstep(0.30, 0.55, sat)), 0.6 and 0.25.

#include "LibraryInternal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::render::reference::detail {

namespace {

using E = LibraryEffect;

constexpr double kGolden = 2.39996323;
constexpr double kRangeSigma = 0.08;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double smoothstep(double e0, double e1, double x) {
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

Rgb encodedOf(const Rgba& c) {
    const double a = clamp01(c[3]);
    if (a <= 0.0) {
        return {};
    }
    return {toEncoded(c[0] / a), toEncoded(c[1] / a), toEncoded(c[2] / a)};
}

double lumaOf(const Rgb& c) { return luma(c); }

double skinTone(const Rgb& e, double tone, double halfWidth, double soft) {
    const double y = lumaOf(e);
    const double cb = (e[2] - y) / 1.8556;
    const double cr = (e[0] - y) / 1.5748;
    const double chroma = std::hypot(cb, cr);
    double angle = std::atan2(cr, cb) * 180.0 / 3.14159265358979;
    if (angle < 0.0) {
        angle += 360.0;
    }
    double d = std::abs(angle - tone);
    d = std::min(d, 360.0 - d);
    const double edge = 3.0 + (20.0 - 3.0) * soft;
    const double wHue = 1.0 - smoothstep(std::max(halfWidth - edge, 0.0), halfWidth + edge, d);
    const double wChroma = smoothstep(0.015, 0.045, chroma) * (1.0 - smoothstep(0.16, 0.26, chroma));
    const double wLuma = smoothstep(0.08, 0.2, y) * (1.0 - smoothstep(0.93, 0.995, y));
    return wHue * wChroma * wLuma;
}

/// The face map (the second picture's linear R, G, B) at the centre of the picture's texel (ix, iy): bilinear between the map's
/// own texel centres, clamped at its edges, which is the texel itself when the map has the picture's size.
Rgb mapAt(const Image& map, int ix, int iy, int width, int height) {
    const double x = (ix + 0.5) / width * map.width - 0.5;
    const double y = (iy + 0.5) / height * map.height - 0.5;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0;
    const double fy = y - y0;
    const auto at = [&](int cx, int cy, std::size_t channel) {
        return static_cast<double>(map.atD(std::clamp(cx, 0, map.width - 1), std::clamp(cy, 0, map.height - 1))[channel]);
    };
    Rgb out{};
    for (std::size_t k = 0; k < 3; ++k) {
        const double top = at(x0, y0, k) * (1.0 - fx) + at(x0 + 1, y0, k) * fx;
        const double bottom = at(x0, y0 + 1, k) * (1.0 - fx) + at(x0 + 1, y0 + 1, k) * fx;
        out[k] = clamp01(top * (1.0 - fy) + bottom * fy);
    }
    return out;
}

struct Context {
    const Image& in;
    const Image* map; ///< null: the skin weight comes from the tap's own colour
    double tone = 123.0;
    double halfWidth = 26.0;
    double soft = 0.5;

    double skinWeight(int cx, int cy, const Rgba& tap, const Rgb& e) const {
        if (tap[3] <= 0.0) {
            return 0.0;
        }
        return map != nullptr ? mapAt(*map, cx, cy, in.width, in.height)[0] : skinTone(e, tone, halfWidth, soft);
    }
};

struct Smoothed {
    Rgb colour{};
    double skin = 0.0;
};

Smoothed smoothSkin(const Context& cx, int x, int y, const Rgb& c0, double s0, double radius) {
    const Image& in = cx.in;
    Rgb num{c0[0] * s0, c0[1] * s0, c0[2] * s0};
    double den = s0;
    double skinSum = s0;
    double weightSum = 1.0;
    for (int i = 0; i < 24; ++i) {
        const double rr = std::sqrt((i + 0.5) / 24.0) * radius;
        const double th = i * kGolden;
        const int tx = static_cast<int>(std::clamp(std::floor(x + 0.5 + std::cos(th) * rr), 0.0, in.width - 1.0));
        const int ty = static_cast<int>(std::clamp(std::floor(y + 0.5 + std::sin(th) * rr), 0.0, in.height - 1.0));
        const Rgba tap = in.atD(tx, ty);
        const Rgb e = encodedOf(tap);
        const double s = cx.skinWeight(tx, ty, tap, e);
        const double ws = std::exp(-2.0 * (rr / radius) * (rr / radius));
        const double ws2 = ws * s;
        double dist2 = 0.0;
        for (std::size_t k = 0; k < 3; ++k) {
            dist2 += (e[k] - c0[k]) * (e[k] - c0[k]);
        }
        const double wr = ws2 * std::exp(-dist2 / (2.0 * kRangeSigma * kRangeSigma));
        for (std::size_t k = 0; k < 3; ++k) {
            num[k] += wr * e[k];
        }
        den += wr;
        skinSum += ws2;
        weightSum += ws;
    }
    Smoothed out;
    out.colour = den > 1e-6 ? Rgb{num[0] / den, num[1] / den, num[2] / den} : c0;
    out.skin = skinSum / weightSum;
    return out;
}

Rgb healBlemish(const Context& cx, int x, int y, const Rgb& c0, double strength, double radius) {
    const Image& in = cx.in;
    std::array<Rgb, 16> colors{};
    std::array<double, 16> weights{};
    Rgb sum1{};
    double tot1 = 0.0;
    for (int i = 0; i < 16; ++i) {
        const double rr = std::sqrt((i + 0.5) / 16.0) * radius;
        const double th = i * kGolden;
        const int tx = static_cast<int>(std::clamp(std::floor(x + 0.5 + std::cos(th) * rr), 0.0, in.width - 1.0));
        const int ty = static_cast<int>(std::clamp(std::floor(y + 0.5 + std::sin(th) * rr), 0.0, in.height - 1.0));
        const Rgba tap = in.atD(tx, ty);
        const Rgb e = encodedOf(tap);
        const double w = tap[3] > 0.0 ? mapAt(*cx.map, tx, ty, in.width, in.height)[0] : 0.0;
        colors[static_cast<std::size_t>(i)] = e;
        weights[static_cast<std::size_t>(i)] = w;
        for (std::size_t k = 0; k < 3; ++k) {
            sum1[k] += w * e[k];
        }
        tot1 += w;
    }
    if (tot1 < 1e-4) {
        return c0;
    }
    const Rgb mean1{sum1[0] / tot1, sum1[1] / tot1, sum1[2] / tot1};
    Rgb sum2{};
    double tot2 = 0.0;
    for (std::size_t i = 0; i < 16; ++i) {
        double d2 = 0.0;
        for (std::size_t k = 0; k < 3; ++k) {
            d2 += (colors[i][k] - mean1[k]) * (colors[i][k] - mean1[k]);
        }
        const double w = weights[i] * std::exp(-d2 / (2.0 * 0.08 * 0.08));
        for (std::size_t k = 0; k < 3; ++k) {
            sum2[k] += w * colors[i][k];
        }
        tot2 += w;
    }
    const Rgb mean = tot2 > 1e-4 ? Rgb{sum2[0] / tot2, sum2[1] / tot2, sum2[2] / tot2} : mean1;
    const double agree = smoothstep(0.30, 0.65, tot2 / tot1); // how much of the skin round the pixel agrees on one colour
    Rgb low = c0;
    constexpr std::array<std::array<int, 2>, 4> kOffsets{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
    for (const auto& off : kOffsets) {
        const int nx = std::clamp(x + off[0], 0, in.width - 1);
        const int ny = std::clamp(y + off[1], 0, in.height - 1);
        const Rgb e = encodedOf(in.atD(nx, ny));
        for (std::size_t k = 0; k < 3; ++k) {
            low[k] += e[k];
        }
    }
    for (std::size_t k = 0; k < 3; ++k) {
        low[k] /= 5.0;
    }
    double dev2 = 0.0;
    double own2 = 0.0;
    for (std::size_t k = 0; k < 3; ++k) {
        dev2 += (low[k] - mean[k]) * (low[k] - mean[k]);
        own2 += (c0[k] - mean[k]) * (c0[k] - mean[k]);
    }
    const double dev = std::min(std::sqrt(own2), std::sqrt(dev2)); // the pixel itself must differ too: a pixel next to a feature does not
    const double t0 = 0.10 + (0.025 - 0.10) * strength;
    const double b = smoothstep(t0, t0 + 0.06, dev) * (1.0 - smoothstep(0.30, 0.50, dev)) * agree;
    const double f = b * clamp01(strength * 1.5);
    Rgb out{};
    for (std::size_t k = 0; k < 3; ++k) {
        const double texture = std::clamp(c0[k] - low[k], -0.04, 0.04);
        out[k] = c0[k] + (mean[k] + texture - c0[k]) * f;
    }
    return out;
}

/// The pixel `in` with its colour moved from encoded `from` to encoded `to` (only the difference is applied).
Rgba delta(const Rgba& in, const Rgb& from, const Rgb& to) {
    if (from == to) {
        return in;
    }
    Rgba out = in;
    for (std::size_t k = 0; k < 3; ++k) {
        out[k] = in[k] + (toLinear(to[k]) - toLinear(from[k])) * in[3];
    }
    return out;
}

Image skinSmoothing(const Image& in, const LibraryParams& q) {
    const double amount = clamp01(q[0] / 100.0);
    const double detail = clamp01(q[1] / 100.0);
    const double radius = std::max(q[2], 0.0);
    Context cx{in, nullptr, q[3], 8.0 + (45.0 - 8.0) * clamp01(q[4] / 100.0), clamp01(q[5] / 100.0)};
    const int view = static_cast<int>(std::floor(q[6] + 0.5));
    if (view != 1 && (amount <= 0.0 || radius < 0.5)) {
        return in;
    }
    Image out(in.width, in.height);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            const Rgba px = in.atD(x, y);
            const Rgb c0 = encodedOf(px);
            const double s0 = px[3] > 0.0 ? skinTone(c0, cx.tone, cx.halfWidth, cx.soft) : 0.0;
            const Smoothed sm = smoothSkin(cx, x, y, c0, s0, std::max(radius, 0.5));
            const double m = s0 * smoothstep(0.15, 0.75, sm.skin);
            if (view == 1) {
                const double g = toLinear(m);
                out.setD(x, y, {g, g, g, 1.0});
                continue;
            }
            Rgb result{};
            for (std::size_t k = 0; k < 3; ++k) {
                const double smoothed = sm.colour[k] + detail * (c0[k] - sm.colour[k]);
                result[k] = c0[k] + (smoothed - c0[k]) * (amount * m);
            }
            out.setD(x, y, delta(px, c0, result));
        }
    }
    return out;
}

Image faceRetouch(const Image& in, const LibraryParams& q, const Image* map) {
    if (map == nullptr || map->empty()) {
        return in; // no face map yet: the picture as it is
    }
    const double smoothing = clamp01(q[0] / 100.0);
    const double detail = clamp01(q[1] / 100.0);
    const double radius = std::max(q[2], 0.0);
    const double blemish = clamp01(q[3] / 100.0);
    const double blemishRadius = std::max(q[4], 0.0);
    const double teeth = clamp01(q[5] / 100.0);
    const double eyes = clamp01(q[6] / 100.0);
    const int view = static_cast<int>(std::floor(q[7] + 0.5));
    Context cx{in, map};
    Image out(in.width, in.height);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            const Rgba px = in.atD(x, y);
            const Rgb m = mapAt(*map, x, y, in.width, in.height);
            if (view >= 1 && view <= 3) {
                const double g = toLinear(m[static_cast<std::size_t>(view - 1)]);
                out.setD(x, y, {g, g, g, 1.0});
                continue;
            }
            const bool doSkin = m[0] > 0.003 && px[3] > 0.0;
            const bool doBlemish = doSkin && blemish > 0.0 && blemishRadius >= 0.5;
            const bool doSmooth = doSkin && smoothing > 0.0 && radius >= 0.5;
            const bool doTeeth = teeth > 0.0 && m[1] > 0.003;
            const bool doEyes = eyes > 0.0 && m[2] > 0.003;
            if (!doBlemish && !doSmooth && !doTeeth && !doEyes) {
                out.setD(x, y, px);
                continue;
            }
            const Rgb c0 = encodedOf(px);
            Rgb c = c0;
            if (doBlemish) {
                const Rgb healed = healBlemish(cx, x, y, c, blemish, blemishRadius);
                for (std::size_t k = 0; k < 3; ++k) {
                    c[k] = c[k] + (healed[k] - c[k]) * m[0]; // by the pixel's own skin weight: a map's soft tail beyond the edge heals nothing
                }
            }
            if (doSmooth) {
                const Smoothed sm = smoothSkin(cx, x, y, c, m[0], radius);
                for (std::size_t k = 0; k < 3; ++k) {
                    const double smoothed = sm.colour[k] + detail * (c[k] - sm.colour[k]);
                    c[k] = c[k] + (smoothed - c[k]) * (smoothing * m[0]);
                }
            }
            const double yl = lumaOf(c);
            const double mx = std::max(c[0], std::max(c[1], c[2]));
            const double sat = mx > 0.0 ? (mx - std::min(c[0], std::min(c[1], c[2]))) / mx : 0.0;
            if (doTeeth) {
                const double k = teeth * m[1] * smoothstep(0.30, 0.50, yl) * (1.0 - smoothstep(0.35, 0.60, sat));
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    const double t = c[ch] + (yl - c[ch]) * (0.85 * k);
                    c[ch] = t + (1.0 - t) * (0.28 * k);
                }
            }
            if (doEyes) {
                const double k = eyes * m[2] * smoothstep(0.35, 0.55, yl) * (1.0 - smoothstep(0.30, 0.55, sat));
                for (std::size_t ch = 0; ch < 3; ++ch) { // luma and saturation as they were before the teeth step (as the shader)
                    const double t = c[ch] + (yl - c[ch]) * (0.6 * k);
                    c[ch] = t + (1.0 - t) * (0.25 * k);
                }
            }
            for (double& v : c) {
                v = clamp01(v);
            }
            out.setD(x, y, delta(px, c0, c));
        }
    }
    return out;
}

} // namespace

Image retouch(const Image& in, LibraryEffect op, const LibraryParams& q, const Image* second) {
    return op == E::SkinSmoothing ? skinSmoothing(in, q) : faceRetouch(in, q, second);
}

} // namespace wabcut::render::reference::detail
