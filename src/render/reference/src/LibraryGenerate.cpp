// The Generate family's CPU twins (shaders/fx_generate.frag), for render/reference/LibraryEffects.h.

#include "LibraryInternal.h"

#include <wabcut/render/reference/Layout.h>
#include <wabcut/render/reference/LibraryGeometry.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

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

// ---- 0.6: Colorama, Fractal Noise, Cell Pattern, Lightning, Scribble, Paint Bucket ---------------------------------

int roundedMode(double v) { return static_cast<int>(std::floor(v + 0.5)); }
double fract(double v) { return v - std::floor(v); }

/// Hue (turns), lightness and saturation (HLS) of an encoded colour.
std::array<double, 3> hls(const Rgb& e) {
    const double mx = std::max({e[0], e[1], e[2]});
    const double mn = std::min({e[0], e[1], e[2]});
    const double l = 0.5 * (mx + mn);
    const double d = mx - mn;
    if (d <= 0.0) {
        return {0.0, l, 0.0};
    }
    const double s = d / std::max(1.0 - std::abs(2.0 * l - 1.0), 1e-6);
    double h = 0.0;
    if (mx == e[0]) {
        h = (e[1] - e[2]) / d;
    } else if (mx == e[1]) {
        h = (e[2] - e[0]) / d + 2.0;
    } else {
        h = (e[0] - e[1]) / d + 4.0;
    }
    return {fract(h / 6.0), l, clamp01(s)};
}

/// Colorama's preset palettes (Use Preset Palette 1..7): five encoded colours at 0, 1/4, 1/2, 3/4, 1.
constexpr std::array<std::array<Rgb, 5>, 7> kColoramaPalettes{{
    {{{0.0, 0.0, 0.0}, {0.25, 0.25, 0.25}, {0.5, 0.5, 0.5}, {0.75, 0.75, 0.75}, {1.0, 1.0, 1.0}}},     // Ramp Grayscale
    {{{0.0, 0.0, 0.35}, {0.0, 0.45, 0.9}, {0.2, 0.85, 0.45}, {1.0, 0.85, 0.1}, {1.0, 1.0, 1.0}}},     // Ramp Color
    {{{0.1, 0.05, 0.0}, {0.5, 0.3, 0.05}, {0.9, 0.65, 0.2}, {1.0, 0.85, 0.5}, {1.0, 1.0, 0.9}}},      // Golden
    {{{0.0, 0.0, 0.0}, {0.5, 0.0, 0.0}, {1.0, 0.3, 0.0}, {1.0, 0.8, 0.1}, {1.0, 1.0, 0.8}}},          // Fire
    {{{0.0, 0.0, 0.1}, {0.0, 0.2, 0.5}, {0.3, 0.6, 0.9}, {0.7, 0.9, 1.0}, {1.0, 1.0, 1.0}}},          // Ice
    {{{1.0, 1.0, 1.0}, {0.75, 0.75, 0.75}, {0.5, 0.5, 0.5}, {0.25, 0.25, 0.25}, {0.0, 0.0, 0.0}}},     // Negative
    {{{1.0, 0.0, 0.0}, {0.8, 1.0, 0.0}, {0.0, 1.0, 0.6}, {0.3, 0.0, 1.0}, {1.0, 0.0, 0.0}}},          // Rainbow
}};

