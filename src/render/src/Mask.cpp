#include <wabcut/render/Mask.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wabcut::render {

namespace {

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 1099511628211ull;
}

std::uint64_t bits(double v) {
    std::uint64_t u = 0;
    std::memcpy(&u, &v, sizeof u);
    return u;
}

Vec2 bezier(Vec2 a, Vec2 c1, Vec2 c2, Vec2 b, double t) {
    const double s = 1.0 - t;
    const double w0 = s * s * s;
    const double w1 = 3.0 * s * s * t;
    const double w2 = 3.0 * s * t * t;
    const double w3 = t * t * t;
    return {w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * b.x, w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * b.y};
}

double smoothstep(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

} // namespace

std::vector<Vec2> flattenOutline(std::span<const OutlinePoint> outline) {
    std::vector<Vec2> out;
    const std::size_t n = outline.size();
    if (n < 2) {
        return out;
    }
    out.reserve(n * kMaskSegmentsPerCurve);
    for (std::size_t i = 0; i < n; ++i) {
        const OutlinePoint& a = outline[i];
        const OutlinePoint& b = outline[(i + 1) % n];
        out.push_back(a.p);
        const bool straight = a.out.x == 0.0 && a.out.y == 0.0 && b.in.x == 0.0 && b.in.y == 0.0;
        if (straight) {
            continue;
        }
        const Vec2 c1{a.p.x + a.out.x, a.p.y + a.out.y};
        const Vec2 c2{b.p.x + b.in.x, b.p.y + b.in.y};
        for (int k = 1; k < kMaskSegmentsPerCurve; ++k) {
            out.push_back(bezier(a.p, c1, c2, b.p, static_cast<double>(k) / kMaskSegmentsPerCurve));
        }
    }
    return out;
}

void MaskSpec::seal() {
    std::uint64_t all = mix(1469598103934665603ull, bits(aspect));
    for (MaskShapeSpec& s : shapes) {
        std::uint64_t h = mix(1469598103934665603ull, bits(aspect)); // the distances are in aspect space
        for (const Vec2& p : s.polygon) {
            h = mix(mix(h, bits(p.x)), bits(p.y));
        }
        if (s.matte) { // AI-14: the matte's distance field is the shape's distance texture
            h = mix(h, s.matte->identity);
        }
        s.key = h;
        all = mix(all, h);
        for (const double v : s.affine) {
            all = mix(all, bits(v));
        }
        all = mix(mix(mix(all, bits(s.feather)), bits(s.expansion)), bits(s.opacity));
        all = mix(all, (s.inverted ? 1u : 0u) | (static_cast<std::uint64_t>(s.mode) << 1));
    }
    key = all;
}

double maskSignedDistance(std::span<const Vec2> polygon, Vec2 p, double aspect) {
    const std::size_t n = polygon.size();
    if (n < 3) {
        return -1e9;
    }
    const double px = p.x * aspect;
    const double py = p.y;
    double best = 1e300;
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double x0 = polygon[j].x * aspect;
        const double y0 = polygon[j].y;
        const double x1 = polygon[i].x * aspect;
        const double y1 = polygon[i].y;
        const double dx = x1 - x0;
        const double dy = y1 - y0;
        const double len2 = dx * dx + dy * dy;
        const double t = len2 > 0.0 ? std::clamp(((px - x0) * dx + (py - y0) * dy) / len2, 0.0, 1.0) : 0.0;
        const double ex = px - (x0 + t * dx);
        const double ey = py - (y0 + t * dy);
        best = std::min(best, ex * ex + ey * ey);
        if ((y0 > py) != (y1 > py) && px < x0 + (py - y0) * dx / dy) {
            inside = !inside;
        }
    }
    const double d = std::sqrt(best);
    return inside ? d : -d;
}

