// Homographies for the stabilizer (C.15): the algebra, the exact 4-point solution (a corner pin) and the robust fit
// (RANSAC over 4-point samples, then the normalised DLT on the inliers, after Hartley & Zisserman).

#include <wabcut/ai/stabilize/Stabilize.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <random>

namespace wabcut::ai::stabilize {

using masks::Point2;

namespace {

using Matrix3 = std::array<double, 9>;

constexpr std::size_t kMinHomographyInliers = 8;

Matrix3 multiply(const Matrix3& a, const Matrix3& b) {
    Matrix3 r{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
        }
    }
    return r;
}

/// Scaled so that m[8] = 1 (when m[8] is not vanishingly small next to the rest).
Homography normalised(const Matrix3& m) {
    Homography h;
    h.m = m;
    double largest = 0.0;
    for (const double v : m) {
        largest = std::max(largest, std::abs(v));
    }
    if (std::isfinite(m[8]) && m[8] != 0.0 && std::abs(m[8]) > 1e-12 * largest) {
        const double k = 1.0 / m[8];
        for (double& v : h.m) {
            v *= k;
        }
        h.m[8] = 1.0;
    }
    return h;
}

bool finite(const Matrix3& m) {
    return std::all_of(m.begin(), m.end(), [](double v) { return std::isfinite(v); });
}

/// Hartley's conditioning: the points' centroid to the origin, their mean distance from it to sqrt(2).
Matrix3 conditioning(std::span<const Point2> points, std::span<const std::size_t> use) {
    double mx = 0.0;
    double my = 0.0;
    for (const std::size_t i : use) {
        mx += points[i].x;
        my += points[i].y;
    }
    const double n = static_cast<double>(std::max<std::size_t>(use.size(), 1));
    mx /= n;
    my /= n;
    double distance = 0.0;
    for (const std::size_t i : use) {
        distance += std::hypot(points[i].x - mx, points[i].y - my);
    }
    distance /= n;
    const double s = distance > 1e-12 ? std::numbers::sqrt2 / distance : 1.0;
    return {s, 0.0, -s * mx, 0.0, s, -s * my, 0.0, 0.0, 1.0};
}

Point2 applyAffine(const Matrix3& m, Point2 p) { return {m[0] * p.x + m[1] * p.y + m[2], m[3] * p.x + m[4] * p.y + m[5]}; }

/// No three of the four points (nearly) on one line: a homography through them is then well defined.
bool spread(const std::array<Point2, 4>& p, double minArea) {
    constexpr std::array<std::array<std::size_t, 3>, 4> kTriples{{{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}}};
    for (const auto& t : kTriples) {
        const Point2 a = p[t[0]];
        const Point2 b = p[t[1]];
        const Point2 c = p[t[2]];
        const double cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (!(std::abs(cross) > minArea)) {
            return false;
        }
    }
    return true;
}

/// The homography (h8 = 1) taking a[k] to b[k] exactly, by Gaussian elimination with partial pivoting. Points should
/// be conditioned (coordinates of order 1). Nullopt when the system is singular.
std::optional<Matrix3> fourPoint(const std::array<Point2, 4>& a, const std::array<Point2, 4>& b) {
    std::array<std::array<double, 9>, 8> s{};
    for (std::size_t k = 0; k < 4; ++k) {
        const double x = a[k].x, y = a[k].y, u = b[k].x, v = b[k].y;
        s[2 * k] = {x, y, 1.0, 0.0, 0.0, 0.0, -x * u, -y * u, u};
        s[2 * k + 1] = {0.0, 0.0, 0.0, x, y, 1.0, -x * v, -y * v, v};
    }
    for (std::size_t col = 0; col < 8; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < 8; ++r) {
            if (std::abs(s[r][col]) > std::abs(s[pivot][col])) {
                pivot = r;
            }
        }
        if (!(std::abs(s[pivot][col]) > 1e-12)) {
            return std::nullopt;
        }
        std::swap(s[col], s[pivot]);
        for (std::size_t r = col + 1; r < 8; ++r) {
            const double f = s[r][col] / s[col][col];
            if (f == 0.0) {
                continue;
            }
            for (std::size_t c = col; c < 9; ++c) {
                s[r][c] -= f * s[col][c];
            }
        }
    }
    Matrix3 h{};
    h[8] = 1.0;
    for (std::size_t r = 8; r-- > 0;) {
        double sum = s[r][8];
        for (std::size_t c = r + 1; c < 8; ++c) {
            sum -= s[r][c] * h[c];
        }
        h[r] = sum / s[r][r];
    }
    if (!finite(h)) {
        return std::nullopt;
    }
    return h;
}

