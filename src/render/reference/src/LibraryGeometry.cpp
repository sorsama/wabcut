// Strokes of Lightning and Scribble (render/reference/LibraryGeometry.h): shared by the compositor and the CPU twins.

#include <wabcut/render/reference/LibraryGeometry.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace wabcut::render::reference {

namespace {

constexpr double kPi = 3.14159265358979;
constexpr std::uint32_t kStaticSlot = 0xffffffffu;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

int roundedInt(double v, int lo, int hi) {
    if (!std::isfinite(v)) {
        return lo;
    }
    return static_cast<int>(std::clamp(std::lround(v), static_cast<long>(lo), static_cast<long>(hi)));
}

std::uint32_t seedOf(double v) {
    return static_cast<std::uint32_t>(std::isfinite(v) ? std::clamp(std::lround(v), 0L, 1000000L) : 0L);
}

/// A slot number (time x rate) as a hash word: two's complement, so negative times work too.
std::uint32_t slotWord(double slot) {
    if (!std::isfinite(slot)) {
        return 0u;
    }
    const double clamped = std::clamp(slot, -1.0e15, 1.0e15);
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(clamped))));
}

/// A value in [-1, 1) for (seed, stream, index, slot).
double signedHash(std::uint32_t seed, std::uint32_t stream, std::uint32_t index, std::uint32_t slot) {
    return 2.0 * libraryHash01(index + 7919u * stream, seed * 1013u + slot * 2654435761u, 17u) - 1.0;
}

double unitHash(std::uint32_t seed, std::uint32_t stream, std::uint32_t index, std::uint32_t slot) {
    return libraryHash01(index + 7919u * stream, seed * 1013u + slot * 2654435761u, 17u);
}

struct Pt {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
};

void emitPolyline(const std::vector<Pt>& line, std::vector<StrokeSegment>& out) {
    for (std::size_t i = 1; i < line.size() && out.size() < kMaxStrokeSegments; ++i) {
        out.push_back({line[i - 1].x, line[i - 1].y, line[i].x, line[i].y, line[i - 1].w, line[i].w});
    }
}

} // namespace

double strokeReach(StrokeKind kind, double width, double coreWidth) {
    const double w = std::max(width, 0.0);
    if (kind == StrokeKind::Line) {
        return 0.5 * w + 1.0;
    }
    return std::max(1.5 * w, 0.5 * w * clamp01(coreWidth) + 1.0) + 1.0;
}

// ---- Lightning -------------------------------------------------------------------------------------------------------

