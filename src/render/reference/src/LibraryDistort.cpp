// The Distort family's CPU twins (shaders/fx_distort.frag), for render/reference/LibraryEffects.h.

#include "LibraryInternal.h"

#include <wabcut/render/reference/Transitions.h>

#include <algorithm>
#include <cmath>

namespace wabcut::render::reference::detail {

namespace {

using E = LibraryEffect;

constexpr double kPi = 3.14159265358979;

struct V2 {
    double x = 0.0;
    double y = 0.0;
};
V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
V2 operator*(V2 a, double k) { return {a.x * k, a.y * k}; }
V2 operator*(V2 a, V2 b) { return {a.x * b.x, a.y * b.y}; }
V2 operator/(V2 a, V2 b) { return {a.x / b.x, a.y / b.y}; }
double dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
double length(V2 a) { return std::sqrt(dot(a, a)); }

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double radians(double d) { return d * kPi / 180.0; }
double fract(double v) { return v - std::floor(v); }
double glslMod(double x, double y) { return x - y * std::floor(x / y); }

Rgba mixC(const Rgba& a, const Rgba& b, double t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t};
}
Rgba scaled(const Rgba& a, double k) { return {a[0] * k, a[1] * k, a[2] * k, a[3] * k}; }
Rgba sum(const Rgba& a, const Rgba& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]}; }

double srgbLinear(double v) { return srgbToLinear(v); }

V2 rotateCw(V2 v, double a) { return {v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a)}; }

/// A distortion pass over a picture that fills the canvas (frame = canvas, identity Motion, divisor 1).
struct Ctx {
    const Image& in;
    V2 size;

    Rgba sampleRaw(V2 f) const { return sampleBilinear(in, f.x, f.y); }
    double frameCoverage(V2 f) const {
        return clamp01(std::min(std::min(f.x, size.x - f.x), std::min(f.y, size.y - f.y)) + 0.5);
    }
    Rgba sampleFrame(V2 f) const { return scaled(sampleRaw(f), frameCoverage(f)); }
    Rgba sampleCanvas(V2 c) const {
        if (c.x < 0.0 || c.y < 0.0 || c.x > size.x || c.y > size.y) {
            return {};
        }
        return sampleRaw(c);
    }
};

// ---- Corner Pin ----

struct Rows {
    std::array<double, 3> r0{}, r1{}, r2{};
};

Rows squareToQuad(V2 p0, V2 p1, V2 p2, V2 p3) {
    const V2 d1 = p1 - p2;
    const V2 d2 = p3 - p2;
    const V2 d3 = p0 - p1 + p2 - p3;
    Rows m;
    if (d3.x == 0.0 && d3.y == 0.0) {
        m.r0 = {p1.x - p0.x, p2.x - p1.x, p0.x};
        m.r1 = {p1.y - p0.y, p2.y - p1.y, p0.y};
        m.r2 = {0.0, 0.0, 1.0};
        return m;
    }
    const double den = d1.x * d2.y - d2.x * d1.y;
    const double g = (d3.x * d2.y - d2.x * d3.y) / den;
    const double h = (d1.x * d3.y - d3.x * d1.y) / den;
    m.r0 = {p1.x - p0.x + g * p1.x, p3.x - p0.x + h * p3.x, p0.x};
    m.r1 = {p1.y - p0.y + g * p1.y, p3.y - p0.y + h * p3.y, p0.y};
    m.r2 = {g, h, 1.0};
    return m;
}

std::array<double, 3> quadToSquare(const Rows& m, V2 f) {
    const double a = m.r0[0], b = m.r0[1], c = m.r0[2];
    const double d = m.r1[0], e = m.r1[1], ff = m.r1[2];
    const double g = m.r2[0], h = m.r2[1];
    const std::array<double, 3> u{e - ff * h, c * h - b, b * ff - c * e};
    const std::array<double, 3> v{ff * g - d, a - c * g, c * d - a * ff};
    const std::array<double, 3> w{d * h - e * g, b * g - a * h, a * e - b * d};
    const double ww = w[0] * f.x + w[1] * f.y + w[2];
    return {(u[0] * f.x + u[1] * f.y + u[2]) / ww, (v[0] * f.x + v[1] * f.y + v[2]) / ww, ww};
}

