#include <wabcut/masks/Tracker.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

namespace wabcut::masks {

namespace {

/// Central-difference gradients of a picture (pixels on the grid).
struct Gradients {
    GrayImage x;
    GrayImage y;
};

Gradients gradientsOf(const GrayImage& image) {
    Gradients g{GrayImage(image.width, image.height), GrayImage(image.width, image.height)};
    const int w = image.width;
    for (int y = 0; y < image.height; ++y) {
        const bool inner = y > 0 && y + 1 < image.height;
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (inner && x > 0 && x + 1 < w) {
                g.x.px[i] = 0.5f * (image.px[i + 1] - image.px[i - 1]);
                g.y.px[i] = 0.5f * (image.px[i + static_cast<std::size_t>(w)] - image.px[i - static_cast<std::size_t>(w)]);
            } else {
                g.x.px[i] = 0.5f * (image.at(x + 1, y) - image.at(x - 1, y));
                g.y.px[i] = 0.5f * (image.at(x, y + 1) - image.at(x, y - 1));
            }
        }
    }
    return g;
}

/// The (2 half + 1)^2 window of `image` centred on (cx, cy), bilinear: every sample shares the same fraction, so the
/// weights are computed once (and the edge clamp only where the window leaves the picture).
void sampleWindow(const GrayImage& image, double cx, double cy, int half, float* out) {
    // Past the edge, the clamped samples are all the edge's: a centre further out than the window reads the same values,
    // so clamp it there before converting to int (a diverged guess could otherwise exceed INT_MAX: undefined behaviour;
    // Codex wave-4 P2-37). NaN reads the corner; the caller rejects the point anyway.
    const double margin = half + 2.0;
    cx = std::isfinite(cx) ? std::clamp(cx, -margin, image.width - 1 + margin) : -margin;
    cy = std::isfinite(cy) ? std::clamp(cy, -margin, image.height - 1 + margin) : -margin;
    const double fx = std::floor(cx);
    const double fy = std::floor(cy);
    const auto u = static_cast<float>(cx - fx);
    const auto v = static_cast<float>(cy - fy);
    const int n = 2 * half + 1;
    const int x0 = static_cast<int>(fx) - half;
    const int y0 = static_cast<int>(fy) - half;
    const float w00 = (1.0f - u) * (1.0f - v), w10 = u * (1.0f - v), w01 = (1.0f - u) * v, w11 = u * v;
    if (x0 >= 0 && y0 >= 0 && x0 + n < image.width && y0 + n < image.height) {
        const auto stride = static_cast<std::size_t>(image.width);
        for (int j = 0; j < n; ++j) {
            const float* r0 = image.px.data() + static_cast<std::size_t>(y0 + j) * stride + x0;
            const float* r1 = r0 + stride;
            float* o = out + static_cast<std::size_t>(j) * n;
            for (int i = 0; i < n; ++i) {
                o[i] = w00 * r0[i] + w10 * r0[i + 1] + w01 * r1[i] + w11 * r1[i + 1];
            }
        }
        return;
    }
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            out[static_cast<std::size_t>(j) * n + i] = w00 * image.at(x0 + i, y0 + j) + w10 * image.at(x0 + i + 1, y0 + j) +
                                                       w01 * image.at(x0 + i, y0 + j + 1) + w11 * image.at(x0 + i + 1, y0 + j + 1);
        }
    }
}

GrayImage halve(const GrayImage& in) {
    // 5-tap binomial blur, separable, then every other pixel.
    constexpr float k[5] = {1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16};
    GrayImage rows(in.width, in.height);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            float s = 0.0f;
            for (int t = -2; t <= 2; ++t) {
                s += k[t + 2] * in.at(x + t, y);
            }
            rows.px[static_cast<std::size_t>(y) * in.width + x] = s;
        }
    }
    GrayImage out((in.width + 1) / 2, (in.height + 1) / 2);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            float s = 0.0f;
            for (int t = -2; t <= 2; ++t) {
                s += k[t + 2] * rows.at(2 * x, 2 * y + t);
            }
            out.px[static_cast<std::size_t>(y) * out.width + x] = s;
        }
    }
    return out;
}