/// Colorama's output cycle at phase t (0..1), encoded.
Rgb coloramaAt(const LibraryParams& q, double t) {
    std::array<double, 5> pos{};
    std::array<Rgb, 5> col{};
    int n = 5;
    const int palette = std::clamp(roundedMode(q[3]), 0, 7);
    if (palette == 0) {
        n = std::clamp(roundedMode(q[4]), 2, 5);
        for (int k = 0; k < n; ++k) {
            const std::size_t o = 5 + 4 * static_cast<std::size_t>(k);
            pos[static_cast<std::size_t>(k)] = q[o];
            col[static_cast<std::size_t>(k)] = {clamp01(q[o + 1]), clamp01(q[o + 2]), clamp01(q[o + 3])};
        }
        // Stops in order of position (insertion sort: the shader's too).
        for (int i = 1; i < n; ++i) {
            for (int j = i; j > 0 && pos[static_cast<std::size_t>(j)] < pos[static_cast<std::size_t>(j - 1)]; --j) {
                std::swap(pos[static_cast<std::size_t>(j)], pos[static_cast<std::size_t>(j - 1)]);
                std::swap(col[static_cast<std::size_t>(j)], col[static_cast<std::size_t>(j - 1)]);
            }
        }
    } else {
        for (int k = 0; k < 5; ++k) {
            pos[static_cast<std::size_t>(k)] = 0.25 * k;
            col[static_cast<std::size_t>(k)] = kColoramaPalettes[static_cast<std::size_t>(palette - 1)][static_cast<std::size_t>(k)];
        }
    }
    if (t <= pos[0]) {
        return col[0];
    }
    for (int k = 0; k + 1 < n; ++k) {
        const double p0 = pos[static_cast<std::size_t>(k)];
        const double p1 = pos[static_cast<std::size_t>(k + 1)];
        if (t <= p1) {
            const double u = (t - p0) / std::max(p1 - p0, 1e-6);
            const Rgb& a = col[static_cast<std::size_t>(k)];
            const Rgb& b = col[static_cast<std::size_t>(k + 1)];
            if (q[25] < 0.5) {
                return u < 0.5 ? a : b;
            }
            return {a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u};
        }
    }
    return col[static_cast<std::size_t>(n - 1)];
}

// Value noise (fx_generate.frag's twin): lattice values in [-1, 1].
double latticeValue(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed) {
    return 2.0 * libraryHash01(static_cast<std::uint32_t>(x) + 2654435761u * static_cast<std::uint32_t>(y),
                               static_cast<std::uint32_t>(z) + seed * 1013u, 11u) -
           1.0;
}

double catmullRom(double p0, double p1, double p2, double p3, double t) {
    return 0.5 * (2.0 * p1 + (p2 - p0) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t +
                  (3.0 * p1 - p0 - 3.0 * p2 + p3) * t * t * t);
}

/// One layer of noise at lattice depth z, by Noise Type (Block, Linear, Soft Linear, Spline).
double noisePlane(double vx, double vy, std::int32_t z, int type, std::uint32_t seed) {
    const double ix = std::floor(vx);
    const double iy = std::floor(vy);
    const double fx = vx - ix;
    const double fy = vy - iy;
    const auto x = static_cast<std::int32_t>(ix);
    const auto y = static_cast<std::int32_t>(iy);
    if (type == 0) {
        return latticeValue(x, y, z, seed);
    }
    if (type == 3) {
        std::array<double, 4> rows{};
        for (int j = 0; j < 4; ++j) {
            rows[static_cast<std::size_t>(j)] =
                catmullRom(latticeValue(x - 1, y + j - 1, z, seed), latticeValue(x, y + j - 1, z, seed),
                           latticeValue(x + 1, y + j - 1, z, seed), latticeValue(x + 2, y + j - 1, z, seed), fx);
        }
        return std::clamp(catmullRom(rows[0], rows[1], rows[2], rows[3], fy), -1.0, 1.0);
    }
    const double wx = type == 1 ? fx : fx * fx * (3.0 - 2.0 * fx);
    const double wy = type == 1 ? fy : fy * fy * (3.0 - 2.0 * fy);
    const auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    return lerp(lerp(latticeValue(x, y, z, seed), latticeValue(x + 1, y, z, seed), wx),
                lerp(latticeValue(x, y + 1, z, seed), latticeValue(x + 1, y + 1, z, seed), wx), wy);
}

double noise3(double vx, double vy, double vz, int type, int period, std::uint32_t seed) {
    const double iz = std::floor(vz);
    const double fz = vz - iz;
    auto z0 = static_cast<std::int32_t>(iz);
    auto z1 = z0 + 1;
    if (period > 0) {
        z0 = z0 - period * static_cast<std::int32_t>(std::floor(static_cast<double>(z0) / period));
        z1 = z1 - period * static_cast<std::int32_t>(std::floor(static_cast<double>(z1) / period));
    }
    const double wz = fz * fz * (3.0 - 2.0 * fz);
    const double a = noisePlane(vx, vy, z0, type, seed);
    const double b = noisePlane(vx, vy, z1, type, seed);
    return a + (b - a) * wz;
}

