#include <wabcut/render/reference/Transitions.h>

#include <algorithm>
#include <cmath>

namespace wabcut::render::reference {

namespace {

constexpr double kPi = 3.14159265358979;

using P = TransitionPattern;

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
V2 absV(V2 a) { return {std::abs(a.x), std::abs(a.y)}; }
V2 maxV(V2 a, V2 b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
V2 floorV(V2 a) { return {std::floor(a.x), std::floor(a.y)}; }
V2 normalize(V2 a) { return a * (1.0 / length(a)); }

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double fract(double v) { return v - std::floor(v); }
double glslMod(double x, double y) { return x - y * std::floor(x / y); }
double smoothstep(double e0, double e1, double x) {
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

Rgba mix(const Rgba& a, const Rgba& b, double t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t};
}
Rgba scaled(const Rgba& a, double k) { return {a[0] * k, a[1] * k, a[2] * k, a[3] * k}; }
Rgba sum(const Rgba& a, const Rgba& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]}; }

int roundInt(double v) { return static_cast<int>(std::floor(v + 0.5)); }

/// One transition pass over a picture (fx_transition.frag's uniforms and helpers).
struct Pass {
    const Image* a = nullptr;
    const Image* b = nullptr;
    const TransitionSpec& s;
    V2 size;
    double aa = 1.0;

    Rgba sampleA(V2 px) const { return a ? sampleBilinear(*a, px.x, px.y) : Rgba{}; }
    Rgba sampleB(V2 px) const { return b ? sampleBilinear(*b, px.x, px.y) : Rgba{}; }

    V2 mirrorPos(V2 px) const {
        const V2 q{glslMod(px.x, 2.0 * size.x), glslMod(px.y, 2.0 * size.y)};
        return {q.x > size.x ? 2.0 * size.x - q.x : q.x, q.y > size.y ? 2.0 * size.y - q.y : q.y};
    }

    double coverage(double f) const { return clamp01(f / aa + 0.5); }
    double margin() const { return 0.5 * aa + s.borderWidth; }

    Rgba patternMix(const Rgba& a0, const Rgba& b0, double f) const {
        const double inB = coverage(f);
        const double bw = s.borderWidth;
        if (bw <= 0.0) {
            return mix(a0, b0, inB);
        }
        const double inBorder = coverage(f + bw) - inB;
        const Rgba border{s.borderColor[0], s.borderColor[1], s.borderColor[2], 1.0};
        return sum(sum(scaled(a0, 1.0 - inB - inBorder), scaled(b0, inB)), scaled(border, inBorder));
    }

