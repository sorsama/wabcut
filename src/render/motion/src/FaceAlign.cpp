// Face-aware motion for Morph Cut (FaceAlign.h).

#include <wabcut/motion/FaceAlign.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace wabcut::motion {

FaceFeatures FaceFeatures::scaled(float sx, float sy) const {
    FaceFeatures f = *this;
    f.x *= sx;
    f.y *= sy;
    f.w *= sx;
    f.h *= sy;
    for (FacePoint& p : f.points) {
        p.x *= sx;
        p.y *= sy;
    }
    return f;
}

FacePoint Similarity::map(FacePoint p) const {
    return {static_cast<float>(a * p.x - b * p.y + tx), static_cast<float>(b * p.x + a * p.y + ty)};
}

Similarity Similarity::inverted() const {
    const double n = a * a + b * b;
    if (n < 1e-12) {
        return {};
    }
    // The inverse of z' = m z + t (m = a + ib) is z = (z' - t) / m.
    Similarity r;
    r.a = a / n;
    r.b = -b / n;
    r.tx = -(r.a * tx - r.b * ty);
    r.ty = -(r.b * tx + r.a * ty);
    return r;
}

double Similarity::scale() const { return std::hypot(a, b); }

double Similarity::turnDegrees() const { return std::atan2(b, a) * 180.0 / std::numbers::pi; }

Similarity similarityOf(std::span<const FacePoint> from, std::span<const FacePoint> to) {
    const std::size_t n = std::min(from.size(), to.size());
    if (n < 2) {
        return {};
    }
    double fx = 0.0;
    double fy = 0.0;
    double tx = 0.0;
    double ty = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        fx += from[i].x;
        fy += from[i].y;
        tx += to[i].x;
        ty += to[i].y;
    }
    fx /= static_cast<double>(n);
    fy /= static_cast<double>(n);
    tx /= static_cast<double>(n);
    ty /= static_cast<double>(n);
    double dot = 0.0;
    double cross = 0.0;
    double norm = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double px = from[i].x - fx;
        const double py = from[i].y - fy;
        const double qx = to[i].x - tx;
        const double qy = to[i].y - ty;
        dot += qx * px + qy * py;
        cross += qy * px - qx * py;
        norm += px * px + py * py;
    }
    if (norm < 1e-9) {
        return {};
    }
    Similarity s;
    s.a = dot / norm;
    s.b = cross / norm;
    s.tx = tx - (s.a * fx - s.b * fy);
    s.ty = ty - (s.b * fx + s.a * fy);
    return s;
}

std::vector<FaceMatch> matchFaces(std::span<const FaceFeatures> a, std::span<const FaceFeatures> b) {
    struct Candidate {
        double cost;
        int a;
        int b;
    };
    std::vector<Candidate> candidates;
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (std::size_t j = 0; j < b.size(); ++j) {
            if (!a[i].valid() || !b[j].valid()) {
                continue;
            }
            const double sa = std::sqrt(static_cast<double>(a[i].w) * a[i].h);
            const double sb = std::sqrt(static_cast<double>(b[j].w) * b[j].h);
            const double size = std::max(sa, sb);
            const double ratio = sa / sb;
            const double distance = std::hypot(a[i].centreX() - b[j].centreX(), a[i].centreY() - b[j].centreY()) / size;
            if (distance > 1.0 || ratio < 0.5 || ratio > 2.0) {
                continue;
            }
            candidates.push_back({distance + std::abs(std::log(ratio)), static_cast<int>(i), static_cast<int>(j)});
        }
    }
    std::ranges::sort(candidates, [](const Candidate& x, const Candidate& y) {
        return x.cost != y.cost ? x.cost < y.cost : (x.a != y.a ? x.a < y.a : x.b < y.b);
    });
    std::vector<bool> usedA(a.size(), false);
    std::vector<bool> usedB(b.size(), false);
    std::vector<FaceMatch> out;
    for (const Candidate& c : candidates) {
        if (usedA[static_cast<std::size_t>(c.a)] || usedB[static_cast<std::size_t>(c.b)]) {
            continue;
        }
        usedA[static_cast<std::size_t>(c.a)] = true;
        usedB[static_cast<std::size_t>(c.b)] = true;
        out.push_back({c.a, c.b});
    }
    std::ranges::sort(out, [](const FaceMatch& x, const FaceMatch& y) { return x.a < y.a; });
    return out;
}