/// Contrast, Overflow (Clip, Soft Clamp, Wrap Back) and Invert of a noise value.
double shapeValue(double v, double contrast, double brightness, int overflow, bool invert) {
    v = (v - 0.5) * contrast / 100.0 + 0.5 + brightness / 100.0;
    if (overflow == 1) {
        v = 1.0 / (1.0 + std::exp(-4.0 * (v - 0.5)));
    } else if (overflow == 2) {
        const double w = v - 2.0 * std::floor(v / 2.0);
        v = w <= 1.0 ? w : 2.0 - w;
    } else {
        v = clamp01(v);
    }
    return invert ? 1.0 - v : v;
}

/// A grey value drawn over the layer at Opacity with a Blending Mode (None: the grey alone).
Rgba drawGrey(int mode, const Rgba& dst, double v, double opacity) {
    const double g = toLinear(clamp01(v));
    const double k = clamp01(opacity);
    const Rgba src{g * k, g * k, g * k, k};
    return mode == 0 ? src : blendOver(mode, dst, src);
}

double fractalNoiseAt(const LibraryParams& q, double fx, double fy, double sx, double sy) {
    const double cell = std::max(q[7], 1.0);
    const double theta = radians(q[6]);
    const double px = fx - q[8] * sx;
    const double py = fy - q[9] * sy;
    const double vx = (std::cos(theta) * px + std::sin(theta) * py) / cell;
    const double vy = (-std::sin(theta) * px + std::cos(theta) * py) / cell;
    const int period = q[15] > 0.5 ? std::max(1, roundedMode(q[16])) : 0;
    double z = q[14] / 360.0;
    if (period > 0) {
        z = z - period * std::floor(z / period);
    }
    const int fractal = std::clamp(roundedMode(q[0]), 0, 3);
    const int type = std::clamp(roundedMode(q[1]), 0, 3);
    const double complexity = std::clamp(q[10], 1.0, 20.0);
    const int octaves = static_cast<int>(std::floor(complexity));
    const double last = complexity - octaves;
    const double influence = clamp01(q[11]);
    const double subScale = 1.0 / std::clamp(q[12], 0.25, 1.0);
    const double subRotation = radians(q[13]);
    const auto seed = static_cast<std::uint32_t>(std::clamp(std::lround(q[17]), 0L, 1000000L));
    double sum = 0.0;
    double norm = 0.0;
    double amp = 1.0;
    double freq = 1.0;
    for (int o = 0; o < 21; ++o) {
        const double weight = o < octaves ? 1.0 : (o == octaves ? last : 0.0);
        if (weight <= 0.0) {
            break;
        }
        const double r = subRotation * o;
        double ox = (std::cos(r) * vx + std::sin(r) * vy) * freq + 7.31 * o;
        double oy = (-std::sin(r) * vx + std::cos(r) * vy) * freq + 3.17 * o;
        if (fractal == 2) { // Dynamic: the finer layers drift as it evolves
            ox += 0.37 * o * z;
            oy += 0.21 * o * z;
        }
        double n = noise3(ox, oy, z, type, period, seed + static_cast<std::uint32_t>(o) * 101u);
        if (fractal == 1) {
            n = std::abs(n);
        } else if (fractal == 3) {
            n = n * n;
        }
        sum += weight * amp * n;
        norm += weight * amp;
        amp *= influence;
        freq *= subScale;
    }
    const double value = norm > 0.0 ? sum / norm : 0.0;
    const double v = fractal == 1 || fractal == 3 ? value : 0.5 + 0.5 * value;
    return shapeValue(v, q[3], q[4], std::clamp(roundedMode(q[5]), 0, 2), q[2] > 0.5);
}