Rgba cornerPin(const Ctx& c, const LibraryParams& q, V2 f) {
    const V2 s = c.size;
    const Rows m = squareToQuad(V2{q[0], q[1]} * s, V2{q[2], q[3]} * s, V2{q[6], q[7]} * s, V2{q[4], q[5]} * s);
    const auto uv = quadToSquare(m, f);
    const auto ux = quadToSquare(m, f + V2{1.0, 0.0});
    const auto uy = quadToSquare(m, f + V2{0.0, 1.0});
    if (uv[2] <= 0.0) {
        return {};
    }
    const double gu = std::max(std::hypot(ux[0] - uv[0], uy[0] - uv[0]), 1e-6);
    const double gv = std::max(std::hypot(ux[1] - uv[1], uy[1] - uv[1]), 1e-6);
    const double edge = std::min(std::min(uv[0], 1.0 - uv[0]) / gu, std::min(uv[1], 1.0 - uv[1]) / gv);
    return scaled(c.sampleRaw(V2{uv[0], uv[1]} * s), clamp01(edge + 0.5));
}

// ---- Magnify's blending ----

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

// ---- Waves and noise ----

double waveOf(int type, double arg) {
    const double t = fract(arg / (2.0 * kPi));
    switch (type) {
    case 1: return t < 0.5 ? 1.0 : -1.0;
    case 2: return t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t;
    case 3: return 2.0 * t - 1.0;
    case 4:
        return t < 0.5 ? std::sqrt(std::max(0.0, 1.0 - (4.0 * t - 1.0) * (4.0 * t - 1.0)))
                       : -std::sqrt(std::max(0.0, 1.0 - (4.0 * t - 3.0) * (4.0 * t - 3.0)));
    case 5: return 2.0 * std::sqrt(std::max(0.0, 1.0 - (2.0 * t - 1.0) * (2.0 * t - 1.0))) - 1.0;
    case 6: return 1.0 - 2.0 * std::sqrt(std::max(0.0, 1.0 - (2.0 * t - 1.0) * (2.0 * t - 1.0)));
    case 7:
    case 8: {
        const int k = static_cast<int>(std::floor(arg / (2.0 * kPi)));
        const double a = 2.0 * libraryHash01(static_cast<std::uint32_t>(k), 7u, 0u) - 1.0;
        if (type == 7) {
            return a;
        }
        const double b = 2.0 * libraryHash01(static_cast<std::uint32_t>(k + 1), 7u, 0u) - 1.0;
        return a + (b - a) * (t * t * (3.0 - 2.0 * t));
    }
    default: return std::sin(arg);
    }
}

double pinWeight(int pin, int set, V2 f, V2 s) {
    const double ramp = 0.1 * std::min(s.x, s.y);
    const double l = f.x;
    const double r = s.x - f.x;
    const double t = f.y;
    const double b = s.y - f.y;
    double d = 1e9;
    if (set == 0) {
        switch (pin) {
        case 1: d = std::min(std::min(l, r), std::min(t, b)); break;
        case 2: d = length(f - s * 0.5); break;
        case 3: d = l; break;
        case 4: d = t; break;
        case 5: d = r; break;
        case 6: d = b; break;
        case 7: d = std::min(t, b); break;
        case 8: d = std::min(l, r); break;
        default: break;
        }
    } else {
        switch (pin) {
        case 1: d = std::min(std::min(l, r), std::min(t, b)); break;
        case 2: d = std::min(t, b); break;
        case 3: d = std::min(l, r); break;
        case 4: d = l; break;
        case 5: d = t; break;
        case 6: d = r; break;
        case 7: d = b; break;
        default: break;
        }
    }
    return clamp01(d / ramp);
}