    double hash01(int i, int j, int salt) const { return transitionHash01(i, j, salt, s.seed); }
};

double cwAngle(V2 u, V2 d) {
    const double y = u.x * d.y - u.y * d.x;
    const double x = dot(u, d);
    return x == 0.0 && y == 0.0 ? 0.0 : std::atan2(y, x);
}

V2 fromDirection(int dir) {
    switch (dir) {
    case 1: return {1.0, 0.0};
    case 2: return {0.0, 1.0};
    case 3: return {-1.0, 0.0};
    default: return {0.0, -1.0};
    }
}

V2 travel(int dir) {
    switch (dir) {
    case 1: return {0.0, 1.0};
    case 2: return {-1.0, 0.0};
    case 3: return {0.0, -1.0};
    case 4: return {1.0, 1.0};
    case 5: return {-1.0, 1.0};
    case 6: return {-1.0, -1.0};
    case 7: return {1.0, -1.0};
    default: return {1.0, 0.0};
    }
}

V2 cornerOf(int dir, V2 s) {
    switch (dir) {
    case 1: return {s.x, 0.0};
    case 2: return s;
    case 3: return {0.0, s.y};
    default: return {0.0, 0.0};
    }
}

// ---- Pattern fields ----

double irisField(const Pass& c, P kind, V2 pos, double t) {
    const V2 s = c.size;
    const V2 ctr{c.s.center[0] * s.x, c.s.center[1] * s.y};
    const V2 d = absV(pos - ctr);
    const V2 h = maxV(ctr, s - ctr);
    if (kind == P::IrisRound) {
        return t * length(h) - length(pos - ctr);
    }
    if (kind == P::IrisBox) {
        return std::min(t * h.x - d.x, t * h.y - d.y);
    }
    if (kind == P::IrisDiamond) {
        const V2 ab = h * 2.0;
        const double n = 1.0 / std::sqrt(1.0 / (ab.x * ab.x) + 1.0 / (ab.y * ab.y));
        return (t - d.x / ab.x - d.y / ab.y) * n;
    }
    const double reach = std::min(1.0, 2.0 * t);
    const double horizontal = std::min(reach * h.x - d.x, t * h.y - d.y);
    const double vertical = std::min(reach * h.y - d.y, t * h.x - d.x);
    return std::max(horizontal, vertical);
}

double sectorField(V2 pos, V2 ctr, V2 start, double sweep) {
    const V2 d = pos - ctr;
    const double r = length(d);
    const double a = cwAngle(start, d);
    const double f = sweep <= kPi ? std::min(a, sweep - a) : std::max(a, sweep - 2.0 * kPi - a);
    return r * f;
}

double bandIndexField(const Pass& c, V2 pos, double t, int bands, bool vertical) {
    const V2 s = c.size;
    const double along = vertical ? pos.y : pos.x;
    const double across = vertical ? pos.x : pos.y;
    const double len = vertical ? s.y : s.x;
    const double size = (vertical ? s.x : s.y) / bands;
    const int band = static_cast<int>(std::floor(across / size));
    return band % 2 == 0 ? t * len - along : along - (1.0 - t) * len;
}

double cellWipe(double q, double u, double cellLength) { return (q - u) * cellLength; }

/// (local coordinate along the travel, cell length along it).
V2 cellLocal(V2 pos, V2 cell, int dir) {
    const V2 local{fract(pos.x / cell.x), fract(pos.y / cell.y)};
    switch (dir) {
    case 1: return {local.y, cell.y};
    case 2: return {1.0 - local.x, cell.x};
    case 3: return {1.0 - local.y, cell.y};
    default: return {local.x, cell.x};
    }
}

V2 cellCounts(const TransitionSpec& s) {
    return {std::max(1.0, std::floor(s.custom[0] + 0.5)), std::max(1.0, std::floor(s.custom[1] + 0.5))};
}

double checkerField(const Pass& c, P kind, V2 pos, double t) {
    const V2 n = cellCounts(c.s);
    const V2 cell = c.size / n;
    const V2 ij = floorV(pos / cell);
    const bool odd = (static_cast<int>(ij.x) + static_cast<int>(ij.y)) % 2 == 1;
    const V2 lc = cellLocal(pos, cell, c.s.direction);
    double q = 0.0;
    if (kind == P::CheckerWipe) {
        q = odd ? 1.5 * t - 0.5 : 1.5 * t;
    } else {
        q = odd ? clamp01(2.0 * t - 1.0) : clamp01(2.0 * t);
    }
    return cellWipe(q, lc.x, lc.y);
}

/// (spiral index, travel direction) of cell (i, j).
std::array<int, 2> spiralOrder(int i, int j, int nx, int ny) {
    const int k = std::min(std::min(i, j), std::min(nx - 1 - i, ny - 1 - j));
    const int w = nx - 2 * k;
    const int h = ny - 2 * k;
    const int before = nx * ny - w * h;
    if (j == k) {
        return {before + (i - k), 0};
    }
    if (i == nx - 1 - k) {
        return {before + (w - 1) + (j - k), 1};
    }
    if (j == ny - 1 - k) {
        return {before + (w - 1) + (h - 1) + (nx - 1 - k - i), 2};
    }
    return {before + 2 * (w - 1) + (h - 1) + (ny - 1 - k - j), 3};
}

double blocksField(const Pass& c, P kind, V2 pos, double t) {
    const V2 s = c.size;
    V2 n = cellCounts(c.s);
    const int dir = c.s.direction;
    if (kind == P::RandomWipe) {
        const double rows = 18.0;
        n = {std::max(1.0, std::floor(rows * s.x / s.y + 0.5)), rows};
    }
    const V2 cell = s / n;
    const V2 fij = floorV(pos / cell);
    const int nx = static_cast<int>(n.x);
    const int ny = static_cast<int>(n.y);
    const int i = std::clamp(static_cast<int>(fij.x), 0, nx - 1);
    const int j = std::clamp(static_cast<int>(fij.y), 0, ny - 1);
    if (kind == P::RandomBlocks) {
        return t > c.hash01(i, j, 1) ? 1e4 : -1e4;
    }
    if (kind == P::RandomWipe) {
        const double spread = 4.0;
        const int at = dir == 1 ? j : dir == 2 ? nx - 1 - i : dir == 3 ? ny - 1 - j : i;
        const double count = (dir == 1 || dir == 3) ? n.y : n.x;
        const double reveal = (at + spread * c.hash01(i, j, 2)) / (count + spread);
        return t > reveal ? 1e4 : -1e4;
    }
    std::array<int, 2> order{};
    if (kind == P::SpiralBoxes) {
        order = spiralOrder(i, j, nx, ny);
    } else {
        const bool columns = dir == 1 || dir == 3;
        const int line = columns ? i : j;
        const int along = columns ? j : i;
        const int len = columns ? ny : nx;
        const bool forward = (line % 2 == 0) == (dir == 0 || dir == 1);
        const int at = forward ? along : len - 1 - along;
        const int travelDir = columns ? (forward ? 1 : 3) : (forward ? 0 : 2);
        order = {line * len + at, travelDir};
    }
    const V2 lc = cellLocal(pos, cell, order[1]);
    const double q = t * (nx * ny) - order[0];
    return cellWipe(q, lc.x, lc.y);
}

double splatterField(const Pass& c, V2 pos, double t) {
    const V2 s = c.size;
    const double diag = length(s);
    double f = -1e4;
    for (int i = 0; i < 28; ++i) {
        const V2 ctr = V2{c.hash01(i, 0, 3), c.hash01(i, 1, 3)} * s;
        const double rho = (0.12 + 0.2 * c.hash01(i, 2, 3)) * diag;
        const double start = 0.6 * c.hash01(i, 3, 3);
        const double grow = clamp01((t - start) / (1.0 - start));
        const V2 d = pos - ctr;
        const double wobble = 1.0 + 0.18 * std::sin(7.0 * cwAngle({1.0, 0.0}, d) + 6.2831853 * c.hash01(i, 4, 3));
        f = std::max(f, grow * rho * wobble - length(d));
    }
    const double flood = clamp01((t - 0.8) / 0.2);
    return std::max(f, (2.0 * flood - 1.0) * diag);
}

double patternField(const Pass& c, P kind, V2 pos, double t) {
    const V2 s = c.size;
    const int dir = c.s.direction;
    switch (kind) {
    case P::IrisBox:
    case P::IrisCross:
    case P::IrisDiamond:
    case P::IrisRound:
        return irisField(c, kind, pos, t);
    case P::ClockWipe:
        return sectorField(pos, s * 0.5, fromDirection(dir), t * 2.0 * kPi);
    case P::Pinwheel: {
        const int wedges = std::max(1, roundInt(c.s.custom[0]));
        const double w = 2.0 * kPi / wedges;
        const V2 ctr{c.s.center[0] * s.x, c.s.center[1] * s.y};
        const V2 d = pos - ctr;
        const double a = cwAngle({0.0, -1.0}, d);
        const double local = glslMod(a, w);
        const double sweep = t * w;
        const double f = local < sweep ? std::min(local, sweep - local) : -std::min(local - sweep, w - local);
        return length(d) * f;
    }
    case P::WedgeWipe: {
        const V2 d = pos - s * 0.5;
        return length(d) * (t * kPi - std::abs(cwAngle(fromDirection(dir), d)));
    }
    case P::RadialWipe: {
        const V2 corner = cornerOf(dir, s);
        const V2 start = dir == 1 ? V2{0.0, 1.0} : dir == 2 ? V2{-1.0, 0.0} : dir == 3 ? V2{0.0, -1.0} : V2{1.0, 0.0};
        const V2 d = pos - corner;
        return length(d) * (t * 0.5 * kPi - cwAngle(start, d));
    }
    case P::Inset: {
        const V2 d = absV(pos - cornerOf(dir, s));
        return std::min(t * s.x - d.x, t * s.y - d.y);
    }
    case P::BarnDoors:
        return dir == 1 ? t * 0.5 * s.y - std::abs(pos.y - 0.5 * s.y) : t * 0.5 * s.x - std::abs(pos.x - 0.5 * s.x);
    case P::BandWipe:
        return bandIndexField(c, pos, t, std::max(1, roundInt(c.s.custom[0])), dir == 1);
    case P::VenetianBlinds: {
        const int bands = std::max(1, roundInt(c.s.custom[0]));
        const bool vertical = dir == 0 || dir == 2;
        const double size = (vertical ? s.x : s.y) / bands;
        const double along = vertical ? pos.x : pos.y;
        double local = along - std::floor(along / size) * size;
        if (dir == 2 || dir == 3) {
            local = size - local;
        }
        return t * size - local;
    }
    case P::CheckerWipe:
    case P::CheckerBoard:
        return checkerField(c, kind, pos, t);
    case P::RandomBlocks:
    case P::RandomWipe:
    case P::SpiralBoxes:
    case P::ZigZagBlocks:
        return blocksField(c, kind, pos, t);
    case P::PaintSplatter:
        return splatterField(c, pos, t);
    default:
        return 0.0;
    }
}

// ---- Moving pictures ----

Rgba pushAt(const Pass& c, V2 pos, V2 v, double offset) {
    const double len = std::abs(v.x) > 0.5 ? c.size.x : c.size.y;
    const double along = dot(pos, v) + (v.x + v.y < 0.0 ? len : 0.0);
    return offset > along ? c.sampleB(pos - v * offset + v * len) : c.sampleA(pos - v * offset);
}

Rgba slideMix(const Pass& c, V2 pos, double t) {
    const V2 s = c.size;
    const V2 v = travel(c.s.direction);
    const bool reverse = c.s.reverse;
    const V2 o = reverse ? v * s * t : v * s * (t - 1.0);
    const V2 q = pos - o;
    const double inside = std::min(std::min(q.x, s.x - q.x), std::min(q.y, s.y - q.y));
    if (reverse) {
        return c.patternMix(c.sampleA(q), c.sampleB(pos), -inside + c.margin() * (2.0 * t - 1.0));
    }
    return c.patternMix(c.sampleA(pos), c.sampleB(q), inside + c.margin() * (2.0 * t - 1.0));
}

Rgba bandSlideMix(const Pass& c, V2 pos, double t) {
    const V2 s = c.size;
    const bool vertical = c.s.direction == 1;
    const int bands = std::max(1, roundInt(c.s.custom[0]));
    const double size = (vertical ? s.x : s.y) / bands;
    const int band = static_cast<int>(std::floor((vertical ? pos.x : pos.y) / size));
    const double len = vertical ? s.y : s.x;
    const double along = vertical ? pos.y : pos.x;
    const bool fromStart = band % 2 == 0;
    const double shift = fromStart ? (1.0 - t) * len : -(1.0 - t) * len;
    const double f = fromStart ? t * len - along : along - (1.0 - t) * len;
    const V2 q = vertical ? V2{pos.x, pos.y + shift} : V2{pos.x + shift, pos.y};
    return c.patternMix(c.sampleA(pos), c.sampleB(q), f + c.margin() * (2.0 * t - 1.0));
}

Rgba splitMix(const Pass& c, P kind, V2 pos, double t) {
    const V2 s = c.size;
    const V2 h = s * 0.5;
    const V2 side{pos.x < h.x ? -1.0 : 1.0, pos.y < h.y ? -1.0 : 1.0};
    V2 shift;
    double insideA = 0.0;
    if (kind == P::CenterSplit) {
        shift = side * h * t;
        const V2 q = pos - shift;
        const V2 in2{side.x < 0.0 ? h.x - q.x : q.x - h.x, side.y < 0.0 ? h.y - q.y : q.y - h.y};
        insideA = std::min(in2.x, in2.y);
    } else if (c.s.direction == 1) {
        shift = {0.0, side.y * t * h.y};
        const double qy = pos.y - shift.y;
        insideA = side.y < 0.0 ? h.y - qy : qy - h.y;
    } else {
        shift = {side.x * t * h.x, 0.0};
        const double qx = pos.x - shift.x;
        insideA = side.x < 0.0 ? h.x - qx : qx - h.x;
    }
    return c.patternMix(c.sampleA(pos - shift), c.sampleB(pos), -insideA + c.margin() * (2.0 * t - 1.0));
}

Rgba whipMix(const Pass& c, V2 pos, double t) {
    const V2 v = travel(c.s.direction);
    const double len = std::abs(v.x) > 0.5 ? c.size.x : c.size.y;
    const double e = t * t * (3.0 - 2.0 * t);
    const double speed = 6.0 * t * (1.0 - t);
    const double blur = c.s.custom[0] / 100.0 * speed * len * 0.25;
    constexpr int kTaps = 24;
    Rgba acc{};
    for (int i = 0; i < kTaps; ++i) {
        const double k = (i + 0.5) / kTaps - 0.5;
        acc = sum(acc, pushAt(c, pos + v * (k * blur), v, e * len));
    }
    return scaled(acc, 1.0 / kTaps);
}

Rgba zoomSample(const Pass& c, bool fromA, V2 pos, V2 ctr, double scale, double blur, bool mirror) {
    constexpr int kTaps = 16;
    Rgba acc{};
    for (int i = 0; i < kTaps; ++i) {
        const double k = 1.0 + blur * i / (kTaps - 1);
        V2 q = ctr + (pos - ctr) * (1.0 / (scale * k));
        if (mirror) {
            q = c.mirrorPos(q);
        }
        acc = sum(acc, fromA ? c.sampleA(q) : c.sampleB(q));
    }
    return scaled(acc, 1.0 / kTaps);
}

Rgba zoomMix(const Pass& c, P kind, V2 pos, double t) {
    const V2 ctr{c.s.center[0] * c.size.x, c.s.center[1] * c.size.y};
    const bool out = kind == P::ZoomBlur && c.s.direction == 1;
    const double strength = kind == P::CrossZoom ? 1.0 : c.s.custom[0] / 50.0;
    const double w = smoothstep(0.45, 0.55, t);
    Rgba a{};
    Rgba b{};
    if (w < 1.0) {
        const double z = 2.0 * t;
        const double scale = out ? 1.0 / (1.0 + 3.0 * z * z) : 1.0 + 3.0 * z * z;
        a = zoomSample(c, true, pos, ctr, scale, 0.15 * strength * z, out);
    }
    if (w > 0.0) {
        const double z = 2.0 * (1.0 - t);
        const double scale = out ? 1.0 / (1.0 + 3.0 * z * z) : 1.0 + 3.0 * z * z;
        b = zoomSample(c, false, pos, ctr, scale, 0.15 * strength * z, out);
    }
    return mix(a, b, w);
}

Rgba pageMix(const Pass& c, P kind, V2 pos, double t) {
    const V2 s = c.size;
    const bool reverse = c.s.reverse;
    const double tt = reverse ? 1.0 - t : t;
    const V2 k = cornerOf(c.s.direction, s);
    const V2 n = normalize(k * 2.0 - s);
    const double extent = length(s);
    const double margin = 0.5 * c.aa;
    const double cut = tt * (extent + 2.0 * margin) - margin;
    const double tau = dot(k - pos, n);
    const V2 mirrored = pos + n * (2.0 * (tau - cut));
    const double tauS = 2.0 * cut - tau;
    const Rgba page = reverse ? c.sampleB(pos) : c.sampleA(pos);
    Rgba under = reverse ? c.sampleA(pos) : c.sampleB(pos);
    const Rgba back = reverse ? c.sampleB(mirrored) : c.sampleA(mirrored);
    const double edge = std::min(std::min(mirrored.x, s.x - mirrored.x), std::min(mirrored.y, s.y - mirrored.y));
    const double flap = c.coverage(tau - cut) * c.coverage(edge) * c.coverage(tauS);
    Rgba flapColor{};
    if (kind == P::PageTurn) {
        flapColor = {back[0] * 0.8, back[1] * 0.8, back[2] * 0.8, back[3]};
    } else {
        const double curl = 0.55 + 0.45 * clamp01((tau - cut) / (0.06 * extent));
        const double g = 0.72 * curl * back[3];
        flapColor = {g, g, g, back[3]};
        const double shadow = 1.0 - 0.45 * (1.0 - tt) * std::exp(-std::max(cut - tau, 0.0) / (0.03 * extent));
        under = {under[0] * shadow, under[1] * shadow, under[2] * shadow, under[3]};
    }
    const Rgba top = mix(page, flapColor, flap);
    return mix(top, under, c.coverage(cut - tau));
}

Rgba glitchMix(const Pass& c, V2 pos, double t) {
    const V2 s = c.size;
    const double sine = std::max(0.0, std::sin(kPi * t));
    const double g = c.s.custom[0] / 100.0 * sine * sine;
    const int step = static_cast<int>(std::floor(t * 24.0));
    const int band = static_cast<int>(std::floor(pos.y / (s.y / 32.0)));
    double dx = 0.0;
    if (c.hash01(band, step, 5) < 0.35 * g * 2.0) {
        dx = (c.hash01(band, step, 6) - 0.5) * 0.2 * s.x * g;
    }
    const V2 cellF = floorV(pos / (s / V2{16.0, 9.0}));
    const bool flicker = c.hash01(static_cast<int>(cellF.x) + 16 * static_cast<int>(cellF.y), step, 7) < 0.15 * g;
    const bool useB = (t >= 0.5) != flicker;
    const double split = c.s.custom[1] * g;
    const V2 q{pos.x + dx, pos.y};
    const auto at = [&](V2 p0) { return useB ? c.sampleB(p0) : c.sampleA(p0); };
    const Rgba r = at(q + V2{split, 0.0});
    const Rgba m = at(q);
    const Rgba b = at(q - V2{split, 0.0});
    return {r[0], m[1], b[2], m[3]};
}

Rgba lightLeakMix(const Pass& c, V2 pos, double t, const Rgba& a, const Rgba& b) {
    const V2 s = c.size;
    const Rgba base = mix(a, b, smoothstep(0.35, 0.65, t));
    const double sine = std::max(0.0, std::sin(kPi * t));
    const double intensity = c.s.custom[0] * sine * std::sqrt(sine);
    std::array<double, 3> leak{};
    for (int k = 0; k < 3; ++k) {
        const V2 ctr = V2{0.15 + 0.7 * fract(k * 0.37 + 0.8 * t), 0.2 + 0.6 * fract(k * 0.61 + 0.3)} * s;
        const double sigma = (0.3 + 0.1 * k) * s.y;
        const V2 d = pos - ctr;
        const std::array<double, 3> tint = k == 0   ? std::array<double, 3>{1.0, 1.0, 1.0}
                                           : k == 1 ? std::array<double, 3>{1.0, 0.8, 0.6}
                                                    : std::array<double, 3>{1.0, 0.9, 0.8};
        const double glow = std::exp(-dot(d, d) / (2.0 * sigma * sigma));
        for (std::size_t ch = 0; ch < 3; ++ch) {
            leak[ch] += c.s.color[ch] * tint[ch] * glow;
        }
    }
    return {base[0] + intensity * leak[0] * base[3], base[1] + intensity * leak[1] * base[3],
            base[2] + intensity * leak[2] * base[3], base[3]};
}

Rgba flashMix(const Pass& c, double t, const Rgba& a, const Rgba& b) {
    const Rgba base = t < 0.5 ? a : b;
    const double bump = 1.0 - std::abs(2.0 * t - 1.0);
    const double f = clamp01(c.s.custom[0] * bump * bump);
    const double gain = 1.0 + 3.0 * f;
    const Rgba bright{base[0] * gain, base[1] * gain, base[2] * gain, base[3]};
    return mix(bright, {c.s.color[0], c.s.color[1], c.s.color[2], 1.0}, f * f);
}

Rgba gradientMix(const Pass& c, V2 pos, double t, const Rgba& a, const Rgba& b) {
    const V2 s = c.size;
    const int type = roundInt(c.s.custom[0]);
    double g = 0.0;
    if (type == 1) {
        g = pos.y / s.y;
    } else if (type == 2) {
        g = length(pos - s * 0.5) / length(s * 0.5);
    } else if (type == 3) {
        g = 0.5 * (pos.x / s.x + pos.y / s.y);
    } else {
        g = pos.x / s.x;
    }
    const bool reverse = c.s.reverse;
    const double tt = reverse ? 1.0 - t : t;
    const double softness = c.s.custom[1] / 100.0;
    double m = 0.0;
    if (softness > 0.0) {
        m = clamp01((tt * (1.0 + softness) - g) / softness);
    } else {
        const double len = type == 1 ? s.y : (type == 2 ? length(s * 0.5) : s.x);
        m = clamp01(((tt - g) * len + 0.5 * c.aa * (2.0 * tt - 1.0)) / c.aa + 0.5);
    }
    if (reverse) {
        m = 1.0 - m;
    }
    return mix(a, b, m);
}

Rgba wipeMix(const Pass& c, V2 pos, double t, const Rgba& a, const Rgba& b) {
    const bool reverse = c.s.reverse;
    const double tt = reverse ? 1.0 - t : t;
    const double angle = c.s.angleDegrees * 3.14159265358979323846 / 180.0;
    const V2 dir{std::cos(angle), std::sin(angle)};
    const V2 size = c.size;
    const double s0 = std::min(0.0, dir.x * size.x) + std::min(0.0, dir.y * size.y);
    const double s1 = std::max(0.0, dir.x * size.x) + std::max(0.0, dir.y * size.y);
    const double len = s1 - s0;
    const double w = clamp01(c.s.feather) * len;
    const double e = s0 - 0.5 * w + tt * (len + w);
    double f = e - dot(pos, dir);
    if (reverse) {
        f = -f;
    }
    if (w > 0.0) {
        f += c.s.borderWidth * (2.0 * t - 1.0);
        const double m = clamp01(f / w + 0.5);
        const double bw = c.s.borderWidth;
        const double inBorder = bw > 0.0 ? clamp01((f + bw) / w + 0.5) - m : 0.0;
        const Rgba border{c.s.borderColor[0], c.s.borderColor[1], c.s.borderColor[2], 1.0};
        return sum(sum(scaled(a, 1.0 - m - inBorder), scaled(b, m)), scaled(border, inBorder));
    }
    return c.patternMix(a, b, f + c.margin() * (2.0 * t - 1.0));
}

} // namespace

