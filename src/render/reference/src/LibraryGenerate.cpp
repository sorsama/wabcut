// The Generate family's CPU twins (shaders/fx_generate.frag), for render/reference/LibraryEffects.h.

#include "LibraryInternal.h"

#include <algorithm>
#include <cmath>

namespace wabcut::render::reference::detail {

namespace {

using E = LibraryEffect;

constexpr double kPi = 3.14159265358979;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double radians(double d) { return d * kPi / 180.0; }
double smoothstep(double e0, double e1, double x) {
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}
double glslMod(double x, double y) { return x - y * std::floor(x / y); }

Rgb encoded(const Rgba& c) {
    const double a = clamp01(c[3]);
    if (a <= 0.0) {
        return {};
    }
    return {toEncoded(c[0] / a), toEncoded(c[1] / a), toEncoded(c[2] / a)};
}

Rgb srgb3(double r, double g, double b) { return {srgbToLinear(r), srgbToLinear(g), srgbToLinear(b)}; }

Rgba mixC(const Rgba& a, const Rgba& b, double t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t};
}
Rgba scaled(const Rgba& a, double k) { return {a[0] * k, a[1] * k, a[2] * k, a[3] * k}; }

double cwAngle(double ux, double uy, double dx, double dy) {
    const double y = ux * dy - uy * dx;
    const double x = ux * dx + uy * dy;
    return x == 0.0 && y == 0.0 ? 0.0 : std::atan2(y, x);
}

double blendChannel(int mode, double b, double s) {
    switch (mode) {
    case 3: return b * s;
    case 4: return b + s - b * s;
    case 5: return b <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
    case 6: return std::min(b, s);
    case 7: return std::max(b, s);
    case 8: return std::abs(b - s);
    default: return s;
    }
}

Rgba blendOver(int mode, const Rgba& dst, const Rgba& src) {
    if (mode == 0) {
        return src;
    }
    if (mode == 2) {
        return {dst[0] + src[0], dst[1] + src[1], dst[2] + src[2], std::min(1.0, dst[3] + src[3])};
    }
    const double sa = clamp01(src[3]);
    const double ba = clamp01(dst[3]);
    Rgba out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const double cs = sa > 0.0 ? clamp01(src[i] / sa) : 0.0;
        const double cb = ba > 0.0 ? clamp01(dst[i] / ba) : 0.0;
        const double cr = (1.0 - ba) * cs + ba * blendChannel(mode, cb, cs);
        out[i] = sa * cr + (1.0 - sa) * dst[i];
    }
    out[3] = sa + ba * (1.0 - sa);
    return out;
}

Rgba drawShape(int mode, const Rgba& dst, const Rgb& color, double k) {
    if (mode == 0) {
        return {color[0] * k, color[1] * k, color[2] * k, k};
    }
    if (mode == 9) {
        return scaled(dst, k);
    }
    if (mode == 10) {
        return scaled(dst, 1.0 - k);
    }
    return blendOver(mode, dst, {color[0] * k, color[1] * k, color[2] * k, k});
}

std::array<double, 2> cellSize(double sizeFrom, double ax, double ay, double cx, double cy, double width, double height) {
    const int from = static_cast<int>(std::floor(sizeFrom + 0.5));
    double w = 0.0;
    double h = 0.0;
    if (from == 0) {
        w = std::abs(cx - ax);
        h = std::abs(cy - ay);
    } else if (from == 1) {
        w = width;
        h = width;
    } else {
        w = width;
        h = height;
    }
    return {std::max(w, 1.0), std::max(h, 1.0)};
}