double valueAt(int x, int y, int z, std::uint32_t seed) {
    return 2.0 * libraryHash01(static_cast<std::uint32_t>(x) + 2654435761u * static_cast<std::uint32_t>(y),
                               static_cast<std::uint32_t>(z) + seed * 1013u, 11u) -
           1.0;
}

double noise3(double vx, double vy, double vz, int period, std::uint32_t seed) {
    const double ix = std::floor(vx);
    const double iy = std::floor(vy);
    const double iz = std::floor(vz);
    const double fx = vx - ix;
    const double fy = vy - iy;
    const double fz = vz - iz;
    const double wx = fx * fx * (3.0 - 2.0 * fx);
    const double wy = fy * fy * (3.0 - 2.0 * fy);
    const double wz = fz * fz * (3.0 - 2.0 * fz);
    const int x = static_cast<int>(ix);
    const int y = static_cast<int>(iy);
    int z0 = static_cast<int>(iz);
    int z1 = z0 + 1;
    if (period > 0) {
        z0 = z0 - period * static_cast<int>(std::floor(static_cast<double>(z0) / period));
        z1 = z1 - period * static_cast<int>(std::floor(static_cast<double>(z1) / period));
    }
    const auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    const double a = lerp(lerp(valueAt(x, y, z0, seed), valueAt(x + 1, y, z0, seed), wx),
                          lerp(valueAt(x, y + 1, z0, seed), valueAt(x + 1, y + 1, z0, seed), wx), wy);
    const double b = lerp(lerp(valueAt(x, y, z1, seed), valueAt(x + 1, y, z1, seed), wx),
                          lerp(valueAt(x, y + 1, z1, seed), valueAt(x + 1, y + 1, z1, seed), wx), wy);
    return lerp(a, b, wz);
}

