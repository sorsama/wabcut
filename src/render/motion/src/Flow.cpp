// Dense Inverse Search optical flow (Flow.h).

#include <wabcut/motion/Flow.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace wabcut::motion {

void FlowField::sample(float x, float y, float& outU, float& outV) const {
    if (empty()) {
        outU = outV = 0.0f;
        return;
    }
    x = x >= 0.0f ? std::min(x, static_cast<float>(width - 1)) : 0.0f; // NaN to the edge
    y = y >= 0.0f ? std::min(y, static_cast<float>(height - 1)) : 0.0f;
    const int x0 = std::min(static_cast<int>(x), width - 1);
    const int y0 = std::min(static_cast<int>(y), height - 1);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto lerp2 = [&](int component) {
        const auto at = [&](int px, int py) {
            return uv[2 * (static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)) +
                      static_cast<std::size_t>(component)];
        };
        const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
        const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
        return top + (bottom - top) * fy;
    };
    outU = lerp2(0);
    outV = lerp2(1);
}

FlowField FlowField::resized(int w, int h) const {
    if (empty() || w <= 0 || h <= 0) {
        return FlowField(std::max(w, 0), std::max(h, 0));
    }
    FlowField out(w, h);
    const float kx = static_cast<float>(w) / static_cast<float>(width);
    const float ky = static_cast<float>(h) / static_cast<float>(height);
    for (int y = 0; y < h; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f) / ky - 0.5f;
        for (int x = 0; x < w; ++x) {
            float u = 0.0f;
            float v = 0.0f;
            sample((static_cast<float>(x) + 0.5f) / kx - 0.5f, sy, u, v);
            out.u(x, y) = u * kx;
            out.v(x, y) = v * ky;
        }
    }
    return out;
}