std::vector<StrokeSegment> lightningStrokes(const LibraryParams& q, double width, double height) {
    std::vector<StrokeSegment> out;
    const double sx = q[0] * width;
    const double sy = q[1] * height;
    const double ex = q[2] * width;
    const double ey = q[3] * height;
    const double length = std::hypot(ex - sx, ey - sy);
    if (!(length > 1e-6) || !std::isfinite(length)) {
        return out;
    }
    const int n = roundedInt(q[4], 1, 32);
    const double amplitude = std::max(q[5], 0.0) / 100.0;
    const int detail = roundedInt(q[6], 0, 3);
    const double detailAmplitude = clamp01(q[7]);
    const double branching = clamp01(q[8]);
    const double rebranching = clamp01(q[9]);
    const double branchAngle = q[10] * kPi / 180.0;
    const double branchLength = std::max(q[11], 0.0);
    const int branchSegments = roundedInt(q[12], 1, 16);
    const double branchWidth = clamp01(q[13]);
    const double speed = std::max(q[14], 0.0);
    const double stability = clamp01(q[15]);
    const bool fixedEndpoint = q[16] > 0.5;
    const double boltWidth = std::max(q[17], 0.0);
    const double widthVariation = clamp01(q[18]);
    const double decay = clamp01(q[26]);
    const std::uint32_t seed = seedOf(q[27]);
    // Speed: how many new shapes a second; between two the bolt morphs (smoothstep). Stability keeps that much of a
    // shape that never changes.
    const double slot = speed * q[kLibraryTimeParamIndex];
    const double s0 = std::floor(slot);
    const double f = slot - s0;
    const double fs = f * f * (3.0 - 2.0 * f);
    const std::uint32_t slotIndex = slotWord(s0);
    const auto rnd = [&](std::uint32_t stream, std::uint32_t index) {
        const double base = signedHash(seed, stream, index, kStaticSlot);
        const double a = signedHash(seed, stream, index, slotIndex);
        const double b = signedHash(seed, stream, index, slotIndex + 1u);
        return stability * base + (1.0 - stability) * (a + (b - a) * fs);
    };
    const double ux = (ex - sx) / length;
    const double uy = (ey - sy) / length;
    const double px = -uy;
    const double py = ux;

    // The main bolt: a random walk across the line from start to end (pinned to the end with Fixed Endpoint).
    const double step = amplitude * length * 0.6 / std::sqrt(static_cast<double>(n));
    std::vector<double> offset(static_cast<std::size_t>(n) + 1, 0.0);
    for (int i = 1; i <= n; ++i) {
        offset[static_cast<std::size_t>(i)] = offset[static_cast<std::size_t>(i - 1)] + rnd(1u, static_cast<std::uint32_t>(i)) * step;
    }
    if (fixedEndpoint) {
        const double last = offset[static_cast<std::size_t>(n)];
        for (int i = 1; i <= n; ++i) {
            offset[static_cast<std::size_t>(i)] -= last * i / n;
        }
    }
    std::vector<Pt> main;
    main.reserve(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) {
        const double along = length * i / n;
        const double o = offset[static_cast<std::size_t>(i)];
        const double w = boltWidth * (1.0 - decay * i / n) * (1.0 + widthVariation * rnd(2u, static_cast<std::uint32_t>(i)));
        main.push_back({sx + ux * along + px * o, sy + uy * along + py * o, std::max(w, 0.0)});
    }
    // Detail: each level splits every segment, its middle pushed across by Detail Amplitude of its length.
    std::vector<Pt> bolt = main;
    for (int level = 1; level <= detail; ++level) {
        std::vector<Pt> finer;
        finer.reserve(bolt.size() * 2);
        for (std::size_t i = 0; i + 1 < bolt.size(); ++i) {
            const Pt& a = bolt[i];
            const Pt& b = bolt[i + 1];
            const double dx = b.x - a.x;
            const double dy = b.y - a.y;
            const double r = rnd(3u + static_cast<std::uint32_t>(level), static_cast<std::uint32_t>(i)) * detailAmplitude * 0.5;
            finer.push_back(a);
            finer.push_back({(a.x + b.x) * 0.5 - dy * r, (a.y + b.y) * 0.5 + dx * r, (a.w + b.w) * 0.5});
        }
        finer.push_back(bolt.back());
        bolt = std::move(finer);
    }
    emitPolyline(bolt, out);

    // Branches from the main bolt's corners (Branching: the chance at each), with Rebranching from their corners.
    const double segmentLength = branchLength * length / n;
    const auto branch = [&](const auto& self, Pt origin, double heading, int segments, std::uint32_t stream, int depth) -> void {
        std::vector<Pt> line{origin};
        const double w0 = origin.w;
        double theta = heading;
        for (int j = 1; j <= segments; ++j) {
            theta += rnd(stream, static_cast<std::uint32_t>(j)) * 0.35;
            const Pt& prev = line.back();
            const double w = w0 * (1.0 - static_cast<double>(j) / (segments + 1) * (0.5 + 0.5 * decay));
            line.push_back({prev.x + std::cos(theta) * segmentLength, prev.y + std::sin(theta) * segmentLength, std::max(w, 0.0)});
        }
        emitPolyline(line, out);
        if (depth > 0) {
            return;
        }
        for (int j = 1; j < segments; ++j) {
            const double chance = stability >= 0.5 ? unitHash(seed, stream + 7u, static_cast<std::uint32_t>(j), kStaticSlot)
                                                   : unitHash(seed, stream + 7u, static_cast<std::uint32_t>(j), slotIndex);
            if (chance >= rebranching) {
                continue;
            }
            const double side = signedHash(seed, stream + 8u, static_cast<std::uint32_t>(j), kStaticSlot) < 0.0 ? -1.0 : 1.0;
            Pt start = line[static_cast<std::size_t>(j)];
            start.w *= branchWidth;
            self(self, start, theta + side * branchAngle, std::max(1, segments / 2), stream + 16u * static_cast<std::uint32_t>(j), 1);
        }
    };
    const double mainHeading = std::atan2(uy, ux);
    for (int i = 1; i < n; ++i) {
        const double chance = stability >= 0.5 ? unitHash(seed, 20u, static_cast<std::uint32_t>(i), kStaticSlot)
                                               : unitHash(seed, 20u, static_cast<std::uint32_t>(i), slotIndex);
        if (chance >= branching) {
            continue;
        }
        const double side = signedHash(seed, 21u, static_cast<std::uint32_t>(i), kStaticSlot) < 0.0 ? -1.0 : 1.0;
        Pt start = main[static_cast<std::size_t>(i)];
        start.w *= branchWidth;
        branch(branch, start, mainHeading + side * branchAngle + rnd(22u, static_cast<std::uint32_t>(i)) * 0.2, branchSegments,
               1000u + 1024u * static_cast<std::uint32_t>(i), 0);
    }
    return out;
}