// ---- Motion estimates (least squares) -----------------------------------------------------------------------------

std::optional<Motion> translationOf(std::span<const Point2> a, std::span<const Point2> b, std::span<const std::size_t> use) {
    if (use.empty()) {
        return std::nullopt;
    }
    double tx = 0.0;
    double ty = 0.0;
    for (const std::size_t i : use) {
        tx += b[i].x - a[i].x;
        ty += b[i].y - a[i].y;
    }
    const double n = static_cast<double>(use.size());
    return Motion{1.0, 0.0, tx / n, 0.0, 1.0, ty / n};
}

std::optional<Motion> similarityOf(std::span<const Point2> a, std::span<const Point2> b, std::span<const std::size_t> use) {
    if (use.size() < 2) {
        return std::nullopt;
    }
    Point2 ma, mb;
    for (const std::size_t i : use) {
        ma.x += a[i].x, ma.y += a[i].y, mb.x += b[i].x, mb.y += b[i].y;
    }
    const double n = static_cast<double>(use.size());
    ma.x /= n, ma.y /= n, mb.x /= n, mb.y /= n;
    double sxx = 0.0, sp = 0.0, sq = 0.0;
    for (const std::size_t i : use) {
        const double ax = a[i].x - ma.x, ay = a[i].y - ma.y, bx = b[i].x - mb.x, by = b[i].y - mb.y;
        sxx += ax * ax + ay * ay;
        sp += ax * bx + ay * by;
        sq += ax * by - ay * bx;
    }
    if (sxx < 1e-9) {
        return std::nullopt;
    }
    const double p = sp / sxx;
    const double q = sq / sxx;
    return Motion{p, -q, mb.x - (p * ma.x - q * ma.y), q, p, mb.y - (q * ma.x + p * ma.y)};
}

std::optional<Motion> affineOf(std::span<const Point2> a, std::span<const Point2> b, std::span<const std::size_t> use) {
    if (use.size() < 3) {
        return std::nullopt;
    }
    Point2 ma, mb;
    for (const std::size_t i : use) {
        ma.x += a[i].x, ma.y += a[i].y, mb.x += b[i].x, mb.y += b[i].y;
    }
    const double n = static_cast<double>(use.size());
    ma.x /= n, ma.y /= n, mb.x /= n, mb.y /= n;
    double xx = 0, xy = 0, yy = 0, ux = 0, uy = 0, vx = 0, vy = 0;
    for (const std::size_t i : use) {
        const double ax = a[i].x - ma.x, ay = a[i].y - ma.y, bx = b[i].x - mb.x, by = b[i].y - mb.y;
        xx += ax * ax, xy += ax * ay, yy += ay * ay;
        ux += ax * bx, uy += ay * bx, vx += ax * by, vy += ay * by;
    }
    const double det = xx * yy - xy * xy;
    if (std::abs(det) < 1e-9 * std::max(1.0, xx * yy)) {
        return std::nullopt; // collinear
    }
    const double ia = yy / det, ib = -xy / det, id = xx / det;
    const double ma_ = ia * ux + ib * uy, mb_ = ib * ux + id * uy;
    const double md = ia * vx + ib * vy, me = ib * vx + id * vy;
    return Motion{ma_, mb_, mb.x - (ma_ * ma.x + mb_ * ma.y), md, me, mb.y - (md * ma.x + me * ma.y)};
}

std::optional<Motion> estimate(MotionModel model, std::span<const Point2> a, std::span<const Point2> b, std::span<const std::size_t> use) {
    switch (model) {
    case MotionModel::Translation:
        return translationOf(a, b, use);
    case MotionModel::Similarity:
        return similarityOf(a, b, use);
    case MotionModel::Affine:
        return affineOf(a, b, use);
    }
    return std::nullopt;
}

double residual(const Motion& m, Point2 a, Point2 b) {
    const Point2 p = m.map(a);
    return std::hypot(p.x - b.x, p.y - b.y);
}

} // namespace