namespace {

constexpr int kMaxPatch = 32;

/// Patch origins along one axis: every `stride` pixels, and the last one flush with the end.
std::vector<int> origins(int size, int patch, int stride) {
    std::vector<int> out;
    if (size <= patch) {
        out.push_back(0);
        return out;
    }
    for (int o = 0; o + patch <= size; o += stride) {
        out.push_back(o);
    }
    if (out.back() + patch < size) {
        out.push_back(size - patch);
    }
    return out;
}

/// For each coordinate, the first and one-past-last index of the patches (`starts`, size `patch`) covering it.
std::vector<std::pair<int, int>> covering(int size, const std::vector<int>& starts, int patch) {
    std::vector<std::pair<int, int>> out(static_cast<std::size_t>(size));
    int first = 0;
    int last = 0;
    const int n = static_cast<int>(starts.size());
    for (int c = 0; c < size; ++c) {
        while (first < n && starts[static_cast<std::size_t>(first)] + patch <= c) {
            ++first;
        }
        last = std::max(last, first);
        while (last < n && starts[static_cast<std::size_t>(last)] <= c) {
            ++last;
        }
        out[static_cast<std::size_t>(c)] = {first, last};
    }
    return out;
}

/// The displacement of the patch at (ox, oy), side `p`, from `u0, v0`: inverse compositional Gauss-Newton on the
/// zero-mean patch. Keeps the start when the patch is flat, when no step lowers the error, or when it runs away.
void searchPatch(const Plane& i0, const Plane& gx, const Plane& gy, const Plane& i1, int ox, int oy, int p, int iterations,
                 float u0, float v0, float& outU, float& outV) {
    std::array<float, kMaxPatch * kMaxPatch> t{};
    std::array<float, kMaxPatch * kMaxPatch> dx{};
    std::array<float, kMaxPatch * kMaxPatch> dy{};
    std::array<float, kMaxPatch * kMaxPatch> warped{};
    const int n = p * p;
    double meanT = 0.0;
    double meanX = 0.0;
    double meanY = 0.0;
    for (int j = 0; j < p; ++j) {
        for (int i = 0; i < p; ++i) {
            const auto k = static_cast<std::size_t>(j * p + i);
            t[k] = i0(ox + i, oy + j);
            dx[k] = gx(ox + i, oy + j);
            dy[k] = gy(ox + i, oy + j);
            meanT += t[k];
            meanX += dx[k];
            meanY += dy[k];
        }
    }
    meanT /= n;
    meanX /= n;
    meanY /= n;
    double hxx = 0.0;
    double hxy = 0.0;
    double hyy = 0.0;
    for (int k = 0; k < n; ++k) {
        const auto s = static_cast<std::size_t>(k);
        t[s] = static_cast<float>(t[s] - meanT);
        dx[s] = static_cast<float>(dx[s] - meanX);
        dy[s] = static_cast<float>(dy[s] - meanY);
        hxx += static_cast<double>(dx[s]) * dx[s];
        hxy += static_cast<double>(dx[s]) * dy[s];
        hyy += static_cast<double>(dy[s]) * dy[s];
    }
    // A tiny ridge keeps nearly one-dimensional patches (an edge) from stepping along the edge without bound.
    const double ridge = 1e-6 * n;
    hxx += ridge;
    hyy += ridge;
    const double det = hxx * hyy - hxy * hxy;
    outU = u0;
    outV = v0;
    if (!(det > 1e-12)) {
        return; // flat: nothing to match
    }
    float u = u0;
    float v = v0;
    double best = std::numeric_limits<double>::infinity();
    for (int it = 0; it <= iterations; ++it) {
        double sum = 0.0;
        // One displacement for the whole patch: the same bilinear weights at every pixel.
        const float wx = static_cast<float>(ox) + u;
        const float wy = static_cast<float>(oy) + v;
        const float flx = std::floor(wx);
        const float fly = std::floor(wy);
        const int xi = static_cast<int>(flx);
        const int yi = static_cast<int>(fly);
        if (xi >= 0 && yi >= 0 && xi + p < i1.width && yi + p < i1.height) {
            const float fx = wx - flx;
            const float fy = wy - fly;
            const float a00 = (1.0f - fx) * (1.0f - fy);
            const float a10 = fx * (1.0f - fy);
            const float a01 = (1.0f - fx) * fy;
            const float a11 = fx * fy;
            for (int j = 0; j < p; ++j) {
                const float* r0 = i1.px.data() + static_cast<std::size_t>(yi + j) * static_cast<std::size_t>(i1.width) +
                                  static_cast<std::size_t>(xi);
                const float* r1 = r0 + i1.width;
                for (int i = 0; i < p; ++i) {
                    const auto k = static_cast<std::size_t>(j * p + i);
                    warped[k] = a00 * r0[i] + a10 * r0[i + 1] + a01 * r1[i] + a11 * r1[i + 1];
                    sum += warped[k];
                }
            }
        } else {
            for (int j = 0; j < p; ++j) {
                for (int i = 0; i < p; ++i) {
                    const auto k = static_cast<std::size_t>(j * p + i);
                    warped[k] = i1.sample(static_cast<float>(ox + i) + u, static_cast<float>(oy + j) + v);
                    sum += warped[k];
                }
            }
        }
        const double meanI = sum / n;
        double err = 0.0;
        double bx = 0.0;
        double by = 0.0;
        for (int k = 0; k < n; ++k) {
            const auto s = static_cast<std::size_t>(k);
            const double r = (warped[s] - meanI) - t[s];
            err += r * r;
            bx += dx[s] * r;
            by += dy[s] * r;
        }
        if (err < best) {
            best = err;
            outU = u;
            outV = v;
        } else {
            break; // the last step made it worse: keep the best so far
        }
        if (it == iterations) {
            break;
        }
        const double du = (hyy * bx - hxy * by) / det;
        const double dv = (hxx * by - hxy * bx) / det;
        if (!std::isfinite(du) || !std::isfinite(dv)) {
            break; // overflowing values: keep the best so far
        }
        u = static_cast<float>(u - du);
        v = static_cast<float>(v - dv);
        if (du * du + dv * dv < 1e-4) {
            // Converged: one more evaluation decides whether the last step is kept.
            iterations = it + 1;
        }
    }
    if (std::abs(outU - u0) > static_cast<float>(p) || std::abs(outV - v0) > static_cast<float>(p)) {
        outU = u0; // ran away (a repetitive or flat area): keep where it started
        outV = v0;
    }
}

/// The median of nine values (Paeth's 19 compare-exchanges; the array is scrambled).
float median9(std::array<float, 9>& p) {
    const auto sort2 = [&](int a, int b) {
        const float lo = std::min(p[static_cast<std::size_t>(a)], p[static_cast<std::size_t>(b)]);
        const float hi = std::max(p[static_cast<std::size_t>(a)], p[static_cast<std::size_t>(b)]);
        p[static_cast<std::size_t>(a)] = lo;
        p[static_cast<std::size_t>(b)] = hi;
    };
    sort2(1, 2); sort2(4, 5); sort2(7, 8);
    sort2(0, 1); sort2(3, 4); sort2(6, 7);
    sort2(1, 2); sort2(4, 5); sort2(7, 8);
    sort2(0, 3); sort2(5, 8); sort2(4, 7);
    sort2(3, 6); sort2(1, 4); sort2(2, 5);
    sort2(4, 7); sort2(4, 2); sort2(6, 4);
    sort2(4, 2);
    return p[4];
}

/// Component-wise 3 x 3 median of `f`.
FlowField median3(const FlowField& f, int threads) {
    FlowField out(f.width, f.height);
    parallelFor(f.height, threads, [&](int y) {
        std::array<float, 9> us{};
        std::array<float, 9> vs{};
        for (int x = 0; x < f.width; ++x) {
            std::size_t k = 0;
            for (int j = -1; j <= 1; ++j) {
                const int yy = std::clamp(y + j, 0, f.height - 1);
                for (int i = -1; i <= 1; ++i) {
                    const int xx = std::clamp(x + i, 0, f.width - 1);
                    us[k] = f.u(xx, yy);
                    vs[k] = f.v(xx, yy);
                    ++k;
                }
            }
            out.u(x, y) = median9(us);
            out.v(x, y) = median9(vs);
        }
    });
    return out;
}

/// One level: patch search from `flow` (this level's size), then densification into `flow`.
void disLevel(const Plane& i0, const Plane& i1, FlowField& flow, const FlowOptions& o) {
    const int p = std::clamp(std::min({o.patchSize, i0.width, i0.height}), 1, kMaxPatch);
    const int stride = std::clamp(o.patchStride, 1, p);
    Plane gx;
    Plane gy;
    gradients(i0, gx, gy);
    const std::vector<int> xs = origins(i0.width, p, stride);
    const std::vector<int> ys = origins(i0.height, p, stride);
    const int nx = static_cast<int>(xs.size());
    const int ny = static_cast<int>(ys.size());
    std::vector<float> pu(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny));
    std::vector<float> pv(pu.size());
    const float half = 0.5f * static_cast<float>(p - 1);
    parallelFor(ny, o.threads, [&](int py) {
        for (int px = 0; px < nx; ++px) {
            const int ox = xs[static_cast<std::size_t>(px)];
            const int oy = ys[static_cast<std::size_t>(py)];
            float u0 = 0.0f;
            float v0 = 0.0f;
            flow.sample(static_cast<float>(ox) + half, static_cast<float>(oy) + half, u0, v0);
            const auto k = static_cast<std::size_t>(py) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(px);
            searchPatch(i0, gx, gy, i1, ox, oy, p, o.iterations, u0, v0, pu[k], pv[k]);
        }
    });
    // Densify: every pixel averages the patches covering it, weighted by how well each fits it there.
    const std::vector<std::pair<int, int>> byX = covering(i0.width, xs, p);
    const std::vector<std::pair<int, int>> byY = covering(i0.height, ys, p);
    FlowField dense(i0.width, i0.height);
    parallelFor(i0.height, o.threads, [&](int y) {
        const auto [y0, y1] = byY[static_cast<std::size_t>(y)];
        for (int x = 0; x < i0.width; ++x) {
            const auto [x0, x1] = byX[static_cast<std::size_t>(x)];
            const float here = i0(x, y);
            double su = 0.0;
            double sv = 0.0;
            double sw = 0.0;
            for (int py = y0; py < y1; ++py) {
                for (int px = x0; px < x1; ++px) {
                    const auto k = static_cast<std::size_t>(py) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(px);
                    const float u = pu[k];
                    const float v = pv[k];
                    const float d = std::abs(i1.sample(static_cast<float>(x) + u, static_cast<float>(y) + v) - here) * 255.0f;
                    const double w = 1.0 / std::max(1.0f, d);
                    su += w * u;
                    sv += w * v;
                    sw += w;
                }
            }
            if (sw > 0.0) {
                dense.u(x, y) = static_cast<float>(su / sw);
                dense.v(x, y) = static_cast<float>(sv / sw);
            } else {
                dense.u(x, y) = flow.u(x, y);
                dense.v(x, y) = flow.v(x, y);
            }
        }
    });
    for (int pass = 0; pass < o.medianPasses; ++pass) {
        dense = median3(dense, o.threads);
    }
    flow = std::move(dense);
}

} // namespace