double fbm(V2 v, double z, double complexity, int period, std::uint32_t seed) {
    double total = 0.0;
    double norm = 0.0;
    double amp = 1.0;
    double freq = 1.0;
    const int octaves = static_cast<int>(std::floor(complexity));
    const double last = complexity - octaves;
    for (int o = 0; o < 11; ++o) {
        const double weight = o < octaves ? 1.0 : (o == octaves ? last : 0.0);
        if (weight <= 0.0) {
            break;
        }
        total += weight * amp * noise3(v.x * freq, v.y * freq, z, period, seed + static_cast<std::uint32_t>(o) * 101u);
        norm += weight * amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return norm > 0.0 ? total / norm : 0.0;
}

// ---- Transform ----

V2 transformInverse(V2 f, V2 anchor, V2 position, V2 scale, double skew, double axis, double rotation) {
    V2 v = rotateCw(f - position, -rotation);
    v = rotateCw(v, -axis);
    v.x -= skew * v.y;
    v = rotateCw(v, axis);
    return anchor + v / scale;
}

Rgba transformAt(const Ctx& c, const LibraryParams& q, V2 f, double k) {
    const V2 s = c.size;
    const bool end = q[23] > 0.5;
    V2 anchor{q[0], q[1]};
    V2 position{q[2], q[3]};
    double sh = q[5];
    double sw = q[4] > 0.5 ? q[5] : q[6];
    double skew = q[7];
    double axis = q[8];
    double rotation = q[9];
    if (end) {
        const auto lerp = [k](double a, double b) { return a + (b - a) * k; };
        anchor = {lerp(anchor.x, q[14]), lerp(anchor.y, q[15])};
        position = {lerp(position.x, q[16]), lerp(position.y, q[17])};
        sh = lerp(sh, q[18]);
        sw = lerp(sw, q[4] > 0.5 ? q[18] : q[19]);
        skew = lerp(skew, q[20]);
        axis = lerp(axis, q[21]);
        rotation = lerp(rotation, q[22]);
    }
    const V2 scale{sw / 100.0, sh / 100.0};
    if (std::abs(scale.x) < 1e-6 || std::abs(scale.y) < 1e-6) {
        return {};
    }
    const V2 src = transformInverse(f, anchor * s, position * s, scale, std::tan(radians(skew)), radians(axis), radians(rotation));
    return c.sampleFrame(src);
}

// ---- Basic 3D ----

struct V3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 normalize3(V3 a) { return a * (1.0 / std::sqrt(dot3(a, a))); }
V3 rotX(V3 v, double a) { return {v.x, v.y * std::cos(a) - v.z * std::sin(a), v.y * std::sin(a) + v.z * std::cos(a)}; }
V3 rotY(V3 v, double a) { return {v.x * std::cos(a) + v.z * std::sin(a), v.y, -v.x * std::sin(a) + v.z * std::cos(a)}; }

Rgba basic3d(const Ctx& c, const LibraryParams& q, V2 f) {
    const V2 s = c.size;
    const V2 ctr = s * 0.5;
    const double focal = 1.6 * std::max(s.x, s.y);
    const double swivel = radians(q[0]);
    const double tilt = radians(q[1]);
    const V3 ex = rotY(rotX({1.0, 0.0, 0.0}, tilt), swivel);
    const V3 ey = rotY(rotX({0.0, 1.0, 0.0}, tilt), swivel);
    const V3 n = rotY(rotX({0.0, 0.0, 1.0}, tilt), swivel);
    const V3 p0{ctr.x, ctr.y, q[2] / 100.0 * focal};
    const V3 cam{ctr.x, ctr.y, -focal};
    const V3 d{f.x - ctr.x, f.y - ctr.y, focal};
    const double den = dot3(d, n);
    if (std::abs(den) < 1e-6) {
        return {};
    }
    const double t = dot3(p0 - cam, n) / den;
    if (t <= 0.0) {
        return {};
    }
    const V3 x = cam + d * t;
    const V2 src = ctr + V2{dot3(x - p0, ex), dot3(x - p0, ey)};
    Rgba color = c.sampleFrame(src);
    if (q[3] > 0.5) {
        const V3 nn = dot3(n, d) > 0.0 ? n * -1.0 : n;
        const V3 toLight = normalize3({-1.0, -1.0, -1.0});
        const V3 toEye = normalize3(cam - x);
        // reflect(-L, N) = -L - 2 dot(N, -L) N
        const V3 l = toLight * -1.0;
        const V3 r = l - nn * (2.0 * dot3(nn, l));
        const double spec = std::pow(std::max(0.0, dot3(r, toEye)), 40.0);
        for (std::size_t i = 0; i < 3; ++i) {
            color[i] += 0.8 * spec * color[3];
        }
    }
    return color;
}

} // namespace

bool isDistort(LibraryEffect effect) {
    const int v = static_cast<int>(effect);
    return v >= 40 && v < 70;
}