/// Eigenvector of the smallest eigenvalue of a symmetric 9x9 matrix (cyclic Jacobi rotations).
std::array<double, 9> smallestEigenvector(std::array<double, 81> a) {
    constexpr std::size_t n = 9;
    std::array<double, 81> v{};
    for (std::size_t i = 0; i < n; ++i) {
        v[i * n + i] = 1.0;
    }
    double total = 0.0;
    for (const double x : a) {
        total += x * x;
    }
    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = 0.0;
        for (std::size_t p = 0; p < n; ++p) {
            for (std::size_t q = p + 1; q < n; ++q) {
                off += a[p * n + q] * a[p * n + q];
            }
        }
        if (!(off > 1e-30 * total)) {
            break;
        }
        for (std::size_t p = 0; p < n; ++p) {
            for (std::size_t q = p + 1; q < n; ++q) {
                const double apq = a[p * n + q];
                if (std::abs(apq) < 1e-300) {
                    continue;
                }
                const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (std::size_t k = 0; k < n; ++k) { // columns p, q
                    const double akp = a[k * n + p];
                    const double akq = a[k * n + q];
                    a[k * n + p] = c * akp - s * akq;
                    a[k * n + q] = s * akp + c * akq;
                }
                for (std::size_t k = 0; k < n; ++k) { // rows p, q
                    const double apk = a[p * n + k];
                    const double aqk = a[q * n + k];
                    a[p * n + k] = c * apk - s * aqk;
                    a[q * n + k] = s * apk + c * aqk;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const double vkp = v[k * n + p];
                    const double vkq = v[k * n + q];
                    v[k * n + p] = c * vkp - s * vkq;
                    v[k * n + q] = s * vkp + c * vkq;
                }
            }
        }
    }
    std::size_t smallest = 0;
    for (std::size_t i = 1; i < n; ++i) {
        if (a[i * n + i] < a[smallest * n + smallest]) {
            smallest = i;
        }
    }
    std::array<double, 9> out{};
    for (std::size_t k = 0; k < n; ++k) {
        out[k] = v[k * n + smallest];
    }
    return out;
}

/// The direct linear transform on conditioned points: the h (|h| = 1) minimising the algebraic error.
std::optional<Matrix3> dlt(std::span<const Point2> a, std::span<const Point2> b, std::span<const std::size_t> use) {
    if (use.size() < 4) {
        return std::nullopt;
    }
    std::array<double, 81> m{};
    const auto accumulate = [&m](const std::array<double, 9>& row) {
        for (std::size_t i = 0; i < 9; ++i) {
            if (row[i] == 0.0) {
                continue;
            }
            for (std::size_t j = 0; j < 9; ++j) {
                m[i * 9 + j] += row[i] * row[j];
            }
        }
    };
    for (const std::size_t i : use) {
        const double x = a[i].x, y = a[i].y, u = b[i].x, v = b[i].y;
        accumulate({x, y, 1.0, 0.0, 0.0, 0.0, -u * x, -u * y, -u});
        accumulate({0.0, 0.0, 0.0, x, y, 1.0, -v * x, -v * y, -v});
    }
    const Matrix3 h = smallestEigenvector(m);
    if (!finite(h)) {
        return std::nullopt;
    }
    return h;
}

} // namespace