double cellPatternAt(const LibraryParams& q, double fx, double fy, double sx, double sy) {
    constexpr double kTwoPi = 2.0 * kPi;
    const double size = std::max(q[5], 2.0);
    const double ux = (fx - q[6] * sx) / size;
    const double uy = (fy - q[7] * sy) / size;
    const double ix = std::floor(ux);
    const double iy = std::floor(uy);
    const double disperse = std::clamp(q[4], 0.0, 1.5);
    const double evolution = radians(q[9]);
    const bool cycle = q[10] > 0.5;
    const double cycleSpeed = 1.0 / std::max(1, roundedMode(q[11]));
    const auto seed = static_cast<std::uint32_t>(std::clamp(std::lround(q[12]), 0L, 1000000L));
    double f1 = 1e9;
    double f2 = 1e9;
    double nearHash = 0.0;
    double nearStatic = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const auto nx = static_cast<std::int32_t>(ix) + dx;
            const auto ny = static_cast<std::int32_t>(iy) + dy;
            const std::uint32_t key = static_cast<std::uint32_t>(nx) + 2654435761u * static_cast<std::uint32_t>(ny);
            const double h1 = libraryHash01(key, seed * 1013u + 5u, 21u);
            const double h2 = libraryHash01(key, seed * 1013u + 6u, 21u);
            const double h3 = libraryHash01(key, seed * 1013u + 7u, 21u);
            const double h4 = libraryHash01(key, seed * 1013u + 8u, 21u);
            const double a = kTwoPi * h3 + evolution * (cycle ? cycleSpeed : 0.75 + 0.5 * h4);
            const double px = nx + 0.5 + disperse * (h1 - 0.5) + 0.25 * disperse * std::cos(a);
            const double py = ny + 0.5 + disperse * (h2 - 0.5) + 0.25 * disperse * std::sin(a);
            const double d = std::hypot(ux - px, uy - py);
            if (d < f1) {
                f2 = f1;
                f1 = d;
                nearHash = h3;
                nearStatic = h4;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
    const double edge = f2 - f1;
    const double r = f1 / std::max(0.5 * (f1 + f2), 1e-6);
    double v = 0.0;
    switch (std::clamp(roundedMode(q[0]), 0, 6)) {
    case 0: v = std::sqrt(std::max(0.0, 1.0 - r * r)); break;                       // Bubbles
    case 1: v = clamp01(edge); break;                                               // Crystals
    case 2: v = 0.5 + 0.5 * std::sin(kTwoPi * nearHash + evolution); break;         // Plates
    case 3: v = nearStatic; break;                                                  // Static Plates
    case 4: v = nearStatic * (0.4 + 0.6 * clamp01(edge * 4.0)); break;              // Crystallize
    case 5: v = clamp01(2.0 * edge / std::max(f2, 1e-6)); break;                    // Pillow
    default: v = 1.0 - clamp01(edge * 3.0); break;                                  // Tubular
    }
    if (q[8] > 0.0) {
        v *= smoothstep(0.0, 0.5 * q[8], edge);
    }
    return shapeValue(v, q[2], 0.0, std::clamp(roundedMode(q[3]), 0, 2), q[1] > 0.5);
}

/// Paint Bucket: the flood fill as the compositor's passes compute it (run lengths along the four directions by
/// doubling, then 24 propagation passes jumping 1, 2, 4 ... 1024 pixels along a run), then the fill composited.
Image paintBucket(const Image& in, const LibraryParams& q) {
    const int w = in.width;
    const int h = in.height;
    const auto index = [&](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); };
    const int seedX = static_cast<int>(std::floor(q[0] * w));
    const int seedY = static_cast<int>(std::floor(q[1] * h));
    const bool seedInside = seedX >= 0 && seedY >= 0 && seedX < w && seedY < h;
    const Rgba seedColor = seedInside ? in.atD(seedX, seedY) : Rgba{};
    const Rgb seedE = encoded(seedColor);
    const double seedA = clamp01(seedColor[3]);
    const int selector = std::clamp(roundedMode(q[2]), 0, 4);
    std::vector<std::array<double, 4>> runs(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    std::vector<std::uint8_t> similar(runs.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Rgba c = in.atD(x, y);
            const Rgb e = encoded(c);
            const double a = clamp01(c[3]);
            const double dc = std::max({std::abs(e[0] - seedE[0]), std::abs(e[1] - seedE[1]), std::abs(e[2] - seedE[2])});
            const double d = selector == 0 ? std::max(dc, std::abs(a - seedA))
                             : selector == 1 ? dc
                             : selector == 2 ? a
                             : selector == 3 ? 1.0 - a
                                             : std::abs(a - seedA);
            const bool s = seedInside && d * 255.0 <= q[3];
            similar[index(x, y)] = s ? 1 : 0;
            runs[index(x, y)] = s ? std::array<double, 4>{1.0, 1.0, 1.0, 1.0} : std::array<double, 4>{};
        }
    }
    constexpr std::array<std::array<int, 2>, 4> kDirs{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
    for (int k = 0; k <= 10; ++k) {
        const int step = 1 << k;
        std::vector<std::array<double, 4>> next(runs.size());
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                for (std::size_t c = 0; c < 4; ++c) {
                    const double r = runs[index(x, y)][c];
                    double out = r;
                    if (r >= step) {
                        const int nx = x + kDirs[c][0] * step;
                        const int ny = y + kDirs[c][1] * step;
                        out = step + (nx >= 0 && ny >= 0 && nx < w && ny < h ? runs[index(nx, ny)][c] : 0.0);
                    }
                    next[index(x, y)][c] = out;
                }
            }
        }
        runs = std::move(next);
    }
    std::vector<std::uint8_t> filled(runs.size(), 0);
    if (seedInside && similar[index(seedX, seedY)]) {
        filled[index(seedX, seedY)] = 1;
    }
    for (int pass = 0; pass < 24; ++pass) {
        std::vector<std::uint8_t> next = filled;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const auto& r = runs[index(x, y)];
                if (filled[index(x, y)] || r[0] <= 0.0) {
                    continue;
                }
                bool hit = false;
                for (std::size_t c = 0; c < 4 && !hit; ++c) {
                    for (int j = 0; j <= 10 && !hit; ++j) {
                        const int s = 1 << j;
                        if (r[c] <= s) {
                            break;
                        }
                        const int nx = x + kDirs[c][0] * s;
                        const int ny = y + kDirs[c][1] * s;
                        hit = nx >= 0 && ny >= 0 && nx < w && ny < h && filled[index(nx, ny)];
                    }
                }
                next[index(x, y)] = hit ? 1 : 0;
            }
        }
        filled = std::move(next);
    }
    Image mask(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double m = filled[index(x, y)] ? 1.0 : 0.0;
            if (q[11] > 0.5) {
                m = 1.0 - m;
            }
            mask.setD(x, y, {m, m, m, m});
        }
    }
    const int stroke = std::clamp(roundedMode(q[5]), 0, 4);
    const double sigma = stroke == 0 ? 0.75 : stroke == 1 ? std::max(q[7], 0.0) / 2.0 : std::max(q[6], 0.5) / 2.0;
    const Image soft = gaussianBlurSigma(mask, sigma, true);
    const Rgb fill = srgb3(q[8], q[9], q[10]);
    const int mode = std::clamp(roundedMode(q[13]), 0, 5);
    Image out(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Rgba c = in.atD(x, y);
            if (q[4] > 0.5) { // View Threshold: what the fill selects, white on black
                const double v = similar[index(x, y)] ? 1.0 : 0.0;
                out.setD(x, y, {v, v, v, 1.0});
                continue;
            }
            const double b = clamp01(soft.atD(x, y)[3]);
            double m = b;
            if (stroke == 2) {
                m = smoothstep(0.02, 0.3, b);
            } else if (stroke == 3) {
                m = smoothstep(0.7, 0.98, b);
            } else if (stroke == 4) {
                m = smoothstep(0.05, 0.3, b) * (1.0 - smoothstep(0.7, 0.95, b));
            }
            const double k = m * clamp01(q[12]);
            const Rgba f{fill[0] * k, fill[1] * k, fill[2] * k, k};
            Rgba r{};
            switch (mode) {
            case 0: r = {f[0] + c[0] * (1.0 - k), f[1] + c[1] * (1.0 - k), f[2] + c[2] * (1.0 - k), f[3] + c[3] * (1.0 - k)}; break;
            case 1: r = {c[0] + f[0] * (1.0 - c[3]), c[1] + f[1] * (1.0 - c[3]), c[2] + f[2] * (1.0 - c[3]), c[3] + f[3] * (1.0 - c[3])}; break;
            case 2: r = {c[0] + f[0], c[1] + f[1], c[2] + f[2], std::min(1.0, c[3] + f[3])}; break;
            case 3: r = blendOver(3, c, f); break;
            case 4: r = blendOver(4, c, f); break;
            default: r = f; break;
            }
            out.setD(x, y, r);
        }
    }
    return out;
}