Rgb lensFlare(double fx, double fy, double sx, double sy, double cx, double cy, double brightness, int lens) {
    const double diag = std::hypot(sx, sy);
    const double dx = fx - cx;
    const double dy = fy - cy;
    const double r = std::hypot(dx, dy);
    const double glow = 0.9 * std::exp(-r * r / (2.0 * (0.05 * diag) * (0.05 * diag))) + 0.3 * std::exp(-r / (0.12 * diag));
    const double streak = 0.4 * std::exp(-std::abs(dy) / (0.004 * diag)) * std::exp(-std::abs(dx) / (0.3 * diag));
    Rgb color{(glow + streak) * 1.0, (glow + streak) * 0.92, (glow + streak) * 0.8};
    const double ax = 0.5 * sx - cx;
    const double ay = 0.5 * sy - cy;
    for (int k = 0; k < 6; ++k) {
        const double t = lens == 1 ? 0.35 + 0.33 * k : (lens == 2 ? 0.6 + 0.22 * k : 0.25 + 0.3 * k);
        const double radius = diag * (lens == 2 ? 0.02 + 0.012 * k : 0.03 + 0.018 * glslMod(k * 3.0, 5.0));
        const Rgb tint = k % 3 == 0 ? Rgb{0.3, 0.6, 1.0} : (k % 3 == 1 ? Rgb{0.4, 1.0, 0.5} : Rgb{1.0, 0.5, 0.3});
        const double g = std::hypot(fx - (cx + ax * t), fy - (cy + ay * t));
        const double disc = 0.12 * (1.0 - smoothstep(0.75 * radius, radius, g));
        for (std::size_t i = 0; i < 3; ++i) {
            color[i] += tint[i] * disc;
        }
    }
    for (double& v : color) {
        v *= brightness / 100.0;
    }
    return color;
}

double wipeKeep(double field, double feather, double completion) {
    const double w = std::max(feather, 1.0);
    return 1.0 - clamp01((field + 0.5 * w * (2.0 * completion - 1.0)) / w + 0.5);
}

} // namespace

bool isGenerate(LibraryEffect effect) {
    return static_cast<int>(effect) >= 100;
}