// ---- GrayImage, Motion ------------------------------------------------------------------------------------------

float GrayImage::at(int x, int y) const {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return px[static_cast<std::size_t>(y) * width + x];
}

float GrayImage::sample(double x, double y) const {
    const double fx = std::floor(x);
    const double fy = std::floor(y);
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const auto u = static_cast<float>(x - fx);
    const auto v = static_cast<float>(y - fy);
    const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return (a * (1.0f - u) + b * u) * (1.0f - v) + (c * (1.0f - u) + d * u) * v;
}

Motion Motion::after(const Motion& m) const {
    return {a * m.a + b * m.d, a * m.b + b * m.e, a * m.c + b * m.f + c, d * m.a + e * m.d, d * m.b + e * m.e, d * m.c + e * m.f + f};
}

std::optional<Motion> Motion::inverted() const {
    const double det = determinant();
    if (std::abs(det) < 1e-12) {
        return std::nullopt;
    }
    const double ia = e / det, ib = -b / det, id = -d / det, ie = a / det;
    return Motion{ia, ib, -(ia * c + ib * f), id, ie, -(id * c + ie * f)};
}

bool insidePolygon(std::span<const Point2> polygon, Point2 p) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Point2 a = polygon[j];
        const Point2 b = polygon[i];
        if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y)) {
            inside = !inside;
        }
    }
    return inside;
}

// ---- Pyramid, features ------------------------------------------------------------------------------------------

std::vector<GrayImage> buildPyramid(const GrayImage& image, int levels) {
    std::vector<GrayImage> out;
    out.push_back(image);
    while (static_cast<int>(out.size()) < levels && out.back().width >= 32 && out.back().height >= 32) {
        out.push_back(halve(out.back()));
    }
    return out;
}