/// Lightning or Scribble: their strokes (LibraryGeometry.h) shaded and composited.
Image strokes(const Image& in, LibraryEffect op, const LibraryParams& q, std::span<const LibraryPath> paths) {
    const double sx = in.width;
    const double sy = in.height;
    const bool lightning = op == LibraryEffect::Lightning;
    const std::vector<StrokeSegment> segments = lightning ? lightningStrokes(q, sx, sy) : scribbleStrokes(q, paths, sx, sy);
    const StrokeKind kind = lightning ? StrokeKind::Glow : StrokeKind::Line;
    const double core = lightning ? q[19] : 0.0;
    Image out(in.width, in.height);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            const Rgba c = in.atD(x, y);
            const auto shade = strokeShade(segments, kind, core, x + 0.5, y + 0.5);
            Rgba r{};
            if (lightning) {
                const Rgb outside = srgb3(q[20], q[21], q[22]);
                const Rgb inside = srgb3(q[23], q[24], q[25]);
                const double ki = shade[0];
                const double ko = shade[1] * (1.0 - ki);
                const Rgba bolt{inside[0] * ki + outside[0] * ko, inside[1] * ki + outside[1] * ko,
                                inside[2] * ki + outside[2] * ko, ki + ko};
                const int mode = std::clamp(roundedMode(q[28]), 0, 2);
                for (std::size_t i = 0; i < 4; ++i) {
                    r[i] = mode == 0 ? bolt[i] + c[i] * (1.0 - bolt[3])
                           : mode == 1 ? (i < 3 ? c[i] + bolt[i] : std::min(1.0, c[i] + bolt[i]))
                                       : c[i] + bolt[i] - c[i] * bolt[i];
                }
            } else {
                const Rgb color = srgb3(q[3], q[4], q[5]);
                const double k = shade[0] * clamp01(q[6]);
                const Rgba s{color[0] * k, color[1] * k, color[2] * k, k};
                const int composite = std::clamp(roundedMode(q[18]), 0, 2);
                for (std::size_t i = 0; i < 4; ++i) {
                    r[i] = composite == 0 ? s[i] + c[i] * (1.0 - k) : composite == 1 ? s[i] : c[i] * k;
                }
            }
            out.setD(x, y, r);
        }
    }
    return out;
}

} // namespace