FlowField estimateFlow(const Plane& from, const Plane& to, const FlowOptions& options) {
    FlowField flow = estimateFlowAtFinestLevel(from, to, options);
    if (!flow.empty() && (flow.width != from.width || flow.height != from.height)) {
        flow = flow.resized(from.width, from.height);
    }
    return flow;
}

FlowOptions analysisFlowOptions() {
    FlowOptions o;
    o.patchSize = 8;
    o.patchStride = 4;
    o.iterations = 12;
    o.finestLevel = 1;
    o.medianPasses = 1;
    return o;
}

FlowField estimateFlowAtFinestLevel(const Plane& from, const Plane& to, const FlowOptions& options) {
    if (from.empty() || from.width != to.width || from.height != to.height ||
        from.px.size() != static_cast<std::size_t>(from.width) * static_cast<std::size_t>(from.height) ||
        to.px.size() != from.px.size()) {
        return {};
    }
    int levels = 1;
    for (int w = from.width, h = from.height; levels < 12;) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        if (std::min(w, h) < std::max(options.coarsestSide, 4)) {
            break;
        }
        ++levels;
    }
    const std::vector<Plane> a = pyramid(from, levels);
    const std::vector<Plane> b = pyramid(to, levels);
    const int coarsest = static_cast<int>(std::min(a.size(), b.size())) - 1;
    const int finest = std::clamp(options.finestLevel, 0, coarsest);
    FlowField flow(a[static_cast<std::size_t>(coarsest)].width, a[static_cast<std::size_t>(coarsest)].height);
    for (int l = coarsest; l >= finest; --l) {
        const Plane& i0 = a[static_cast<std::size_t>(l)];
        const Plane& i1 = b[static_cast<std::size_t>(l)];
        if (flow.width != i0.width || flow.height != i0.height) {
            flow = flow.resized(i0.width, i0.height);
        }
        disLevel(i0, i1, flow, options);
    }
    return flow;
}

