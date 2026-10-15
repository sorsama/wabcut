#include "EffectsReference.h"

#include <algorithm>
#include <cmath>

namespace wabcut::render::test {

namespace {

constexpr int kMaxTaps = 96;
constexpr double kPi = 3.14159265358979323846;

double srgbToLinear(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double clamp01(double v) {
    return std::clamp(v, 0.0, 1.0);
}

double smoothstep(double e0, double e1, double x) {
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

Rgba mix(const Rgba& a, const Rgba& b, double t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t};
}

/// One separable Gaussian pass (shader MODE_BLUR at lod 0).
LinearImage blurPass(const LinearImage& in, int dx, int dy, double sigma, bool repeatEdges) {
    if (sigma < 0.05) {
        return in;
    }
    LinearImage out(in.width, in.height);
    const int taps = std::min(static_cast<int>(std::ceil(3.0 * sigma)), kMaxTaps);
    const auto fetch = [&](int x, int y) -> Rgba {
        if (!repeatEdges && (x < 0 || y < 0 || x >= in.width || y >= in.height)) {
            return {0, 0, 0, 0};
        }
        return in.at(std::clamp(x, 0, in.width - 1), std::clamp(y, 0, in.height - 1));
    };
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            Rgba acc = fetch(x, y);
            double total = 1.0;
            for (int i = 1; i <= taps; ++i) {
                const double w = std::exp(-0.5 * i * i / (sigma * sigma));
                const Rgba a = fetch(x + i * dx, y + i * dy);
                const Rgba b = fetch(x - i * dx, y - i * dy);
                for (int c = 0; c < 4; ++c) {
                    acc[static_cast<std::size_t>(c)] += w * (a[static_cast<std::size_t>(c)] + b[static_cast<std::size_t>(c)]);
                }
                total += 2.0 * w;
            }
            for (double& v : acc) {
                v /= total;
            }
            out.at(x, y) = acc;
        }
    }
    return out;
}

LinearImage blur(const LinearImage& in, double sigma, bool repeatEdges) {
    return blurPass(blurPass(in, 1, 0, sigma, repeatEdges), 0, 1, sigma, repeatEdges);
}

template <class Fn>
LinearImage perPixel(const LinearImage& in, Fn&& fn) {
    LinearImage out(in.width, in.height);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            out.at(x, y) = fn(in.at(x, y), x + 0.5, y + 0.5);
        }
    }
    return out;
}

} // namespace

Rgba LinearImage::sample(double x, double y) const {
    const double fx = x - 0.5;
    const double fy = y - 0.5;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const double tx = fx - x0;
    const double ty = fy - y0;
    const auto texel = [&](int xi, int yi) { return at(std::clamp(xi, 0, width - 1), std::clamp(yi, 0, height - 1)); };
    return mix(mix(texel(x0, y0), texel(x0 + 1, y0), tx), mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), tx), ty);
}

LinearImage linearFromRgba8(int width, int height, const std::function<std::array<std::uint8_t, 4>(int, int)>& at) {
    LinearImage out(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto c = at(x, y);
            const double a = c[3] / 255.0;
            out.at(x, y) = {srgbToLinear(c[0] / 255.0) * a, srgbToLinear(c[1] / 255.0) * a,
                            srgbToLinear(c[2] / 255.0) * a, a};
        }
    }
    return out;
}

LinearImage referenceOver(const LinearImage& top, const LinearImage& bottom) {
    LinearImage out(top.width, top.height);
    for (std::size_t i = 0; i < top.px.size(); ++i) {
        const Rgba& t = top.px[i];
        const Rgba& b = bottom.px[i];
        out.px[i] = {t[0] + b[0] * (1 - t[3]), t[1] + b[1] * (1 - t[3]), t[2] + b[2] * (1 - t[3]), t[3] + b[3] * (1 - t[3])};
    }
    return out;
}