std::vector<Point2> goodFeatures(const GrayImage& image, std::span<const Point2> polygon, int maxPoints, double quality,
                                 double minDistance) {
    std::vector<Point2> out;
    if (image.empty() || maxPoints <= 0) {
        return out;
    }
    constexpr int kBlock = 2; // 5x5 structure tensor
    constexpr int kBorder = 4;
    int x0 = kBorder, y0 = kBorder, x1 = image.width - kBorder, y1 = image.height - kBorder;
    if (!polygon.empty()) {
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        for (const Point2& p : polygon) {
            minX = std::min(minX, p.x), minY = std::min(minY, p.y), maxX = std::max(maxX, p.x), maxY = std::max(maxY, p.y);
        }
        x0 = std::max(x0, static_cast<int>(std::floor(minX)));
        y0 = std::max(y0, static_cast<int>(std::floor(minY)));
        x1 = std::min(x1, static_cast<int>(std::ceil(maxX)) + 1);
        y1 = std::min(y1, static_cast<int>(std::ceil(maxY)) + 1);
    }
    if (x1 - x0 < 3 || y1 - y0 < 3) {
        return out;
    }
    const int w = x1 - x0;
    const int h = y1 - y0;
    // Integral images of the tensor's products over the box (plus the block's reach).
    const int iw = w + 2 * kBlock + 1;
    const int ih = h + 2 * kBlock + 1;
    std::vector<double> sxx(static_cast<std::size_t>(iw) * ih, 0.0), syy(sxx.size(), 0.0), sxy(sxx.size(), 0.0);
    for (int y = 1; y < ih; ++y) {
        for (int x = 1; x < iw; ++x) {
            const int ix = x0 - kBlock + x - 1;
            const int iy = y0 - kBlock + y - 1;
            const double gx = 0.5 * (image.at(ix + 1, iy) - image.at(ix - 1, iy));
            const double gy = 0.5 * (image.at(ix, iy + 1) - image.at(ix, iy - 1));
            const std::size_t i = static_cast<std::size_t>(y) * iw + x;
            const std::size_t up = i - iw;
            sxx[i] = gx * gx + sxx[i - 1] + sxx[up] - sxx[up - 1];
            syy[i] = gy * gy + syy[i - 1] + syy[up] - syy[up - 1];
            sxy[i] = gx * gy + sxy[i - 1] + sxy[up] - sxy[up - 1];
        }
    }
    const auto box = [&](const std::vector<double>& s, int x, int y) {
        // pixel (x0 + x, y0 + y): integral rows/cols x .. x + 2 kBlock + 1
        const int ax = x, ay = y, bx = x + 2 * kBlock + 1, by = y + 2 * kBlock + 1;
        return s[static_cast<std::size_t>(by) * iw + bx] - s[static_cast<std::size_t>(ay) * iw + bx] -
               s[static_cast<std::size_t>(by) * iw + ax] + s[static_cast<std::size_t>(ay) * iw + ax];
    };
    std::vector<float> score(static_cast<std::size_t>(w) * h, 0.0f);
    float best = 0.0f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!polygon.empty() && !insidePolygon(polygon, {static_cast<double>(x0 + x), static_cast<double>(y0 + y)})) {
                continue;
            }
            const double a = box(sxx, x, y), c = box(syy, x, y), b = box(sxy, x, y);
            const double lambda = 0.5 * (a + c) - std::sqrt(0.25 * (a - c) * (a - c) + b * b);
            score[static_cast<std::size_t>(y) * w + x] = static_cast<float>(lambda);
            best = std::max(best, static_cast<float>(lambda));
        }
    }
    if (best <= 0.0f) {
        return out;
    }
    struct Candidate {
        float score;
        int x, y;
    };
    std::vector<Candidate> candidates;
    const auto threshold = static_cast<float>(quality * best);
    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const float s = score[static_cast<std::size_t>(y) * w + x];
            if (s <= threshold) {
                continue;
            }
            bool peak = true;
            for (int dy = -1; dy <= 1 && peak; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx || dy) && score[static_cast<std::size_t>(y + dy) * w + x + dx] > s) {
                        peak = false;
                        break;
                    }
                }
            }
            if (peak) {
                candidates.push_back({s, x0 + x, y0 + y});
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& p, const Candidate& q) { return p.score > q.score; });
    const double d2 = minDistance * minDistance;
    for (const Candidate& c : candidates) {
        const Point2 p{static_cast<double>(c.x), static_cast<double>(c.y)};
        const bool far = std::none_of(out.begin(), out.end(), [&](const Point2& q) {
            return (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) < d2;
        });
        if (far) {
            out.push_back(p);
            if (static_cast<int>(out.size()) >= maxPoints) {
                break;
            }
        }
    }
    return out;
}

// ---- Lucas-Kanade -------------------------------------------------------------------------------------------------