// ---- Homography -------------------------------------------------------------------------------------------------

Point2 Homography::map(Point2 p) const {
    const double w = m[6] * p.x + m[7] * p.y + m[8];
    return {(m[0] * p.x + m[1] * p.y + m[2]) / w, (m[3] * p.x + m[4] * p.y + m[5]) / w};
}

Homography Homography::after(const Homography& first) const { return normalised(multiply(m, first.m)); }

std::optional<Homography> Homography::inverted() const {
    const Matrix3& a = m;
    const double c00 = a[4] * a[8] - a[5] * a[7];
    const double c01 = a[5] * a[6] - a[3] * a[8];
    const double c02 = a[3] * a[7] - a[4] * a[6];
    const double det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    double largest = 0.0;
    for (const double v : a) {
        largest = std::max(largest, std::abs(v));
    }
    if (!std::isfinite(det) || !std::isfinite(largest) || !(std::abs(det) > 1e-12 * largest * largest * largest)) {
        return std::nullopt;
    }
    const Matrix3 inverse{c00 / det,
                          (a[2] * a[7] - a[1] * a[8]) / det,
                          (a[1] * a[5] - a[2] * a[4]) / det,
                          c01 / det,
                          (a[0] * a[8] - a[2] * a[6]) / det,
                          (a[2] * a[3] - a[0] * a[5]) / det,
                          c02 / det,
                          (a[1] * a[6] - a[0] * a[7]) / det,
                          (a[0] * a[4] - a[1] * a[3]) / det};
    if (!finite(inverse)) {
        return std::nullopt;
    }
    return normalised(inverse);
}

Homography Homography::fromMotion(const masks::Motion& motion) {
    Homography h;
    h.m = {motion.a, motion.b, motion.c, motion.d, motion.e, motion.f, 0.0, 0.0, 1.0};
    return h;
}

std::optional<Homography> homographyFromCorners(const std::array<Point2, 4>& from, const std::array<Point2, 4>& to) {
    constexpr std::array<std::size_t, 4> kAll{0, 1, 2, 3};
    for (std::size_t k = 0; k < 4; ++k) {
        if (!std::isfinite(from[k].x) || !std::isfinite(from[k].y) || !std::isfinite(to[k].x) || !std::isfinite(to[k].y)) {
            return std::nullopt;
        }
    }
    const Matrix3 ta = conditioning(from, kAll);
    const Matrix3 tb = conditioning(to, kAll);
    std::array<Point2, 4> a{};
    std::array<Point2, 4> b{};
    for (std::size_t k = 0; k < 4; ++k) {
        a[k] = applyAffine(ta, from[k]);
        b[k] = applyAffine(tb, to[k]);
    }
    if (!spread(a, 1e-9) || !spread(b, 1e-9)) {
        return std::nullopt;
    }
    const auto hn = fourPoint(a, b);
    const auto tbInverse = normalised(tb).inverted();
    if (!hn || !tbInverse) {
        return std::nullopt;
    }
    const Homography h = normalised(multiply(tbInverse->m, multiply(*hn, ta)));
    if (!finite(h.m)) {
        return std::nullopt;
    }
    return h;
}

// ---- The robust fit ---------------------------------------------------------------------------------------------