LinearImage referenceEffect(const LinearImage& in, const LayerEffect& e) {
    const auto& q = e.params;
    const double w = in.width;
    const double h = in.height;
    switch (e.type) {
    case EffectType::Crop:
        return perPixel(in, [&](const Rgba& c, double x, double y) {
            const double insideX = std::min(x - q[0] * w, (w - q[2] * w) - x);
            const double insideY = std::min(y - q[1] * h, (h - q[3] * h) - y);
            const double d = std::min(insideX, insideY);
            const double feather = std::max(0.0, q[4]);
            const double mask = feather > 0.0 ? clamp01(d / feather) : clamp01(d + 0.5);
            return Rgba{c[0] * mask, c[1] * mask, c[2] * mask, c[3] * mask};
        });
    case EffectType::GaussianBlur:
        return blur(in, std::max(0.0, q[0]) / 3.0, q[1] > 0.5);
    case EffectType::BasicCorrection:
        return perPixel(in, [&](const Rgba& c, double, double) {
            if (c[3] <= 0.0) {
                return c;
            }
            std::array<double, 3> rgb{std::max(c[0] / c[3], 0.0), std::max(c[1] / c[3], 0.0), std::max(c[2] / c[3], 0.0)};
            const double gain = std::exp2(q[0]);
            const double t = q[3] / 100.0;
            const double g = q[4] / 100.0;
            const std::array<double, 3> wb{(1.0 + 0.25 * t) * (1.0 + 0.1 * g), 1.0 - 0.2 * g, (1.0 - 0.25 * t) * (1.0 + 0.1 * g)};
            const double k = 1.0 + q[1] / 100.0;
            for (int i = 0; i < 3; ++i) {
                double v = rgb[static_cast<std::size_t>(i)] * gain * wb[static_cast<std::size_t>(i)];
                v = 0.18 * std::pow(std::max(v, 0.0) / 0.18, k);
                rgb[static_cast<std::size_t>(i)] = v;
            }
            const double luma = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
            for (double& v : rgb) {
                v = std::max(luma + (v - luma) * (q[2] / 100.0), 0.0);
            }
            return Rgba{rgb[0] * c[3], rgb[1] * c[3], rgb[2] * c[3], c[3]};
        });
    case EffectType::Vignette:
        return perPixel(in, [&](const Rgba& c, double x, double y) {
            double qx = x / w * 2.0 - 1.0;
            const double qy = y / h * 2.0 - 1.0;
            const double roundness = q[2] / 100.0;
            qx *= 1.0 + (w / std::max(h, 1.0) - 1.0) * std::max(roundness, 0.0);
            const double n = 2.0 + 6.0 * std::max(-roundness, 0.0);
            const double d = std::pow(std::pow(std::abs(qx), n) + std::pow(std::abs(qy), n), 1.0 / n);
            const double mid = 0.3 + 0.9 * q[1] / 100.0;
            const double width = std::max(0.01, q[3] / 100.0);
            const double v = smoothstep(mid - 0.5 * width, mid + 0.5 * width, d);
            const double k = std::exp2(q[0] * v);
            return Rgba{c[0] * k, c[1] * k, c[2] * k, c[3]};
        });
    case EffectType::Sharpen: {
        const double sigma = std::max(0.0, q[1]);
        if (q[0] <= 0.0 || sigma < 0.05) {
            return in;
        }
        const LinearImage blurred = blur(in, sigma, true);
        LinearImage out(in.width, in.height);
        const double amount = q[0] / 100.0;
        for (std::size_t i = 0; i < in.px.size(); ++i) {
            const Rgba& c = in.px[i];
            const Rgba& b = blurred.px[i];
            out.px[i] = {std::max(c[0] + amount * (c[0] - b[0]), 0.0), std::max(c[1] + amount * (c[1] - b[1]), 0.0),
                         std::max(c[2] + amount * (c[2] - b[2]), 0.0), c[3]};
        }
        return out;
    }
    case EffectType::DropShadow: {
        const double angle = q[1] * kPi / 180.0;
        const double distance = std::max(0.0, q[2]);
        const double dx = distance * std::sin(angle);
        const double dy = -distance * std::cos(angle);
        const double opacity = clamp01(q[0]);
        const std::array<double, 3> color{srgbToLinear(q[4]), srgbToLinear(q[5]), srgbToLinear(q[6])};
        LinearImage shadow = perPixel(in, [&](const Rgba&, double x, double y) {
            const double sx = x - dx;
            const double sy = y - dy;
            double a = in.sample(sx, sy)[3];
            if (sx < 0.0 || sy < 0.0 || sx > w || sy > h) {
                a = 0.0;
            }
            const double k = a * opacity;
            return Rgba{color[0] * k, color[1] * k, color[2] * k, k};
        });
        const double sigma = std::max(0.0, q[3]) / 3.0;
        if (sigma >= 0.05) {
            shadow = blur(shadow, sigma, false);
        }
        return referenceOver(in, shadow);
    }
    case EffectType::Border: {
        const double r = std::max(0.0, q[0]);
        const double opacity = clamp01(q[4]);
        if (r < 0.05 || opacity <= 0.0) {
            return in;
        }
        const std::array<double, 3> color{srgbToLinear(q[1]), srgbToLinear(q[2]), srgbToLinear(q[3])};
        const LinearImage border = perPixel(in, [&](const Rgba& c, double x, double y) {
            double a = c[3];
            for (int i = 0; i < 64; ++i) { // as fx.frag MODE_BORDER: rings at the full and half width
                const double angle = i * (2.0 * kPi / 64.0);
                const double dx = std::cos(angle) * r;
                const double dy = std::sin(angle) * r;
                a = std::max({a, in.sample(x + dx, y + dy)[3], in.sample(x + 0.5 * dx, y + 0.5 * dy)[3]});
            }
            const double k = clamp01(a) * opacity;
            return Rgba{color[0] * k, color[1] * k, color[2] * k, k};
        });
        return referenceOver(in, border);
    }
    default:
        break;
    }
    return in;
}