// ---- Scribble --------------------------------------------------------------------------------------------------------

std::vector<StrokeSegment> scribbleStrokes(const LibraryParams& q, std::span<const StrokePolygon> paths, double width,
                                           double height) {
    std::vector<StrokeSegment> out;
    std::vector<StrokePolygon> polygons;
    for (const StrokePolygon& p : paths) {
        if (p.size() >= 3) {
            polygons.push_back(p);
            if (q[0] < 0.5) {
                break; // Single Mask: the first
            }
        }
    }
    if (polygons.empty()) {
        polygons.push_back({{0.0, 0.0}, {width, 0.0}, {width, height}, {0.0, height}});
    }
    const int fillType = roundedInt(q[1], 0, 3);
    const double edgeWidth = std::max(q[2], 0.1);
    const double angle = q[7] * kPi / 180.0;
    const double strokeWidth = std::max(q[8], 0.1);
    const double curvature = clamp01(q[9]);
    const double spacing = std::max(q[10], 1.0);
    const double spacingVariation = std::clamp(q[11], 0.0, spacing * 0.45);
    const double overlap = std::clamp(q[12], -1.0, 1.0);
    const double jitter = std::max(q[13], 0.0);
    const double start = clamp01(q[14]);
    const double end = clamp01(q[15]);
    const double wiggles = std::max(q[16], 0.0);
    const std::uint32_t seed = seedOf(q[17]);
    const std::uint32_t slot = wiggles > 0.0 ? slotWord(q[kLibraryTimeParamIndex] * wiggles) : 0u;
    const auto rnd = [&](std::uint32_t stream, std::uint32_t index) { return signedHash(seed, stream, index, slot); };
    if (end <= start) {
        return out;
    }

    std::vector<std::vector<Pt>> lines;
    if (fillType == 0) {
        // Inside: hatching along Angle, Spacing apart (each line moved by up to Spacing Variation), one zigzag where the
        // sweeps follow each other, turning on curves (Curvature); a new stroke where they jump.
        const double ux = std::cos(angle);
        const double uy = std::sin(angle);
        const double nx = -uy;
        const double ny = ux;
        double lo = std::numeric_limits<double>::max();
        double hi = std::numeric_limits<double>::lowest();
        for (const StrokePolygon& p : polygons) {
            for (const auto& v : p) {
                const double c = nx * v[0] + ny * v[1];
                lo = std::min(lo, c);
                hi = std::max(hi, c);
            }
        }
        const double extend = overlap * spacing;
        std::uint32_t sweep = 0;
        std::vector<Pt>* current = nullptr;
        Pt sweepStart;
        int k = 0;
        for (double c = lo + spacing * 0.5; c < hi && k < 8192; c += spacing, ++k) {
            const double ck = c + rnd(1u, static_cast<std::uint32_t>(k)) * spacingVariation;
            std::vector<double> ts;
            for (const StrokePolygon& p : polygons) {
                for (std::size_t i = 0; i < p.size(); ++i) {
                    const auto& a = p[i];
                    const auto& b = p[(i + 1) % p.size()];
                    const double da = nx * a[0] + ny * a[1] - ck;
                    const double db = nx * b[0] + ny * b[1] - ck;
                    if ((da < 0.0) != (db < 0.0)) {
                        const double s = da / (da - db);
                        ts.push_back(ux * (a[0] + (b[0] - a[0]) * s) + uy * (a[1] + (b[1] - a[1]) * s));
                    }
                }
            }
            std::sort(ts.begin(), ts.end());
            std::vector<std::array<double, 2>> spans;
            for (std::size_t i = 0; i + 1 < ts.size(); i += 2) {
                const double t0 = ts[i] - extend;
                const double t1 = ts[i + 1] + extend;
                if (t1 > t0) {
                    spans.push_back({t0, t1});
                }
            }
            if (k % 2 == 1) {
                std::reverse(spans.begin(), spans.end());
                for (auto& s : spans) {
                    std::swap(s[0], s[1]);
                }
            }
            for (const auto& s : spans) {
                const Pt a{nx * ck + ux * s[0] + rnd(2u, sweep) * jitter, ny * ck + uy * s[0] + rnd(3u, sweep) * jitter, strokeWidth};
                const Pt b{nx * ck + ux * s[1] + rnd(2u, sweep + 1u) * jitter, ny * ck + uy * s[1] + rnd(3u, sweep + 1u) * jitter,
                           strokeWidth};
                sweep += 2u;
                const double reachTurn = 3.0 * spacing + 2.0 * jitter + 2.0 * std::abs(extend);
                if (current && std::hypot(a.x - current->back().x, a.y - current->back().y) < reachTurn) {
                    // A turn: a quadratic curve bulging the way the last sweep went.
                    const Pt last = current->back();
                    const double hx = last.x - sweepStart.x;
                    const double hy = last.y - sweepStart.y;
                    const double hl = std::max(std::hypot(hx, hy), 1e-9);
                    const double bulge = curvature * spacing * 3.0;
                    const double cx = (last.x + a.x) * 0.5 + hx / hl * bulge;
                    const double cy = (last.y + a.y) * 0.5 + hy / hl * bulge;
                    if (bulge > 0.0) {
                        for (int j = 1; j < 4; ++j) {
                            const double t = j / 4.0;
                            const double u0 = (1.0 - t) * (1.0 - t);
                            const double u1 = 2.0 * (1.0 - t) * t;
                            const double u2 = t * t;
                            current->push_back({u0 * last.x + u1 * cx + u2 * a.x, u0 * last.y + u1 * cy + u2 * a.y, strokeWidth});
                        }
                    }
                    current->push_back(a);
                } else {
                    lines.push_back({a});
                    current = &lines.back();
                }
                current->push_back(b);
                sweepStart = a;
            }
        }
    } else {
        // Along the edges: a zigzag across a band of Edge Width, Spacing apart along the outline (Centered, inside or
        // outside it).
        std::uint32_t point = 0;
        for (const StrokePolygon& p : polygons) {
            double area = 0.0;
            for (std::size_t i = 0; i < p.size(); ++i) {
                const auto& a = p[i];
                const auto& b = p[(i + 1) % p.size()];
                area += a[0] * b[1] - b[0] * a[1];
            }
            const double outward = area >= 0.0 ? 1.0 : -1.0;
            const double inner = fillType == 1 ? -0.5 * edgeWidth : (fillType == 2 ? -edgeWidth : 0.0);
            const double outer = fillType == 1 ? 0.5 * edgeWidth : (fillType == 2 ? 0.0 : edgeWidth);
            std::vector<Pt> line;
            double carry = 0.0; // distance into the next edge where the next point goes
            int side = 0;
            for (std::size_t i = 0; i < p.size() && line.size() < kMaxStrokeSegments; ++i) {
                const auto& a = p[i];
                const auto& b = p[(i + 1) % p.size()];
                const double len = std::hypot(b[0] - a[0], b[1] - a[1]);
                if (len <= 1e-9) {
                    continue;
                }
                const double dx = (b[0] - a[0]) / len;
                const double dy = (b[1] - a[1]) / len;
                const double ox = dy * outward;
                const double oy = -dx * outward;
                double s = carry;
                for (; s < len && line.size() < kMaxStrokeSegments; s += spacing) {
                    const double off = side % 2 == 0 ? inner : outer;
                    line.push_back({a[0] + dx * s + ox * off + rnd(4u, point) * jitter,
                                    a[1] + dy * s + oy * off + rnd(5u, point) * jitter, strokeWidth});
                    ++point;
                    ++side;
                }
                carry = s - len;
            }
            if (line.size() >= 2) {
                lines.push_back(std::move(line));
            }
        }
    }

    // Start and End: the part of the whole path between them (the strokes in order, their gaps not counted).
    double total = 0.0;
    for (const auto& line : lines) {
        for (std::size_t i = 1; i < line.size(); ++i) {
            total += std::hypot(line[i].x - line[i - 1].x, line[i].y - line[i - 1].y);
        }
    }
    const double from = start * total;
    const double to = end * total;
    double at = 0.0;
    for (const auto& line : lines) {
        for (std::size_t i = 1; i < line.size() && out.size() < kMaxStrokeSegments; ++i) {
            const Pt& a = line[i - 1];
            const Pt& b = line[i];
            const double len = std::hypot(b.x - a.x, b.y - a.y);
            const double s0 = at;
            const double s1 = at + len;
            at = s1;
            if (s1 <= from || s0 >= to || len <= 0.0) {
                continue;
            }
            const double t0 = std::max(0.0, (from - s0) / len);
            const double t1 = std::min(1.0, (to - s0) / len);
            out.push_back({a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0, a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1,
                           strokeWidth, strokeWidth});
        }
    }
    return out;
}

