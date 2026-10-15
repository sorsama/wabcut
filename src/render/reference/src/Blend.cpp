#include <wabcut/render/reference/Blend.h>

#include <algorithm>
#include <cmath>

namespace wabcut::render::reference {

namespace {

using wabcut::BlendMode;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

// ---- Separable blend functions B(b = backdrop, s = source) ----

double multiplyOf(double b, double s) { return b * s; }

double screenOf(double b, double s) { return b + s - b * s; }

double colorBurnOf(double b, double s) {
    if (b >= 1.0) {
        return 1.0;
    }
    if (s <= 0.0) {
        return 0.0;
    }
    return 1.0 - std::min(1.0, (1.0 - b) / s);
}

double colorDodgeOf(double b, double s) {
    if (b <= 0.0) {
        return 0.0;
    }
    if (s >= 1.0) {
        return 1.0;
    }
    return std::min(1.0, b / (1.0 - s));
}

double hardLightOf(double b, double s) { return s <= 0.5 ? multiplyOf(b, 2.0 * s) : screenOf(b, 2.0 * s - 1.0); }

double softLightOf(double b, double s) {
    if (s <= 0.5) {
        return b - (1.0 - 2.0 * s) * b * (1.0 - b);
    }
    const double d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : std::sqrt(b);
    return b + (2.0 * s - 1.0) * (d - b);
}

double vividLightOf(double b, double s) { return s <= 0.5 ? colorBurnOf(b, 2.0 * s) : colorDodgeOf(b, 2.0 * s - 1.0); }

double pinLightOf(double b, double s) { return s <= 0.5 ? std::min(b, 2.0 * s) : std::max(b, 2.0 * s - 1.0); }

double divideOf(double b, double s) {
    if (s == 0.0) {
        return b == 0.0 ? 0.0 : 1.0;
    }
    return std::min(1.0, b / s);
}

double separable(BlendMode mode, double b, double s) {
    switch (mode) {
    case BlendMode::Normal:
    case BlendMode::Dissolve:
        return s;
    case BlendMode::Darken:
        return std::min(b, s);
    case BlendMode::Multiply:
        return multiplyOf(b, s);
    case BlendMode::ColorBurn:
        return colorBurnOf(b, s);
    case BlendMode::LinearBurn:
        return std::max(0.0, b + s - 1.0);
    case BlendMode::Lighten:
        return std::max(b, s);
    case BlendMode::Screen:
        return screenOf(b, s);
    case BlendMode::ColorDodge:
        return colorDodgeOf(b, s);
    case BlendMode::Add:
        return b + s;
    case BlendMode::Overlay:
        return hardLightOf(s, b);
    case BlendMode::SoftLight:
        return softLightOf(b, s);
    case BlendMode::HardLight:
        return hardLightOf(b, s);
    case BlendMode::VividLight:
        return vividLightOf(b, s);
    case BlendMode::LinearLight:
        return clamp01(b + 2.0 * s - 1.0);
    case BlendMode::PinLight:
        return pinLightOf(b, s);
    case BlendMode::HardMix:
        return b + s >= 1.0 ? 1.0 : 0.0;
    case BlendMode::Difference:
        return std::abs(b - s);
    case BlendMode::Exclusion:
        return b + s - 2.0 * b * s;
    case BlendMode::Subtract:
        return std::max(0.0, b - s);
    case BlendMode::Divide:
        return divideOf(b, s);
    case BlendMode::DarkerColor:
    case BlendMode::LighterColor:
    case BlendMode::Hue:
    case BlendMode::Saturation:
    case BlendMode::Color:
    case BlendMode::Luminosity:
        break; // whole-pixel modes (blendFunction)
    }
    return s;
}

// ---- W3C non-separable helpers ----

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

double satOf(const Rgb& c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

Rgb setSat(const Rgb& c, double s) {
    std::size_t imax = 0;
    std::size_t imin = 0;
    for (std::size_t i = 1; i < 3; ++i) {
        if (c[i] > c[imax]) {
            imax = i;
        }
        if (c[i] < c[imin]) {
            imin = i;
        }
    }
    Rgb out{0.0, 0.0, 0.0};
    if (imax == imin) {
        return out; // every channel equal: no saturation to keep
    }
    const std::size_t imid = 3 - imax - imin;
    const double range = c[imax] - c[imin];
    out[imid] = (c[imid] - c[imin]) * s / range;
    out[imax] = s;
    return out;
}

Rgb clamped(const Rgb& c) { return {clamp01(c[0]), clamp01(c[1]), clamp01(c[2])}; }

double unitAlpha(double a) { return std::isnan(a) ? 0.0 : clamp01(a); }

} // namespace

bool blendClampsInputs(BlendMode mode) {
    switch (mode) {
    case BlendMode::ColorBurn:
    case BlendMode::LinearBurn:
    case BlendMode::Screen:
    case BlendMode::ColorDodge:
    case BlendMode::Overlay:
    case BlendMode::SoftLight:
    case BlendMode::HardLight:
    case BlendMode::VividLight:
    case BlendMode::LinearLight:
    case BlendMode::PinLight:
    case BlendMode::HardMix:
    case BlendMode::Exclusion:
    case BlendMode::Hue:
    case BlendMode::Saturation:
    case BlendMode::Color:
    case BlendMode::Luminosity:
        return true;
    default:
        return false;
    }
}

Rgb blendFunction(BlendMode mode, const Rgb& cb, const Rgb& cs) {
    const bool clampInputs = blendClampsInputs(mode);
    const Rgb b = clampInputs ? clamped(cb) : cb;
    const Rgb s = clampInputs ? clamped(cs) : cs;
    switch (mode) {
    case BlendMode::DarkerColor:
        return luma(s) < luma(b) ? s : b;
    case BlendMode::LighterColor:
        return luma(s) > luma(b) ? s : b;
    case BlendMode::Hue:
        return setLum(setSat(s, satOf(b)), luma(b));
    case BlendMode::Saturation:
        return setLum(setSat(b, satOf(s)), luma(b));
    case BlendMode::Color:
        return setLum(s, luma(b));
    case BlendMode::Luminosity:
        return setLum(b, luma(s));
    default:
        return {separable(mode, b[0], s[0]), separable(mode, b[1], s[1]), separable(mode, b[2], s[2])};
    }
}

std::uint64_t splitmix64(std::uint64_t seed, std::uint64_t n) {
    std::uint64_t z = seed + (n + 1u) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double dissolveValue(std::uint64_t seed, int x, int y, int width, std::int64_t frame) {
    const std::uint64_t n = static_cast<std::uint64_t>(y) * static_cast<std::uint64_t>(width) + static_cast<std::uint64_t>(x) +
                            (static_cast<std::uint64_t>(frame) << 32);
    return static_cast<double>(splitmix64(seed, n) >> 11) * 0x1.0p-53;
}

Image blend(const Image& backdrop, const Image& source, BlendMode mode, double opacity, std::uint64_t seed,
            std::int64_t frame) {
    Image out(backdrop.width, backdrop.height);
    const double amount = unitAlpha(opacity);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const Rgba bp = backdrop.atD(x, y);
            const Rgba sp = x < source.width && y < source.height ? source.atD(x, y) : Rgba{0.0, 0.0, 0.0, 0.0};
            const double ab = unitAlpha(bp[3]);
            const double as = unitAlpha(sp[3]);
            const Rgb cb = ab > 0.0 ? Rgb{bp[0] / ab, bp[1] / ab, bp[2] / ab} : Rgb{0.0, 0.0, 0.0};
            const Rgb cs = as > 0.0 ? Rgb{sp[0] / as, sp[1] / as, sp[2] / as} : Rgb{0.0, 0.0, 0.0};
            const double asOpacity = as * amount;
            if (mode == BlendMode::Dissolve) {
                const bool shows = dissolveValue(seed, x, y, out.width, frame) < asOpacity;
                out.setD(x, y, shows ? Rgba{cs[0], cs[1], cs[2], 1.0} : bp);
                continue;
            }
            const Rgb mixed = blendFunction(mode, cb, cs);
            Rgba result{};
            for (std::size_t c = 0; c < 3; ++c) {
                const double cr = (1.0 - ab) * cs[c] + ab * mixed[c];
                result[c] = asOpacity * cr + (1.0 - asOpacity) * bp[c];
            }
            result[3] = asOpacity + ab * (1.0 - asOpacity);
            out.setD(x, y, result);
        }
    }
    return out;
}

} // namespace wabcut::render::reference