std::optional<Homography> fitHomography(std::span<const Point2> a, std::span<const Point2> b, double threshold, int iterations,
                                        std::vector<char>* inliers) {
    const std::size_t n = std::min(a.size(), b.size());
    if (inliers) {
        inliers->assign(n, 0);
    }
    std::vector<std::size_t> valid;
    valid.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isfinite(a[i].x) && std::isfinite(a[i].y) && std::isfinite(b[i].x) && std::isfinite(b[i].y)) {
            valid.push_back(i);
        }
    }
    if (valid.size() < kMinHomographyInliers || !(threshold > 0.0)) {
        return std::nullopt;
    }
    // Conditioned copies for the solvers; residuals are measured in pixels.
    const Matrix3 ta = conditioning(a, valid);
    const Matrix3 tb = conditioning(b, valid);
    const auto tbInverse = normalised(tb).inverted();
    if (!tbInverse) {
        return std::nullopt;
    }
    std::vector<Point2> an(n);
    std::vector<Point2> bn(n);
    for (const std::size_t i : valid) {
        an[i] = applyAffine(ta, a[i]);
        bn[i] = applyAffine(tb, b[i]);
    }
    const auto pixels = [&](const Matrix3& hn) { return normalised(multiply(tbInverse->m, multiply(hn, ta))); };
    const auto residual = [&](const Homography& h, std::size_t i) {
        const Point2 p = a[i];
        const double w = h.m[6] * p.x + h.m[7] * p.y + h.m[8];
        if (!(w > 0.0)) { // behind the camera: never an inlier
            return std::numeric_limits<double>::infinity();
        }
        const double x = (h.m[0] * p.x + h.m[1] * p.y + h.m[2]) / w;
        const double y = (h.m[3] * p.x + h.m[4] * p.y + h.m[5]) / w;
        return std::hypot(x - b[i].x, y - b[i].y);
    };
    const auto inliersOf = [&](const Homography& h, std::vector<std::size_t>& out, double* error) {
        out.clear();
        double sum = 0.0;
        for (const std::size_t i : valid) {
            const double e = residual(h, i);
            if (e < threshold) {
                out.push_back(i);
                sum += e;
            }
        }
        if (error) {
            *error = sum;
        }
    };

    std::mt19937 rng(0x5eed1e55u);
    std::uniform_int_distribution<std::size_t> pick(0, valid.size() - 1);
    std::vector<std::size_t> best;
    double bestError = std::numeric_limits<double>::max();
    std::vector<std::size_t> current;
    std::array<std::size_t, 4> sample{};
    const int rounds = std::max(iterations, 1);
    for (int r = 0; r < rounds; ++r) {
        for (std::size_t s = 0; s < 4; ++s) {
            std::size_t i = 0;
            do {
                i = valid[pick(rng)];
            } while (std::find(sample.begin(), sample.begin() + static_cast<std::ptrdiff_t>(s), i) != sample.begin() + static_cast<std::ptrdiff_t>(s));
            sample[s] = i;
        }
        std::array<Point2, 4> sa{};
        std::array<Point2, 4> sb{};
        for (std::size_t s = 0; s < 4; ++s) {
            sa[s] = an[sample[s]];
            sb[s] = bn[sample[s]];
        }
        if (!spread(sa, 1e-4) || !spread(sb, 1e-4)) {
            continue;
        }
        const auto hn = fourPoint(sa, sb);
        if (!hn) {
            continue;
        }
        double error = 0.0;
        inliersOf(pixels(*hn), current, &error);
        if (current.size() > best.size() || (current.size() == best.size() && error < bestError)) {
            best = current;
            bestError = error;
        }
    }
    if (best.size() < kMinHomographyInliers) {
        return std::nullopt;
    }
    // Least squares (the normalised DLT) on the inliers, re-collecting them after each refit.
    std::optional<Homography> fit;
    for (int pass = 0; pass < 3; ++pass) {
        const auto hn = dlt(an, bn, best);
        if (!hn) {
            break;
        }
        const Homography h = pixels(*hn);
        std::vector<std::size_t> refined;
        inliersOf(h, refined, nullptr);
        if (refined.size() < kMinHomographyInliers) {
            break;
        }
        fit = h;
        const bool same = refined == best;
        best = std::move(refined);
        if (same) {
            break;
        }
    }
    if (!fit || !finite(fit->m)) {
        return std::nullopt;
    }
    if (inliers) {
        for (const std::size_t i : best) {
            (*inliers)[i] = 1;
        }
    }
    return fit;
}

} // namespace wabcut::ai::stabilize