std::shared_ptr<const MaskSpec> maskSpecAt(std::span<const Mask> masks, Ticks curveTime, double frameAspect, MatteUse use,
                                           std::optional<Ticks> matteTime) {
    auto spec = std::make_shared<MaskSpec>();
    spec->aspect = frameAspect > 0.0 && std::isfinite(frameAspect) ? frameAspect : 16.0 / 9.0;
    for (const Mask& mask : masks) {
        if (!mask.enabled) {
            continue;
        }
        MaskShapeSpec s;
        const std::vector<MaskVertex> shape = maskShapeAt(mask, curveTime, spec->aspect);
        std::vector<OutlinePoint> outline;
        outline.reserve(shape.size());
        for (const MaskVertex& v : shape) {
            outline.push_back({{v.x, v.y}, {v.inX, v.inY}, {v.outX, v.outY}});
        }
        s.polygon = flattenOutline(outline);
        const MaskAffine m = maskTransformAt(mask, curveTime, spec->aspect);
        s.affine = {m.a, m.b, m.c, m.d, m.e, m.f};
        s.feather = std::max(0.0, maskValue(mask, "feather", curveTime));
        s.expansion = maskValue(mask, "expansion", curveTime);
        s.opacity = std::clamp(maskValue(mask, "opacity", curveTime), 0.0, 1.0);
        s.inverted = mask.inverted;
        s.mode = static_cast<std::uint8_t>(mask.mode);
        // AI-14 hook: a matte mask's shape at the frame is its matte (render/Matte.h), looked up at the media time the
        // frame shows. Without a usable one the polygon above, the bounding box, stands in; a redaction then grows it by
        // a quarter (fail closed: it never covers nothing) - also when the field is broken or the mask has no matte
        // reference at all, on the GPU and the CPU paths alike.
        if (mask.shape == MaskShape::Matte) {
            if (mask.matte) {
                if (const std::shared_ptr<MatteSource> source = matteSource()) {
                    s.matte = source->fieldAt(*mask.matte, matteTime.value_or(curveTime), spec->aspect, use);
                    if (s.matte && !usableField(*s.matte)) {
                        s.matte = nullptr;
                    }
                }
            }
            if (!s.matte && use == MatteUse::Redaction) {
                s.expansion += 0.125 * std::max(std::abs(maskValue(mask, "size.x", curveTime)) * spec->aspect,
                                                std::abs(maskValue(mask, "size.y", curveTime)));
            }
        }
        spec->shapes.push_back(std::move(s));
    }
    if (spec->shapes.empty()) {
        return nullptr;
    }
    spec->seal();
    return spec;
}

std::vector<float> maskCoverageCpu(const MaskSpec& spec, int width, int height) {
    std::vector<float> out(static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0)), 0.0f);
    if (width <= 0 || height <= 0 || spec.shapes.empty()) {
        return out;
    }
    const double aa = 1.0 / height; // one texel, frame heights
    struct Shape {
        const MaskShapeSpec* spec;
        std::array<double, 6> inverse; // frame -> shape, normalised
        double scale;
        bool empty = false; ///< covers nothing: fewer than three points or a collapsed transform
        bool matte = false; ///< a usable matte field gives the distances
    };
    std::vector<Shape> shapes;
    for (const MaskShapeSpec& s : spec.shapes) {
        const auto& m = s.affine;
        const double det = m[0] * m[4] - m[1] * m[3];
        const bool matte = s.matte && usableField(*s.matte);
        if (std::abs(det) < 1e-12 || (!matte && s.polygon.size() < 3)) {
            // Covers nothing, but still takes part: inverted it covers everything, as an Intersect it clears everything
            // (docs/masks.md; Codex wave-4 P2-31). The GPU does the same.
            shapes.push_back({&s, {}, 0.0, true, false});
            continue;
        }
        const double ia = m[4] / det;
        const double ib = -m[1] / det;
        const double id = -m[3] / det;
        const double ie = m[0] / det;
        shapes.push_back({&s, {ia, ib, -(ia * m[2] + ib * m[5]), id, ie, -(id * m[2] + ie * m[5])}, std::sqrt(std::abs(det)), false, matte});
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Vec2 p{(x + 0.5) / width, (y + 0.5) / height};
            const MaskShapeSpec* first = spec.shapes.front().mode == 0 ? nullptr : &spec.shapes.front();
            double m = first ? 1.0 : 0.0; // add starts empty; subtract and intersect start full
            for (const Shape& s : shapes) {
                const Vec2 q{s.inverse[0] * p.x + s.inverse[1] * p.y + s.inverse[2], s.inverse[3] * p.x + s.inverse[4] * p.y + s.inverse[5]};
                // AI-14: a matte's own distances; a broken field falls back to the polygon (the bounding box), as on the GPU.
                const double raw = s.matte ? matteDistance(*s.spec->matte, q) : maskSignedDistance(s.spec->polygon, q, spec.aspect);
                const double d = s.empty ? -kMaskEmptyDistance : raw * s.scale + s.spec->expansion;
                double a = s.spec->feather > aa ? smoothstep(-s.spec->feather / 2.0, s.spec->feather / 2.0, d)
                                                : std::clamp(0.5 + d / aa, 0.0, 1.0);
                a = s.spec->inverted ? 1.0 - a : a;
                a *= std::clamp(s.spec->opacity, 0.0, 1.0);
                switch (s.spec->mode) {
                case 1:
                    m *= 1.0 - a;
                    break;
                case 2:
                    m *= a;
                    break;
                default:
                    m = m + a - m * a;
                    break;
                }
            }
            out[static_cast<std::size_t>(y) * width + x] = static_cast<float>(m);
        }
    }
    return out;
}

} // namespace wabcut::render