LinearImage referenceTransition(const LinearImage* a, const LinearImage* b, const LayerTransition& mixInfo) {
    const LinearImage& shape = a ? *a : *b;
    LinearImage out(shape.width, shape.height);
    const double t = clamp01(mixInfo.progress);
    const double w = shape.width;
    const double h = shape.height;
    const double angle = mixInfo.angleDegrees * kPi / 180.0;
    const double dirX = std::cos(angle);
    const double dirY = std::sin(angle);
    const double s0 = std::min(0.0, dirX * w) + std::min(0.0, dirY * h);
    const double s1 = std::max(0.0, dirX * w) + std::max(0.0, dirY * h);
    const double len = s1 - s0;
    const double feather = clamp01(mixInfo.feather) * len;
    const double e = s0 - 0.5 * feather + t * (len + feather);
    const Rgba transparent{0, 0, 0, 0};
    for (int y = 0; y < shape.height; ++y) {
        for (int x = 0; x < shape.width; ++x) {
            const Rgba ca = a ? a->at(x, y) : transparent;
            const Rgba cb = b ? b->at(x, y) : transparent;
            Rgba r{};
            switch (mixInfo.kind) {
            case TransitionKind::DipToColor: {
                const Rgba color{mixInfo.color[0], mixInfo.color[1], mixInfo.color[2], 1.0};
                r = t < 0.5 ? mix(ca, color, 2.0 * t) : mix(color, cb, 2.0 * t - 1.0);
                break;
            }
            case TransitionKind::Wipe: {
                const double s = (x + 0.5) * dirX + (y + 0.5) * dirY;
                const double m = feather > 0.0 ? clamp01((e - s) / feather + 0.5) : clamp01((e - s) + 0.5);
                r = mix(ca, cb, m);
                break;
            }
            case TransitionKind::CrossDissolve:
                r = mix(ca, cb, t);
                break;
            }
            out.at(x, y) = r;
        }
    }
    return out;
}

std::vector<std::uint8_t> encodeOverBlack(const LinearImage& image, OutputTransform transform) {
    std::vector<std::uint8_t> rgb(image.px.size() * 3);
    for (std::size_t i = 0; i < image.px.size(); ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            const double v = linearToOutput(transform, image.px[i][c]);
            rgb[i * 3 + c] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
        }
    }
    return rgb;
}

} // namespace wabcut::render::test