namespace {

float smooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Rect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0; ///< exclusive
    int y1 = 0;
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

/// A window of a face (the box `scale` times as large, grown by the feather) clipped to a `width` x `height` plane.
Rect windowOf(const FaceFeatures& f, const FaceAlignOptions& o, int width, int height) {
    const double rx = 0.5 * f.w * o.windowScale * (1.0 + o.feather);
    const double ry = 0.5 * f.h * o.windowScale * (1.0 + o.feather);
    Rect r;
    r.x0 = std::max(0, static_cast<int>(std::floor(f.centreX() - rx)));
    r.y0 = std::max(0, static_cast<int>(std::floor(f.centreY() - ry)));
    r.x1 = std::min(width, static_cast<int>(std::ceil(f.centreX() + rx)) + 1);
    r.y1 = std::min(height, static_cast<int>(std::ceil(f.centreY() + ry)) + 1);
    return r;
}

/// 1 inside the face's ellipse (the box `windowScale` times as large), falling to 0 over `feather` of its radius.
float windowWeight(const FaceFeatures& f, const FaceAlignOptions& o, int x, int y) {
    const double rx = std::max(1.0, 0.5 * f.w * o.windowScale);
    const double ry = std::max(1.0, 0.5 * f.h * o.windowScale);
    const double rho = std::hypot((x - f.centreX()) / rx, (y - f.centreY()) / ry);
    return 1.0f - smooth(1.0f, 1.0f + static_cast<float>(o.feather), static_cast<float>(rho));
}

/// A 5 x 5 box mean (edge clamped).
Plane boxMean5(const Plane& in) {
    Plane tmp(in.width, in.height);
    Plane out(in.width, in.height);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            float s = 0.0f;
            for (int k = -2; k <= 2; ++k) {
                s += in.at(x + k, y);
            }
            tmp(x, y) = s * 0.2f;
        }
    }
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            float s = 0.0f;
            for (int k = -2; k <= 2; ++k) {
                s += tmp.at(x, y + k);
            }
            out(x, y) = s * 0.2f;
        }
    }
    return out;
}

/// |from(x) - to(x + d(x))| over the rect, smoothed over 5 x 5; `disp(x, y)` gives the displacement in pixels.
template <class Disp>
Plane photometricError(const Plane& from, const Plane& to, const Rect& r, Disp&& disp) {
    Plane e(r.w(), r.h());
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const std::pair<float, float> d = disp(x, y);
            e(x - r.x0, y - r.y0) = std::abs(from(x, y) - to.sample(static_cast<float>(x) + d.first, static_cast<float>(y) + d.second));
        }
    }
    return boxMean5(e);
}

/// One direction of one face: the flow from `from` to `to` inside the face's window in `from`, improved. `channel` is 0 for
/// the forward vectors of the pair, 2 for the backward ones. Returns false when the window is too small to estimate in.
bool improveDirection(FlowPair& out, const FlowPair& plain, const Plane& from, const Plane& to, const FaceFeatures& face,
                      const Similarity& toOther, std::size_t channel, const FaceAlignOptions& options, double* errorBefore,
                      double* errorAfter) {
    const int width = plain.width;
    const int height = plain.height;
    const Rect rect = windowOf(face, options, width, height);
    if (rect.w() < 24 || rect.h() < 24) {
        return false;
    }
    // The residual: `from` against `to` carried onto `from`'s frame by the head's motion.
    Plane cropFrom(rect.w(), rect.h());
    Plane warped(rect.w(), rect.h());
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            cropFrom(x - rect.x0, y - rect.y0) = from(x, y);
            const FacePoint q = toOther.map({static_cast<float>(x), static_cast<float>(y)});
            warped(x - rect.x0, y - rect.y0) = to.sample(q.x, q.y);
        }
    }
    FlowOptions flowOptions;
    flowOptions.threads = options.threads;
    const FlowField residual = estimateFlow(cropFrom, warped, flowOptions);
    if (residual.empty()) {
        return false;
    }
    const auto at = [&](int x, int y) {
        const std::size_t i = 4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) + channel;
        return i;
    };
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    const auto plainDisp = [&](int x, int y) {
        const std::size_t i = at(x, y);
        return std::pair<float, float>{plain.data[i] * fw, plain.data[i + 1] * fh};
    };
    std::vector<float> faceU(static_cast<std::size_t>(rect.w()) * static_cast<std::size_t>(rect.h()));
    std::vector<float> faceV(faceU.size());
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            const int lx = x - rect.x0;
            const int ly = y - rect.y0;
            const FacePoint q = toOther.map({static_cast<float>(x) + residual.u(lx, ly), static_cast<float>(y) + residual.v(lx, ly)});
            const std::size_t k = static_cast<std::size_t>(ly) * static_cast<std::size_t>(rect.w()) + static_cast<std::size_t>(lx);
            faceU[k] = q.x - static_cast<float>(x);
            faceV[k] = q.y - static_cast<float>(y);
        }
    }
    const auto faceDisp = [&](int x, int y) {
        const std::size_t k = static_cast<std::size_t>(y - rect.y0) * static_cast<std::size_t>(rect.w()) + static_cast<std::size_t>(x - rect.x0);
        return std::pair<float, float>{faceU[k], faceV[k]};
    };
    const Plane errPlain = photometricError(from, to, rect, plainDisp);
    const Plane errFace = photometricError(from, to, rect, faceDisp);
    // Blend: the window's weight times how much better the face-aligned flow explains the pictures here.
    std::vector<float> weight(faceU.size());
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            const int lx = x - rect.x0;
            const int ly = y - rect.y0;
            const std::size_t k = static_cast<std::size_t>(ly) * static_cast<std::size_t>(rect.w()) + static_cast<std::size_t>(lx);
            const float ep = errPlain(lx, ly);
            const float ef = errFace(lx, ly);
            const float diff = (ep - ef) / (ep + ef + 0.004f);
            const float better = std::clamp(0.65f + 0.9f * diff, 0.0f, 1.0f);
            weight[k] = windowWeight(face, options, x, y) * better;
        }
    }
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            const std::size_t k = static_cast<std::size_t>(y - rect.y0) * static_cast<std::size_t>(rect.w()) + static_cast<std::size_t>(x - rect.x0);
            const std::size_t i = at(x, y);
            const float wgt = weight[k];
            if (wgt <= 0.0f) {
                continue;
            }
            const float u = plain.data[i] * fw + wgt * (faceU[k] - plain.data[i] * fw);
            const float v = plain.data[i + 1] * fh + wgt * (faceV[k] - plain.data[i + 1] * fh);
            out.data[i] = u / fw;
            out.data[i + 1] = v / fh;
        }
    }
    if (errorBefore != nullptr && errorAfter != nullptr) {
        const auto finalDisp = [&](int x, int y) {
            const std::size_t i = at(x, y);
            return std::pair<float, float>{out.data[i] * fw, out.data[i + 1] * fh};
        };
        const Plane errFinal = photometricError(from, to, rect, finalDisp);
        double before = 0.0;
        double after = 0.0;
        int counted = 0;
        for (int y = rect.y0; y < rect.y1; ++y) {
            for (int x = rect.x0; x < rect.x1; ++x) {
                if (windowWeight(face, options, x, y) < 0.5f) {
                    continue;
                }
                before += errPlain(x - rect.x0, y - rect.y0);
                after += errFinal(x - rect.x0, y - rect.y0);
                ++counted;
            }
        }
        if (counted > 0) {
            *errorBefore += before / counted;
            *errorAfter += after / counted;
        }
    }
    return true;
}

} // namespace