FlowPair estimateFlowPair(const Plane& a, const Plane& b, const FlowOptions& options) {
    FlowPair out;
    const FlowField forward = estimateFlowAtFinestLevel(a, b, options);
    const FlowField backward = estimateFlowAtFinestLevel(b, a, options);
    if (forward.empty() || backward.empty()) {
        return out;
    }
    out.width = forward.width;
    out.height = forward.height;
    out.data.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 4);
    const float kx = 1.0f / static_cast<float>(out.width);
    const float ky = 1.0f / static_cast<float>(out.height);
    for (std::size_t i = 0, n = static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height); i < n; ++i) {
        out.data[4 * i + 0] = forward.uv[2 * i] * kx;
        out.data[4 * i + 1] = forward.uv[2 * i + 1] * ky;
        out.data[4 * i + 2] = backward.uv[2 * i] * kx;
        out.data[4 * i + 3] = backward.uv[2 * i + 1] * ky;
    }
    return out;
}

double meanConsistencyError(const FlowPair& pair) {
    if (pair.empty()) {
        return 0.0;
    }
    const auto at = [&](float x, float y, int component) {
        x = std::clamp(x, 0.0f, static_cast<float>(pair.width - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(pair.height - 1));
        const int xi = static_cast<int>(std::lround(x));
        const int yi = static_cast<int>(std::lround(y));
        return pair.data[4 * (static_cast<std::size_t>(yi) * static_cast<std::size_t>(pair.width) + static_cast<std::size_t>(xi)) +
                         static_cast<std::size_t>(component)];
    };
    double sum = 0.0;
    for (int y = 0; y < pair.height; ++y) {
        for (int x = 0; x < pair.width; ++x) {
            const float fu = at(static_cast<float>(x), static_cast<float>(y), 0) * static_cast<float>(pair.width);
            const float fv = at(static_cast<float>(x), static_cast<float>(y), 1) * static_cast<float>(pair.height);
            const float bu = at(static_cast<float>(x) + fu, static_cast<float>(y) + fv, 2) * static_cast<float>(pair.width);
            const float bv = at(static_cast<float>(x) + fu, static_cast<float>(y) + fv, 3) * static_cast<float>(pair.height);
            sum += std::hypot(fu + bu, fv + bv);
        }
    }
    return sum / (static_cast<double>(pair.width) * pair.height);
}

} // namespace wabcut::motion