bool isGenerate(LibraryEffect effect) {
    return static_cast<int>(effect) >= 100;
}

Image generate(const Image& in, LibraryEffect op, const LibraryParams& q, const Image* second,
               std::span<const LibraryPath> paths) {
    if (op == E::PaintBucket) {
        return paintBucket(in, q);
    }
    if (op == E::Lightning || op == E::Scribble) {
        return strokes(in, op, q, paths);
    }
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
            case E::Colorama: {
                const Rgb e = encoded(c);
                const double a = clamp01(c[3]);
                const auto [hue, light, sat] = hls(e);
                double v = 0.0;
                switch (std::clamp(roundedMode(q[0]), 0, 7)) {
                case 0: v = (e[0] + e[1] + e[2]) / 3.0; break;
                case 1: v = light; break;
                case 2: v = hue; break;
                case 3: v = sat; break;
                case 4: v = e[0]; break;
                case 5: v = e[1]; break;
                case 6: v = e[2]; break;
                default: v = a; break;
                }
                const double phase = v * std::max(q[2], 0.1) + q[1] / 360.0;
                double t = fract(phase);
                if (phase > 0.0 && t < 1e-6) {
                    t = 1.0; // a whole number of cycles ends the cycle (white stays at the end of a ramp)
                }
                const Rgb m = coloramaAt(q, t);
                const Rgba mapped{toLinear(m[0]) * a, toLinear(m[1]) * a, toLinear(m[2]) * a, a};
                const int mode = std::clamp(roundedMode(q[27]), 0, 7);
                const Rgba blended = mode == 0 ? mapped : blendOver(mode + 1, c, mapped);
                r = mixC(blended, c, clamp01(q[26]));
                break;
            }
            case E::FractalNoise:
                r = drawGrey(std::clamp(roundedMode(q[19]), 0, 8), c, fractalNoiseAt(q, fx, fy, sx, sy), q[18]);
                break;
            case E::CellPattern:
                r = drawGrey(std::clamp(roundedMode(q[14]), 0, 8), c, cellPatternAt(q, fx, fy, sx, sy), q[13]);
                break;
            default:
                break;
            }
            out.setD(x, y, r);
        }
    }
    return out;
}

} // namespace wabcut::render::reference::detail