// ---- Shading and packing ---------------------------------------------------------------------------------------------

std::array<double, 2> strokeShade(std::span<const StrokeSegment> segments, StrokeKind kind, double coreWidth, double x,
                                  double y) {
    double inside = 0.0;
    double outside = 0.0;
    const double core = clamp01(coreWidth);
    const double cut = std::exp(-4.5); // the glow at 3 sigma, subtracted so it ends at 0
    for (const StrokeSegment& s : segments) {
        const double vx = s.bx - s.ax;
        const double vy = s.by - s.ay;
        const double len2 = vx * vx + vy * vy;
        const double t = len2 > 0.0 ? std::clamp(((x - s.ax) * vx + (y - s.ay) * vy) / len2, 0.0, 1.0) : 0.0;
        const double d = std::hypot(x - (s.ax + t * vx), y - (s.ay + t * vy));
        const double w = s.wa + (s.wb - s.wa) * t;
        if (kind == StrokeKind::Line) {
            inside = std::max(inside, clamp01(0.5 * w - d + 0.5));
            continue;
        }
        if (core > 0.0 && w > 0.0) {
            inside = std::max(inside, clamp01(0.5 * w * core - d + 0.5));
        }
        const double sigma = 0.5 * w;
        if (sigma > 0.0 && d < 3.0 * sigma) {
            outside = std::max(outside, (std::exp(-d * d / (2.0 * sigma * sigma)) - cut) / (1.0 - cut));
        }
    }
    return {inside, outside};
}