std::vector<PointTrack> trackPoints(const std::vector<GrayImage>& from, const std::vector<GrayImage>& to,
                                    std::span<const Point2> points, const TrackerOptions& options) {
    std::vector<PointTrack> out(points.size());
    const int levels = static_cast<int>(std::min(from.size(), to.size()));
    if (levels == 0) {
        return out;
    }
    std::vector<Gradients> gradients;
    gradients.reserve(static_cast<std::size_t>(levels));
    for (int l = 0; l < levels; ++l) {
        gradients.push_back(gradientsOf(from[static_cast<std::size_t>(l)]));
    }
    const int half = std::max(1, options.window / 2);
    const std::size_t n = static_cast<std::size_t>(2 * half + 1) * static_cast<std::size_t>(2 * half + 1);
    std::vector<float> wi(n), wx(n), wy(n), wj(n);
    for (std::size_t k = 0; k < points.size(); ++k) {
        const Point2 p = points[k];
        double gx = 0.0, gy = 0.0; // guess at the current level
        bool ok = true;
        for (int l = levels - 1; l >= 0 && ok; --l) {
            const GrayImage& I = from[static_cast<std::size_t>(l)];
            const GrayImage& J = to[static_cast<std::size_t>(l)];
            const Gradients& G = gradients[static_cast<std::size_t>(l)];
            const double scale = std::ldexp(1.0, -l);
            const double px = p.x * scale, py = p.y * scale;
            double g00 = 0.0, g01 = 0.0, g11 = 0.0;
            sampleWindow(I, px, py, half, wi.data());
            sampleWindow(G.x, px, py, half, wx.data());
            sampleWindow(G.y, px, py, half, wy.data());
            for (std::size_t i = 0; i < n; ++i) {
                g00 += static_cast<double>(wx[i]) * wx[i];
                g01 += static_cast<double>(wx[i]) * wy[i];
                g11 += static_cast<double>(wy[i]) * wy[i];
            }
            const double det = g00 * g11 - g01 * g01;
            if (det < 1e-12 * static_cast<double>(n) * static_cast<double>(n)) {
                ok = l > 0; // flat at a coarse level: keep the guess and refine below
                gx *= l > 0 ? 2.0 : 1.0;
                gy *= l > 0 ? 2.0 : 1.0;
                continue;
            }
            double vx = 0.0, vy = 0.0;
            for (int it = 0; it < options.iterations; ++it) {
                float bxf = 0.0f, byf = 0.0f;
                sampleWindow(J, px + gx + vx, py + gy + vy, half, wj.data());
                for (std::size_t i = 0; i < n; ++i) {
                    const float diff = wi[i] - wj[i];
                    bxf += diff * wx[i];
                    byf += diff * wy[i];
                }
                const double bx = bxf;
                const double by = byf;
                const double ex = (g11 * bx - g01 * by) / det;
                const double ey = (g00 * by - g01 * bx) / det;
                vx += ex;
                vy += ey;
                // Diverged: not a number, or the window has left the picture (everything there is the clamped edge, so
                // no further step can mean anything). Codex wave-4 P2-37.
                const double qx = px + gx + vx, qy = py + gy + vy;
                if (!std::isfinite(vx) || !std::isfinite(vy) || qx < -half || qy < -half || qx > J.width - 1 + half ||
                    qy > J.height - 1 + half) {
                    ok = false;
                    break;
                }
                if (ex * ex + ey * ey < options.epsilon * options.epsilon) {
                    break;
                }
            }
            if (l > 0) {
                gx = 2.0 * (gx + vx);
                gy = 2.0 * (gy + vy);
            } else {
                gx += vx;
                gy += vy;
            }
        }
        const Point2 q{p.x + gx, p.y + gy};
        const GrayImage& base = to.front();
        out[k].to = q;
        out[k].ok = ok && std::isfinite(q.x) && std::isfinite(q.y) && q.x >= half && q.y >= half && q.x <= base.width - 1 - half &&
                    q.y <= base.height - 1 - half;
    }
    return out;
}

// ---- RANSAC -----------------------------------------------------------------------------------------------------

std::optional<Motion> fitMotion(std::span<const Point2> a, std::span<const Point2> b, MotionModel model, double threshold,
                                int iterations, std::vector<char>* inliers) {
    const std::size_t n = std::min(a.size(), b.size());
    const std::size_t minimal = model == MotionModel::Translation ? 1 : model == MotionModel::Similarity ? 2 : 3;
    if (inliers) {
        inliers->assign(n, 0);
    }
    if (n < minimal) {
        return std::nullopt;
    }
    std::mt19937 rng(0x5eed1e55u);
    std::uniform_int_distribution<std::size_t> pick(0, n - 1);
    std::vector<std::size_t> best;
    double bestError = std::numeric_limits<double>::max();
    std::vector<std::size_t> sample(minimal);
    std::vector<std::size_t> current;
    const int rounds = n == minimal ? 1 : iterations;
    for (int r = 0; r < rounds; ++r) {
        if (n == minimal) {
            std::iota(sample.begin(), sample.end(), std::size_t{0});
        } else {
            for (std::size_t s = 0; s < minimal; ++s) {
                std::size_t i = 0;
                do {
                    i = pick(rng);
                } while (std::find(sample.begin(), sample.begin() + static_cast<std::ptrdiff_t>(s), i) != sample.begin() + static_cast<std::ptrdiff_t>(s));
                sample[s] = i;
            }
        }
        const auto m = estimate(model, a, b, sample);
        if (!m) {
            continue;
        }
        current.clear();
        double error = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double e = residual(*m, a[i], b[i]);
            if (e < threshold) {
                current.push_back(i);
                error += e;
            }
        }
        if (current.size() > best.size() || (current.size() == best.size() && error < bestError)) {
            best = current;
            bestError = error;
        }
    }
    if (best.size() < minimal) {
        return std::nullopt;
    }
    // Least squares on the inliers, twice (the refit may take in a few more).
    std::optional<Motion> fit = estimate(model, a, b, best);
    for (int pass = 0; pass < 2 && fit; ++pass) {
        std::vector<std::size_t> refined;
        for (std::size_t i = 0; i < n; ++i) {
            if (residual(*fit, a[i], b[i]) < threshold) {
                refined.push_back(i);
            }
        }
        if (refined.size() < minimal) {
            break;
        }
        best = std::move(refined);
        fit = estimate(model, a, b, best);
    }
    if (fit && inliers) {
        for (const std::size_t i : best) {
            (*inliers)[i] = 1;
        }
    }
    return fit;
}

