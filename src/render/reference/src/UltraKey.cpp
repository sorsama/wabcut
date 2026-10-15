#include <wabcut/render/reference/UltraKey.h>

#include <wabcut/render/reference/Layout.h>

#include <algorithm>
#include <cmath>

namespace wabcut::render::reference {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCbScale = 1.8556; // 2 (1 - 0.0722)
constexpr double kCrScale = 1.5748; // 2 (1 - 0.2126)

double clamp01(double v) { return std::isnan(v) ? 0.0 : std::clamp(v, 0.0, 1.0); }

double smoothstep(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

Rgb clampedRgb(const Rgb& c) { return {clamp01(c[0]), clamp01(c[1]), clamp01(c[2])}; }

} // namespace

UltraKeyParams ultraKeyParams(const EffectInstance& instance, Ticks curveTime) {
    const UltraKeyParams d;
    const auto at = [&](std::string_view key, double fallback) { return effectParameterAt(instance, key, curveTime, fallback); };
    UltraKeyParams p;
    const double output = std::clamp(std::round(at("output", static_cast<double>(static_cast<int>(d.output)))), 0.0, 2.0);
    p.output = static_cast<UltraKeyOutput>(static_cast<int>(output));
    p.keyColor = {at("keyColor.r", d.keyColor[0]), at("keyColor.g", d.keyColor[1]), at("keyColor.b", d.keyColor[2])};
    p.transparency = at("transparency", d.transparency);
    p.highlight = at("highlight", d.highlight);
    p.shadow = at("shadow", d.shadow);
    p.tolerance = at("tolerance", d.tolerance);
    p.pedestal = at("pedestal", d.pedestal);
    p.choke = at("choke", d.choke);
    p.soften = at("soften", d.soften);
    p.contrast = at("contrast", d.contrast);
    p.midPoint = at("midPoint", d.midPoint);
    p.desaturate = at("desaturate", d.desaturate);
    p.range = at("range", d.range);
    p.spill = at("spill", d.spill);
    p.luma = at("luma", d.luma);
    p.saturation = at("saturation", d.saturation);
    p.hue = at("hue", d.hue);
    p.luminance = at("luminance", d.luminance);
    return p;
}

YCbCr toYCbCr(const Rgb& encoded) {
    const double y = luma(encoded);
    return {y, (encoded[2] - y) / kCbScale, (encoded[0] - y) / kCrScale};
}

Rgb fromYCbCr(const YCbCr& c) {
    const double r = c.y + kCrScale * c.cr;
    const double b = c.y + kCbScale * c.cb;
    const double g = (c.y - 0.2126 * r - 0.0722 * b) / 0.7152;
    return {r, g, b};
}

KeyDistance keyDistance(const Rgb& encoded, const Rgb& keyColor) {
    const YCbCr p = toYCbCr(encoded);
    const YCbCr k = toYCbCr(clampedRgb(keyColor));
    const double keyChroma = std::max(std::hypot(k.cb, k.cr), 1e-6);
    return {std::hypot(p.cb - k.cb, p.cr - k.cr) / keyChroma, p.y - k.y};
}

double ultraKeyMatteAt(const Rgb& encoded, const UltraKeyParams& params) {
    const KeyDistance k = keyDistance(encoded, params.keyColor);
    const double t = params.tolerance / 100.0 * 0.5;
    const double p = params.pedestal / 100.0 * 0.25;
    const double w = 0.25 * (1.0 - params.transparency / 100.0) + 0.02;
    const double chroma = smoothstep(t, t + w, k.d - p);
    double a = chroma + params.highlight / 100.0 * std::max(0.0, k.dl) * (1.0 - chroma);
    // Shadows only clear what the chroma matte calls background: a dark foreground stays opaque (C.16 fix of the C.15
    // formula, which also thinned dark hair and clothes).
    a = a * (1.0 - params.shadow / 100.0 * std::max(0.0, -k.dl) * (1.0 - chroma));
    return clamp01(a);
}

Rgb ultraKeyColorAt(const Rgb& encoded, const UltraKeyParams& params) {
    const Rgb key = clampedRgb(params.keyColor);
    const KeyDistance k = keyDistance(encoded, key);
    // Full suppression up to where the matte starts to rise (t + p), fading out over `range` past it: the fringe is
    // what keeps spill (C.16 fix: the C.15 falloff from 0 ended before the default matte's edge).
    const double start = params.tolerance / 100.0 * 0.5 + params.pedestal / 100.0 * 0.25;
    const double s = params.spill / 100.0 * (1.0 - smoothstep(start, start + params.range / 100.0 * 0.5 + 0.05, k.d));
    // The key channel: the first of the largest in R, G, B order.
    std::size_t kc = 0;
    for (std::size_t i = 1; i < 3; ++i) {
        if (key[i] > key[kc]) {
            kc = i;
        }
    }
    const std::size_t o1 = (kc + 1) % 3;
    const std::size_t o2 = (kc + 2) % 3;
    Rgb c = encoded;
    const double yOriginal = luma(c);
    const double limit = (c[o1] + c[o2]) / 2.0;
    c[kc] = c[kc] - s * std::max(0.0, c[kc] - limit);
    const double yCut = luma(c);
    const double desaturate = params.desaturate / 100.0 * s;
    for (double& v : c) {
        v = v + desaturate * (yCut - v);
    }
    const double restore = (params.luma / 100.0 - 0.5) * s * (yOriginal - yCut);
    for (double& v : c) {
        v += restore;
    }
    // Colour correction in Y'CbCr.
    YCbCr q = toYCbCr(c);
    q.y *= params.luminance / 100.0;
    const double h = params.hue * kPi / 180.0;
    const double sat = params.saturation / 100.0;
    const double cb = (q.cb * std::cos(h) - q.cr * std::sin(h)) * sat;
    const double cr = (q.cb * std::sin(h) + q.cr * std::cos(h)) * sat;
    q.cb = cb;
    q.cr = cr;
    return clampedRgb(fromYCbCr(q));
}

Image ultraKey(const Image& picture, const UltraKeyParams& params) {
    const int w = picture.width;
    const int h = picture.height;
    Image out(w, h);
    if (out.empty()) {
        return out;
    }
    const std::size_t count = picture.pixelCount();
    std::vector<Rgb> encoded(count);
    std::vector<double> alphaIn(count);
    std::vector<double> matte(count);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            const Rgba p = picture.atD(x, y);
            const double a = clamp01(p[3]);
            alphaIn[i] = a;
            encoded[i] = a > 0.0 ? Rgb{toEncoded(p[0] / a), toEncoded(p[1] / a), toEncoded(p[2] / a)} : Rgb{0.0, 0.0, 0.0};
            matte[i] = ultraKeyMatteAt(encoded[i], params);
        }
    }
    // Choke: erode by a disc of radius rc pixels.
    const double rc = std::max(0.0, params.choke) / 100.0 * 0.005 * static_cast<double>(w);
    if (rc >= 1.0) {
        const int reach = static_cast<int>(std::floor(rc));
        std::vector<double> eroded(count);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double lowest = 1.0;
                for (int dy = -reach; dy <= reach; ++dy) {
                    for (int dx = -reach; dx <= reach; ++dx) {
                        const int qx = x + dx;
                        const int qy = y + dy;
                        if (static_cast<double>(dx * dx + dy * dy) <= rc * rc && qx >= 0 && qy >= 0 && qx < w && qy < h) {
                            lowest = std::min(lowest, matte[static_cast<std::size_t>(qy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(qx)]);
                        }
                    }
                }
                eroded[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = lowest;
            }
        }
        matte = std::move(eroded);
    }
    // Soften: the Gaussian blur's kernel on the matte (held in every channel of a picture), edges repeated.
    const double sigma = std::max(0.0, params.soften) / 100.0 * 0.005 * static_cast<double>(w);
    if (sigma >= 0.05) {
        Image holder(w, h);
        for (std::size_t i = 0; i < count; ++i) {
            const float v = static_cast<float>(matte[i]);
            std::fill_n(holder.rgba.begin() + static_cast<std::ptrdiff_t>(i * 4), 4, v);
        }
        const Image soft = gaussianBlurSigma(holder, sigma, true);
        for (std::size_t i = 0; i < count; ++i) {
            matte[i] = static_cast<double>(soft.rgba[i * 4 + 3]);
        }
    }
    // Contrast about the mid point.
    const double mid = params.midPoint / 100.0;
    const double gain = 1.0 + params.contrast / 25.0;
    for (double& v : matte) {
        v = std::clamp((v - mid) * gain + mid, 0.0, 1.0);
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            const double a = alphaIn[i] * matte[i];
            switch (params.output) {
            case UltraKeyOutput::AlphaChannel: {
                const double g = toLinear(a);
                out.setD(x, y, {g, g, g, 1.0});
                break;
            }
            case UltraKeyOutput::ColorChannel: {
                const Rgb c = ultraKeyColorAt(encoded[i], params);
                out.setD(x, y, {toLinear(c[0]), toLinear(c[1]), toLinear(c[2]), 1.0});
                break;
            }
            case UltraKeyOutput::Composite:
            default: {
                const Rgb c = ultraKeyColorAt(encoded[i], params);
                out.setD(x, y, {toLinear(c[0]) * a, toLinear(c[1]) * a, toLinear(c[2]) * a, a});
                break;
            }
            }
        }
    }
    return out;
}

} // namespace wabcut::render::reference