FlowPair alignFaces(const Plane& a, const Plane& b, const FlowPair& plain, std::span<const FaceFeatures> facesA,
                    std::span<const FaceFeatures> facesB, const FaceAlignOptions& options, FaceAlignStats* stats) {
    if (stats != nullptr) {
        *stats = {};
    }
    if (plain.empty() || a.empty() || a.width != b.width || a.height != b.height ||
        plain.data.size() != static_cast<std::size_t>(plain.width) * static_cast<std::size_t>(plain.height) * 4) {
        return plain;
    }
    const std::vector<FaceMatch> matches = matchFaces(facesA, facesB);
    if (matches.empty()) {
        return plain;
    }
    const int width = plain.width;
    const int height = plain.height;
    const Plane pa = a.width == width && a.height == height ? a : resize(a, width, height);
    const Plane pb = b.width == width && b.height == height ? b : resize(b, width, height);
    const float sx = static_cast<float>(width) / static_cast<float>(a.width);
    const float sy = static_cast<float>(height) / static_cast<float>(a.height);
    FlowPair out = plain;
    double before = 0.0;
    double after = 0.0;
    int aligned = 0;
    for (const FaceMatch& m : matches) {
        const FaceFeatures fa = facesA[static_cast<std::size_t>(m.a)].scaled(sx, sy);
        const FaceFeatures fb = facesB[static_cast<std::size_t>(m.b)].scaled(sx, sy);
        const Similarity s = similarityOf(fa.points, fb.points);
        // A head does not change its size by more than about a half, nor turn in the picture's plane by more than ~35 degrees,
        // between two shots of one person at one jump cut: landmarks that say so are wrong, and the plain flow stays.
        if (!(s.scale() > 0.6 && s.scale() < 1.67 && std::abs(s.turnDegrees()) < 35.0)) {
            continue;
        }
        double faceBefore = 0.0;
        double faceAfter = 0.0;
        const bool forward = improveDirection(out, plain, pa, pb, fa, s, 0, options, &faceBefore, &faceAfter);
        const bool backward = improveDirection(out, plain, pb, pa, fb, s.inverted(), 2, options, nullptr, nullptr);
        if (forward || backward) {
            ++aligned;
            before += faceBefore;
            after += faceAfter;
        }
    }
    if (stats != nullptr && aligned > 0) {
        stats->faces = aligned;
        stats->errorBefore = before / aligned;
        stats->errorAfter = after / aligned;
    }
    return out;
}

} // namespace wabcut::motion