double antiAliasWidth(int quality, int divisor) {
    constexpr std::array<double, 4> kWidths{1.0, 2.0, 3.0, 5.0};
    return std::max(static_cast<double>(std::max(divisor, 1)), kWidths[static_cast<std::size_t>(std::clamp(quality, 0, 3))]);
}

std::uint32_t transitionHash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

double transitionHash01(int i, int j, int salt, std::uint32_t seed) {
    const std::uint32_t inner = transitionHash((seed & 0xFFFFFFu) + static_cast<std::uint32_t>(salt) * 0x9e3779b9u);
    const std::uint32_t h =
        transitionHash(static_cast<std::uint32_t>(i) + transitionHash(static_cast<std::uint32_t>(j) + inner));
    return (static_cast<double>(h & 1023u) + 0.5) / 1024.0;
}

Image transition(const Image* a, const Image* b, const TransitionSpec& spec) {
    const Image& shape = a ? *a : *b;
    Image out(shape.width, shape.height);
    const Pass c{a, b, spec, {static_cast<double>(shape.width), static_cast<double>(shape.height)},
                 antiAliasWidth(spec.antiAliasing)};
    const double t = clamp01(spec.progress);
    const P kind = spec.pattern;
    const double wa = std::min(1.0, 2.0 * (1.0 - t));
    const double wb = std::min(1.0, 2.0 * t);
    for (int y = 0; y < shape.height; ++y) {
        for (int x = 0; x < shape.width; ++x) {
            const Rgba ca = a ? a->atD(x, y) : Rgba{};
            const Rgba cb = b ? b->atD(x, y) : Rgba{};
            const V2 pos{x + 0.5, y + 0.5};
            Rgba r{};
            switch (kind) {
            case P::CrossDissolve:
                r = mix(ca, cb, t);
                break;
            case P::DipToColor: {
                const Rgba color{spec.color[0], spec.color[1], spec.color[2], 1.0};
                r = t < 0.5 ? mix(ca, color, 2.0 * t) : mix(color, cb, 2.0 * t - 1.0);
                break;
            }
            case P::AdditiveDissolve:
                r = {ca[0] * wa + cb[0] * wb, ca[1] * wa + cb[1] * wb, ca[2] * wa + cb[2] * wb,
                     std::min(1.0, ca[3] * wa + cb[3] * wb)};
                break;
            case P::NonAdditiveDissolve:
                for (std::size_t ch = 0; ch < 4; ++ch) {
                    r[ch] = std::max(ca[ch] * wa, cb[ch] * wb);
                }
                break;
            case P::Wipe:
                r = wipeMix(c, pos, t, ca, cb);
                break;
            case P::Push: {
                int dir = spec.direction;
                if (spec.reverse) {
                    dir = (dir + 2) % 4;
                }
                const V2 v = travel(dir);
                const double len = std::abs(v.x) > 0.5 ? c.size.x : c.size.y;
                const double shift = t * len;
                const double f = shift - (dot(pos, v) + (v.x + v.y < 0.0 ? len : 0.0)) + 0.5 * c.aa * (2.0 * t - 1.0);
                r = mix(c.sampleA(pos - v * shift), c.sampleB(pos - v * shift + v * len), c.coverage(f));
                break;
            }
            case P::Slide:
                r = slideMix(c, pos, t);
                break;
            case P::BandSlide:
                r = bandSlideMix(c, pos, t);
                break;
            case P::CenterSplit:
            case P::Split:
                r = splitMix(c, kind, pos, t);
                break;
            case P::Whip:
                r = whipMix(c, pos, t);
                break;
            case P::CrossZoom:
            case P::ZoomBlur:
                r = zoomMix(c, kind, pos, t);
                break;
            case P::PagePeel:
            case P::PageTurn:
                r = pageMix(c, kind, pos, t);
                break;
            case P::Glitch:
                r = glitchMix(c, pos, t);
                break;
            case P::LightLeak:
                r = lightLeakMix(c, pos, t, ca, cb);
                break;
            case P::Flash:
                r = flashMix(c, t, ca, cb);
                break;
            case P::GradientWipe:
                r = gradientMix(c, pos, t, ca, cb);
                break;
            default: {
                double f = patternField(c, kind, pos, spec.reverse ? 1.0 - t : t);
                if (spec.reverse) {
                    f = -f;
                }
                r = c.patternMix(ca, cb, f + c.margin() * (2.0 * t - 1.0));
                break;
            }
            }
            out.setD(x, y, r);
        }
    }
    return out;
}

} // namespace wabcut::render::reference