Image generate(const Image& in, LibraryEffect op, const LibraryParams& q, const Image* second) {
    Image out(in.width, in.height);
    const double sx = in.width;
    const double sy = in.height;
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            const double fx = x + 0.5; // frame = canvas
            const double fy = y + 0.5;
            const Rgba c = in.atD(x, y);
            const Rgba other = second && second->width == in.width && second->height == in.height ? second->atD(x, y) : Rgba{};
            const bool hasSecond = second != nullptr;
            const auto px = static_cast<std::uint32_t>(x);
            const auto py = static_cast<std::uint32_t>(y);
            Rgba r = c;
            switch (op) {
            case E::FourColorGradient: {
                const double diag = std::hypot(sx, sy);
                const double power = 200.0 / std::max(q[20], 1.0);
                Rgb total{};
                double weights = 0.0;
                for (std::size_t i = 0; i < 4; ++i) {
                    const std::size_t o = 5 * i;
                    const double d = std::hypot(fx - q[o] * sx, fy - q[o + 1] * sy) / diag;
                    const double w = 1.0 / (std::pow(d, power) + 1e-6);
                    for (std::size_t ch = 0; ch < 3; ++ch) {
                        total[ch] += q[o + 2 + ch] * w;
                    }
                    weights += w;
                }
                const double jitter = (libraryHash01(px + 65536u * py, 3u, 0u) - 0.5) * q[21] * 0.2;
                const Rgba gradient{toLinear(clamp01(total[0] / weights + jitter)), toLinear(clamp01(total[1] / weights + jitter)),
                                    toLinear(clamp01(total[2] / weights + jitter)), 1.0};
                const int mode = static_cast<int>(std::floor(q[23] + 0.5));
                r = mode == 0 ? mixC(c, gradient, q[22]) : blendOver(mode, c, scaled(gradient, q[22]));
                break;
            }
            case E::Ramp: {
                const double ax = q[5] * sx - q[0] * sx;
                const double ay = q[6] * sy - q[1] * sy;
                const double dx = fx - q[0] * sx;
                const double dy = fy - q[1] * sy;
                const double t = q[10] > 0.5 ? std::hypot(dx, dy) / std::max(std::hypot(ax, ay), 1e-6)
                                             : (dx * ax + dy * ay) / std::max(ax * ax + ay * ay, 1e-6);
                const double scatter = (libraryHash01(px + 65536u * py, 5u, 0u) - 0.5) * q[11] / 255.0;
                Rgba ramp{0.0, 0.0, 0.0, 1.0};
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    const double v = q[2 + ch] + (q[7 + ch] - q[2 + ch]) * clamp01(t) + scatter;
                    ramp[ch] = toLinear(clamp01(v));
                }
                r = mixC(ramp, c, q[12]);
                break;
            }
            case E::Circle: {
                const double d = std::hypot(fx - q[0] * sx, fy - q[1] * sy);
                const double rad = q[2];
                const int edge = static_cast<int>(std::floor(q[3] + 0.5));
                const double t = q[4];
                double fo = q[5];
                double fi = q[6];
                const double inner = edge == 1 ? t : (edge == 2 ? rad - t : rad - t * rad / 100.0);
                if (edge == 4) {
                    fo *= rad / 100.0;
                    fi *= rad / 100.0;
                }
                double k = fo > 0.0 ? clamp01((rad - d) / fo + 0.5) : clamp01(rad - d + 0.5);
                if (edge != 0) {
                    k *= fi > 0.0 ? clamp01((d - inner) / fi + 0.5) : clamp01(d - inner + 0.5);
                }
                if (q[7] > 0.5) {
                    k = 1.0 - k;
                }
                r = drawShape(static_cast<int>(std::floor(q[12] + 0.5)), c, srgb3(q[8], q[9], q[10]), k * q[11]);
                break;
            }
            case E::Grid: {
                const double ax = q[0] * sx;
                const double ay = q[1] * sy;
                const auto cell = cellSize(q[2], ax, ay, q[3] * sx, q[4] * sy, q[5], q[6]);
                const double lx = glslMod(fx - ax, cell[0]);
                const double ly = glslMod(fy - ay, cell[1]);
                const double distX = std::min(lx, cell[0] - lx);
                const double distY = std::min(ly, cell[1] - ly);
                const double b = 0.5 * q[7];
                double k = 0.0;
                if (q[7] > 0.0) {
                    k = std::max(clamp01((b - distX) / std::max(q[8], 1.0) + 0.5), clamp01((b - distY) / std::max(q[9], 1.0) + 0.5));
                }
                if (q[10] > 0.5) {
                    k = 1.0 - k;
                }
                r = drawShape(static_cast<int>(std::floor(q[15] + 0.5)), c, srgb3(q[11], q[12], q[13]), k * q[14]);
                break;
            }
            case E::Checkerboard: {
                const double ax = q[0] * sx;
                const double ay = q[1] * sy;
                const auto cell = cellSize(q[2], ax, ay, q[3] * sx, q[4] * sy, q[5], q[6]);
                const double ix = std::floor((fx - ax) / cell[0]);
                const double iy = std::floor((fy - ay) / cell[1]);
                const double k = glslMod(ix + iy, 2.0) == 0.0 ? 1.0 : 0.0;
                r = drawShape(static_cast<int>(std::floor(q[13] + 0.5)), c, srgb3(q[9], q[10], q[11]), k * q[12]);
                break;
            }
            case E::LensFlare: {
                const Rgb flare = lensFlare(fx, fy, sx, sy, q[0] * sx, q[1] * sy, q[2], static_cast<int>(std::floor(q[3] + 0.5)));
                const Rgba lit{c[0] + flare[0], c[1] + flare[1], c[2] + flare[2],
                               std::max(c[3], clamp01(std::max({flare[0], flare[1], flare[2]})))};
                r = mixC(lit, c, q[4]);
                break;
            }
            case E::BlockDissolve: {
                const double bw = std::max(q[1], 1.0);
                const double bh = std::max(q[2], 1.0);
                const auto ix = static_cast<std::int32_t>(std::floor(fx / bw));
                const auto iy = static_cast<std::int32_t>(std::floor(fy / bh));
                const double v = libraryHash01(static_cast<std::uint32_t>(ix), static_cast<std::uint32_t>(iy), 9u);
                r = v >= q[0] ? c : Rgba{};
                break;
            }
            case E::GradientWipe: {
                double g = luma(encoded(hasSecond ? other : c));
                if (q[4] > 0.5) {
                    g = 1.0 - g;
                }
                const double soft = q[1];
                const double keep = soft > 0.0 ? clamp01((g - q[0] * (1.0 + soft)) / soft + 1.0) : (g >= q[0] ? 1.0 : 0.0);
                r = scaled(c, keep);
                break;
            }
            case E::LinearWipe: {
                const double a = radians(q[1]);
                const double vx = std::sin(a);
                const double vy = -std::cos(a);
                const double s0 = std::min(0.0, vx * sx) + std::min(0.0, vy * sy);
                const double s1 = std::max(0.0, vx * sx) + std::max(0.0, vy * sy);
                const double field = s0 + q[0] * (s1 - s0) - (fx * vx + fy * vy);
                r = scaled(c, wipeKeep(field, q[2], q[0]));
                break;
            }
            case E::RadialWipe: {
                const double a0 = radians(q[1]);
                const double dx = fx - q[2] * sx;
                const double dy = fy - q[3] * sy;
                const double rr = std::hypot(dx, dy);
                double a = cwAngle(std::sin(a0), -std::cos(a0), dx, dy);
                const int wipe = static_cast<int>(std::floor(q[4] + 0.5));
                double field = 0.0;
                if (wipe == 2) {
                    field = rr * (q[0] * kPi - std::abs(a));
                } else {
                    if (wipe == 1) {
                        a = -a;
                    }
                    const double sweep = q[0] * 2.0 * kPi;
                    field = rr * (sweep <= kPi ? std::min(a, sweep - a) : std::max(a, sweep - 2.0 * kPi - a));
                }
                r = scaled(c, wipeKeep(field, q[5], q[0]));
                break;
            }
            case E::VenetianBlinds: {
                const double a = radians(q[1]);
                const double width = std::max(q[2], 1.0);
                const double along = fx * -std::sin(a) + fy * std::cos(a);
                const double local = along - std::floor(along / width) * width;
                r = scaled(c, wipeKeep(q[0] * width - local, q[3], q[0]));
                break;
            }
            case E::Echo: {
                const Rgba now = scaled(c, q[2]);
                const Rgba echoes = hasSecond ? other : Rgba{};
                const int eop = static_cast<int>(std::floor(q[4] + 0.5));
                if (eop == 0 || eop == 6) {
                    r = {now[0] + echoes[0], now[1] + echoes[1], now[2] + echoes[2], std::min(1.0, now[3] + echoes[3])};
                } else if (eop == 1 || eop == 2) {
                    for (std::size_t i = 0; i < 4; ++i) {
                        r[i] = eop == 1 ? std::max(now[i], echoes[i]) : std::min(now[i], echoes[i]);
                    }
                } else if (eop == 3) {
                    for (std::size_t i = 0; i < 4; ++i) {
                        r[i] = now[i] + echoes[i] - now[i] * echoes[i];
                    }
                } else if (eop == 4) {
                    for (std::size_t i = 0; i < 4; ++i) {
                        r[i] = now[i] + echoes[i] * (1.0 - now[3]);
                    }
                } else {
                    for (std::size_t i = 0; i < 4; ++i) {
                        r[i] = echoes[i] + now[i] * (1.0 - echoes[3]);
                    }
                }
                break;
            }
            case E::TextOverlay: {
                const Rgba text = hasSecond ? other : Rgba{};
                for (std::size_t i = 0; i < 4; ++i) {
                    r[i] = text[i] + c[i] * (1.0 - text[3]);
                }
                break;
            }
            case E::DifferenceMatte: {
                if (!hasSecond) {
                    r = c; // no Difference Layer: nothing keyed
                    break;
                }
                const Rgb e = encoded(c);
                const Rgb o = encoded(hasSecond ? other : Rgba{});
                const double d = std::max({std::abs(e[0] - o[0]), std::abs(e[1] - o[1]), std::abs(e[2] - o[2])});
                const double m = smoothstep(q[3], q[3] + q[4] + 1e-5, d);
                const int view = static_cast<int>(std::floor(q[0] + 0.5));
                if (view == 1) {
                    r = c;
                } else if (view == 2) {
                    const double g = toLinear(m);
                    r = {g, g, g, 1.0};
                } else {
                    r = scaled(c, m);
                }
                break;
            }
            case E::ImageMatteKey: {
                if (!hasSecond) {
                    r = c; // no matte image chosen: nothing keyed
                    break;
                }
                const Rgba matte = hasSecond ? other : Rgba{};
                double m = q[0] > 0.5 ? clamp01(luma(encoded(matte)) * clamp01(matte[3])) : clamp01(matte[3]);
                if (q[1] > 0.5) {
                    m = 1.0 - m;
                }
                r = scaled(c, m);
                break;
            }
            default:
                break;
            }
            out.setD(x, y, r);
        }
    }
    return out;
}

} // namespace wabcut::render::reference::detail