// ---- RegionTracker ----------------------------------------------------------------------------------------------

RegionTracker::RegionTracker(TrackerOptions options) : m_options(options) {}

void RegionTracker::start(const GrayImage& first, std::vector<Point2> region) {
    m_pyramid = buildPyramid(first, m_options.levels);
    m_region = std::move(region);
    m_total = Motion{};
}

TrackStep RegionTracker::step(const GrayImage& next) {
    TrackStep out;
    out.lost = true;
    if (m_pyramid.empty() || next.width != m_pyramid.front().width || next.height != m_pyramid.front().height) {
        return out;
    }
    std::vector<GrayImage> pyramid = buildPyramid(next, m_options.levels);
    const std::vector<Point2> features =
        goodFeatures(m_pyramid.front(), m_region, m_options.maxPoints, m_options.featureQuality, m_options.minDistance);
    out.features = static_cast<int>(features.size());
    if (out.features < m_options.minInliers) {
        return out;
    }
    const std::vector<PointTrack> forward = trackPoints(m_pyramid, pyramid, features, m_options);
    std::vector<Point2> moved;
    std::vector<std::size_t> index;
    for (std::size_t i = 0; i < forward.size(); ++i) {
        if (forward[i].ok) {
            moved.push_back(forward[i].to);
            index.push_back(i);
        }
    }
    const std::vector<PointTrack> backward = trackPoints(pyramid, m_pyramid, moved, m_options);
    std::vector<Point2> a;
    std::vector<Point2> b;
    for (std::size_t k = 0; k < backward.size(); ++k) {
        const Point2 origin = features[index[k]];
        if (backward[k].ok && std::hypot(backward[k].to.x - origin.x, backward[k].to.y - origin.y) <= m_options.forwardBackward) {
            a.push_back(origin);
            b.push_back(moved[k]);
        }
    }
    std::vector<char> inliers;
    const auto motion = fitMotion(a, b, m_options.model, m_options.ransacThreshold, m_options.ransacIterations, &inliers);
    if (!motion) {
        return out;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (inliers[i]) {
            ++out.inliers;
            sum += residual(*motion, a[i], b[i]);
        }
    }
    const double mean = out.inliers > 0 ? sum / out.inliers : m_options.ransacThreshold;
    out.motion = *motion;
    out.confidence = std::clamp(static_cast<double>(out.inliers) / out.features * (1.0 - 0.5 * mean / m_options.ransacThreshold), 0.0, 1.0);
    out.lost = out.inliers < m_options.minInliers || out.confidence < m_options.minConfidence;
    if (!out.lost) {
        m_total = motion->after(m_total);
        for (Point2& p : m_region) {
            p = motion->map(p);
        }
        m_pyramid = std::move(pyramid);
    }
    return out;
}

} // namespace wabcut::masks