StrokeTexture packStrokes(std::span<const StrokeSegment> segments, StrokeKind kind, double coreWidth) {
    const std::size_t count = std::min(segments.size(), kMaxStrokeSegments);
    // The grid over every segment's reach.
    double x0 = std::numeric_limits<double>::max();
    double y0 = std::numeric_limits<double>::max();
    double x1 = std::numeric_limits<double>::lowest();
    double y1 = std::numeric_limits<double>::lowest();
    std::vector<std::array<double, 4>> boxes(count);
    for (std::size_t i = 0; i < count; ++i) {
        const StrokeSegment& s = segments[i];
        const double r = strokeReach(kind, std::max(s.wa, s.wb), coreWidth);
        boxes[i] = {std::min(s.ax, s.bx) - r, std::min(s.ay, s.by) - r, std::max(s.ax, s.bx) + r, std::max(s.ay, s.by) + r};
        x0 = std::min(x0, boxes[i][0]);
        y0 = std::min(y0, boxes[i][1]);
        x1 = std::max(x1, boxes[i][2]);
        y1 = std::max(y1, boxes[i][3]);
    }
    int gridW = 1;
    int gridH = 1;
    double cellW = 1.0;
    double cellH = 1.0;
    if (count > 0) {
        const double cell = std::max(8.0, std::max(x1 - x0, y1 - y0) / 64.0);
        gridW = std::clamp(static_cast<int>(std::ceil((x1 - x0) / cell)), 1, 64);
        gridH = std::clamp(static_cast<int>(std::ceil((y1 - y0) / cell)), 1, 64);
        cellW = std::max((x1 - x0) / gridW, 1e-3);
        cellH = std::max((y1 - y0) / gridH, 1e-3);
    } else {
        x0 = y0 = 0.0;
    }
    const std::size_t cells = static_cast<std::size_t>(gridW) * static_cast<std::size_t>(gridH);
    std::vector<std::vector<std::uint32_t>> lists(cells);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& b = boxes[i];
        const int cx0 = std::clamp(static_cast<int>(std::floor((b[0] - x0) / cellW)), 0, gridW - 1);
        const int cy0 = std::clamp(static_cast<int>(std::floor((b[1] - y0) / cellH)), 0, gridH - 1);
        const int cx1 = std::clamp(static_cast<int>(std::floor((b[2] - x0) / cellW)), 0, gridW - 1);
        const int cy1 = std::clamp(static_cast<int>(std::floor((b[3] - y0) / cellH)), 0, gridH - 1);
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                lists[static_cast<std::size_t>(cy) * static_cast<std::size_t>(gridW) + static_cast<std::size_t>(cx)].push_back(
                    static_cast<std::uint32_t>(i));
            }
        }
    }
    std::size_t indices = 0;
    for (const auto& l : lists) {
        indices += l.size();
    }
    const std::size_t firstIndex = 3 + cells;
    const std::size_t segmentBase = firstIndex + (indices + 3) / 4;
    const std::size_t texels = segmentBase + 2 * count;
    StrokeTexture t;
    t.width = kStrokeTextureWidth;
    t.height = static_cast<int>(std::max<std::size_t>(1, (texels + kStrokeTextureWidth - 1) / kStrokeTextureWidth));
    t.texels.assign(static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height) * 4, 0.0f);
    const auto put = [&](std::size_t texel, double a, double b, double c, double d) {
        float* o = t.texels.data() + texel * 4;
        o[0] = static_cast<float>(a);
        o[1] = static_cast<float>(b);
        o[2] = static_cast<float>(c);
        o[3] = static_cast<float>(d);
    };
    put(0, static_cast<double>(count), gridW, gridH, static_cast<double>(firstIndex));
    put(1, x0, y0, cellW, cellH);
    put(2, static_cast<double>(segmentBase), static_cast<double>(kind), clamp01(coreWidth), 0.0);
    std::size_t at = 0;
    for (std::size_t c = 0; c < cells; ++c) {
        put(3 + c, static_cast<double>(at), static_cast<double>(lists[c].size()), 0.0, 0.0);
        for (const std::uint32_t s : lists[c]) {
            t.texels[firstIndex * 4 + at] = static_cast<float>(s);
            ++at;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const StrokeSegment& s = segments[i];
        put(segmentBase + 2 * i, s.ax, s.ay, s.bx, s.by);
        put(segmentBase + 2 * i + 1, s.wa, s.wb, 0.0, 0.0);
    }
    return t;
}

} // namespace wabcut::render::reference