Rgba distortPixel(const Image& in, LibraryEffect op, const LibraryParams& q, int px, int py) {
    const Ctx c{in, {static_cast<double>(in.width), static_cast<double>(in.height)}};
    const V2 s = c.size;
    const V2 f{px + 0.5, py + 0.5}; // frame = canvas
    const Rgba orig = in.atD(px, py);
    switch (op) {
    case E::CornerPin:
        return cornerPin(c, q, f);
    case E::LensDistortion: {
        const V2 center = s * 0.5 * V2{1.0 + q[2] / 100.0, 1.0 + q[1] / 100.0};
        const double radius = 0.5 * length(s);
        const V2 u = (f - center) * (1.0 / radius);
        const double r2 = dot(u, u);
        const V2 su = u * (1.0 - q[0] / 100.0 * 0.6 * r2) + V2{q[4], q[3]} * (0.25 * r2 / 100.0);
        const V2 src = center + su * radius;
        const Rgba fill = q[5] > 0.5 ? Rgba{} : Rgba{srgbLinear(q[6]), srgbLinear(q[7]), srgbLinear(q[8]), 1.0};
        return mixC(fill, c.sampleRaw(src), c.frameCoverage(src));
    }
    case E::Magnify: {
        const double m = std::max(q[3] / 100.0, 0.01);
        const int link = static_cast<int>(std::floor(q[4] + 0.5));
        const double size = q[5] * (link >= 1 ? m : 1.0);
        const double feather = q[6] * (link >= 2 ? m : 1.0);
        const V2 center = V2{q[1], q[2]} * s;
        const V2 d = f - center;
        const double dist = q[0] < 0.5 ? length(d) : std::max(std::abs(d.x), std::abs(d.y));
        const double cov = feather > 0.0 ? clamp01((size - dist) / feather) : clamp01(size - dist + 0.5);
        const Rgba lens = blendOver(static_cast<int>(std::floor(q[9] + 0.5)), orig, c.sampleFrame(center + d * (1.0 / m)));
        return mixC(orig, lens, cov * q[7]);
    }
    case E::Mirror: {
        const V2 center = V2{q[0], q[1]} * s;
        const double a = radians(q[2]);
        const V2 n{std::cos(a), std::sin(a)};
        const double d = dot(f - center, n);
        return d > 0.0 ? c.sampleFrame(f - n * (2.0 * d)) : c.sampleFrame(f);
    }
    case E::Offset: {
        const V2 shift = (V2{q[0], q[1]} - V2{0.5, 0.5}) * s;
        const V2 src{glslMod(f.x - shift.x, s.x), glslMod(f.y - shift.y, s.y)};
        return mixC(scaled(c.sampleRaw(src), c.frameCoverage(f)), orig, q[2]);
    }
    case E::Spherize: {
        const V2 center = V2{q[1], q[2]} * s;
        const V2 d = f - center;
        const double r = q[0] > 0.0 ? length(d) / q[0] : 1.0;
        V2 src = f;
        if (r < 1.0 && r > 0.0) {
            src = center + d * (std::asin(r) / (r * 0.5 * kPi));
        } else if (r == 0.0) {
            src = center;
        }
        return c.sampleFrame(src);
    }
    case E::Twirl: {
        const double radius = q[1] / 100.0 * std::max(s.x, s.y);
        const V2 center = V2{q[2], q[3]} * s;
        const V2 d = f - center;
        const double len = length(d);
        V2 src = f;
        if (len < radius) {
            const double k = 1.0 - len / radius;
            src = center + rotateCw(d, -radians(q[0]) * k * k);
        }
        return c.sampleFrame(src);
    }
    case E::WaveWarp: {
        const double a = radians(q[3]);
        const V2 dir{std::sin(a), -std::cos(a)};
        const V2 perp{-dir.y, dir.x};
        const double arg = dot(f, dir) / std::max(q[2], 1.0) * 2.0 * kPi - q[4] * q[31] * 2.0 * kPi + radians(q[6]);
        const V2 disp = perp * (q[1] * waveOf(static_cast<int>(std::floor(q[0] + 0.5)), arg) *
                                pinWeight(static_cast<int>(std::floor(q[5] + 0.5)), 0, f, s));
        return c.sampleFrame(f - disp);
    }
    case E::TurbulentDisplace: {
        const int type = static_cast<int>(std::floor(q[0] + 0.5));
        const double amount = q[1];
        const double size = std::max(q[2], 1.0);
        const V2 v = (f - (V2{q[3], q[4]} - V2{0.5, 0.5}) * s) * (1.0 / size);
        double z = q[6] / 360.0;
        const int period = q[7] > 0.5 ? std::max(1, static_cast<int>(std::floor(q[8] + 0.5))) : 0;
        if (period > 0) {
            z = glslMod(z, period);
        }
        const auto seed = static_cast<std::uint32_t>(std::max(0.0, q[9]));
        const double complexity = (type >= 3 && type <= 5) ? 1.0 : q[5];
        const double n1 = fbm(v, z, complexity, period, seed);
        const double n2 = fbm(v + V2{31.7, 17.3}, z + 5.1, complexity, period, seed);
        const V2 center = s * 0.5;
        V2 src;
        if (type == 1 || type == 4) {
            src = center + (f - center) * (1.0 + 0.002 * amount * n1);
        } else if (type == 2 || type == 5) {
            src = center + rotateCw(f - center, 0.005 * amount * n1);
        } else if (type == 6) {
            src = f + V2{0.0, amount * n2};
        } else if (type == 7) {
            src = f + V2{amount * n1, 0.0};
        } else if (type == 8) {
            src = f + V2{amount * n1, amount * n1};
        } else {
            src = f + V2{n1, n2} * amount;
        }
        src = f + (src - f) * pinWeight(static_cast<int>(std::floor(q[10] + 0.5)), 1, f, s);
        return c.sampleFrame(src);
    }
    case E::Transform: {
        Rgba acc = transformAt(c, q, f, 0.0);
        if (q[23] > 0.5 && q[11] < 0.5 && q[12] > 0.0) {
            acc = {};
            for (int i = 0; i < 16; ++i) {
                acc = sum(acc, transformAt(c, q, f, i / 15.0));
            }
            acc = scaled(acc, 1.0 / 16.0);
        }
        return scaled(acc, q[10]);
    }
    case E::HorizontalFlip:
        return c.sampleFrame({s.x - f.x, f.y});
    case E::VerticalFlip:
        return c.sampleFrame({f.x, s.y - f.y});
    case E::EdgeFeather: {
        const double w = q[0] / 100.0 * 0.5 * std::min(s.x, s.y);
        const double m = w > 0.0 ? clamp01(std::min(std::min(f.x, s.x - f.x), std::min(f.y, s.y - f.y)) / w) : 1.0;
        return scaled(orig, m);
    }
    case E::Replicate: {
        const double n = std::clamp(std::floor(q[0] + 0.5), 2.0, 16.0);
        const V2 src{fract(f.x / s.x * n) * s.x, fract(f.y / s.y * n) * s.y};
        return scaled(c.sampleRaw(src), c.frameCoverage(f));
    }
    case E::Basic3D:
        return basic3d(c, q, f);
    case E::BevelEdges: {
        const double band = q[0] * 0.5 * std::min(s.x, s.y);
        const double l = f.x;
        const double r = s.x - f.x;
        const double t = f.y;
        const double b = s.y - f.y;
        const double dmin = std::min(std::min(l, r), std::min(t, b));
        if (dmin >= band) {
            return orig;
        }
        const V2 n = dmin == l ? V2{-1.0, 0.0} : dmin == r ? V2{1.0, 0.0} : dmin == t ? V2{0.0, -1.0} : V2{0.0, 1.0};
        const double a = radians(q[1]);
        const double shade = dot(n, {std::sin(a), -std::cos(a)}) * q[5];
        Rgba out = orig;
        const std::array<double, 3> light{srgbLinear(q[2]) * orig[3], srgbLinear(q[3]) * orig[3], srgbLinear(q[4]) * orig[3]};
        for (std::size_t i = 0; i < 3; ++i) {
            out[i] = shade > 0.0 ? orig[i] + (light[i] - orig[i]) * shade : orig[i] * (1.0 + shade);
        }
        return out;
    }
    case E::DirectionalBlur: {
        const double a = radians(q[0]);
        const V2 dir{std::sin(a), -std::cos(a)};
        const double len = std::max(q[1], 0.0);
        if (len < 0.5) {
            return orig;
        }
        const int taps = static_cast<int>(std::clamp(std::ceil(len) + 1.0, 2.0, 97.0));
        Rgba acc{};
        for (int i = 0; i < taps; ++i) {
            acc = sum(acc, c.sampleCanvas(f + dir * ((static_cast<double>(i) / (taps - 1) - 0.5) * len)));
        }
        return scaled(acc, 1.0 / taps);
    }
    case E::CameraBlur: {
        const double radius = q[0] / 100.0 * 40.0;
        if (radius < 0.5) {
            return orig;
        }
        Rgba acc{};
        for (int i = 0; i < 64; ++i) {
            const double r = radius * std::sqrt((i + 0.5) / 64.0);
            const double a = i * 2.399963229728653;
            acc = sum(acc, c.sampleCanvas(f + V2{std::cos(a), std::sin(a)} * r));
        }
        return scaled(acc, 1.0 / 64.0);
    }
    default:
        return orig;
    }
}

} // namespace wabcut::render::reference::detail
