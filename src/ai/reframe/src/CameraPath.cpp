#include <wabcut/ai/reframe/CameraPath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace wabcut::ai::reframe {

namespace {

constexpr int kBand = 3;        // D3'D3 couples samples three apart
constexpr double kAlpha = 1.7;  // ADMM over-relaxation

/// A symmetric positive definite band matrix (bandwidth kBand), stored by rows: m[i][k] = M(i, i + k).
struct BandMatrix {
    explicit BandMatrix(std::size_t n) : rows(n) {}
    std::vector<std::array<double, kBand + 1>> rows;

    void add(std::size_t i, std::size_t j, double v) {
        if (j < i) {
            std::swap(i, j);
        }
        rows[i][j - i] += v;
    }
};

/// Cholesky factor of a band matrix: L(i, i - k) = l[i][k].
struct BandCholesky {
    std::vector<std::array<double, kBand + 1>> l;

    explicit BandCholesky(const BandMatrix& m) : l(m.rows.size()) {
        const std::size_t n = m.rows.size();
        for (std::size_t i = 0; i < n; ++i) {
            for (int k = kBand; k >= 0; --k) {
                if (static_cast<std::size_t>(k) > i) {
                    continue;
                }
                const std::size_t j = i - static_cast<std::size_t>(k);
                double sum = m.rows[j][static_cast<std::size_t>(k)]; // M(j, i) = M(i, j)
                for (int p = 1; p <= kBand; ++p) {                  // sum over L(i, j - p) L(j, j - p)
                    const std::size_t kk = static_cast<std::size_t>(k + p);
                    if (kk > kBand || static_cast<std::size_t>(p) > j) {
                        continue;
                    }
                    sum -= l[i][kk] * l[j][static_cast<std::size_t>(p)];
                }
                if (k == 0) {
                    l[i][0] = std::sqrt(std::max(sum, 1e-300));
                } else {
                    l[i][static_cast<std::size_t>(k)] = sum / l[j][0];
                }
            }
        }
    }

    /// Solves L L' x = b in place.
    void solve(std::vector<double>& b) const {
        const std::size_t n = l.size();
        for (std::size_t i = 0; i < n; ++i) {
            double sum = b[i];
            for (std::size_t k = 1; k <= kBand && k <= i; ++k) {
                sum -= l[i][k] * b[i - k];
            }
            b[i] = sum / l[i][0];
        }
        for (std::size_t i = n; i-- > 0;) {
            double sum = b[i];
            for (std::size_t k = 1; k <= kBand && i + k < n; ++k) {
                sum -= l[i + k][k] * b[i + k];
            }
            b[i] = sum / l[i][0];
        }
    }
};

/// Finite-difference stencils of order 1..3.
constexpr std::array<std::array<double, 4>, 3> kStencils{{{-1, 1, 0, 0}, {1, -2, 1, 0}, {-1, 3, -3, 1}}};

double softThreshold(double v, double t) { return v > t ? v - t : v < -t ? v + t : 0.0; }

} // namespace

PathWeights weightsForSmoothness(double smoothness) {
    const double k = std::pow(10.0, 2.0 * std::clamp(smoothness, 0.0, 1.0) - 1.0); // 0.1 .. 10, 1 at 0.5
    PathWeights w;
    w.velocity *= k;
    w.acceleration *= k;
    w.jerk *= k;
    return w;
}

std::vector<double> solveCameraPath(const PathProblem& problem, const PathWeights& weights, const PathSolveOptions& options) {
    const std::size_t n = problem.desired.size();
    std::vector<double> lower = problem.lower;
    std::vector<double> upper = problem.upper;
    lower.resize(n, -std::numeric_limits<double>::infinity());
    upper.resize(n, std::numeric_limits<double>::infinity());
    for (std::size_t i = 0; i < n; ++i) {
        if (lower[i] > upper[i]) {
            lower[i] = upper[i] = 0.5 * (lower[i] + upper[i]);
        }
    }
    const auto weightOf = [&](std::size_t i) {
        const double w = problem.weight.empty() ? 1.0 : problem.weight[i];
        return std::isfinite(problem.desired[i]) && std::isfinite(w) ? std::max(w, 0.0) : 0.0;
    };
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double start = std::isfinite(problem.desired[i]) ? problem.desired[i] : 0.5 * (lower[i] + upper[i]);
        x[i] = std::isfinite(start) ? std::clamp(start, lower[i], upper[i]) : 0.0;
    }
    if (n < 2) {
        return x;
    }
    const double dt = problem.dt > 0.0 ? problem.dt : 0.1;
    // Penalties of the three difference blocks, in the units of their (unscaled) differences.
    const std::array<double, 3> lambda{weights.velocity, weights.acceleration / dt, weights.jerk / (dt * dt)};
    // ADMM's steps: each block's threshold (lambda / rho) set near the size its values take (a per-sample move of
    // about 1% of the picture, smaller for the higher differences), so the duals need not grow for long before the
    // path may move: this converges in a few hundred iterations where uniform steps need thousands.
    const std::array<double, 4> rho{std::max(100.0 * dt, 1e-3), std::max(lambda[0] / 0.01, 1e-3), std::max(lambda[1] / 0.002, 1e-3),
                                    std::max(lambda[2] / 0.0005, 1e-3)}; // data, D1, D2, D3
    const auto factorFor = [&] {
        BandMatrix m(n);
        for (std::size_t i = 0; i < n; ++i) {
            m.add(i, i, rho[0]);
        }
        for (std::size_t order = 1; order <= 3; ++order) {
            if (n <= order) {
                continue;
            }
            const auto& c = kStencils[order - 1];
            for (std::size_t r = 0; r + order < n; ++r) {
                for (std::size_t a = 0; a <= order; ++a) {
                    for (std::size_t b = a; b <= order; ++b) {
                        m.add(r + a, r + b, rho[order] * c[a] * c[b]);
                    }
                }
            }
        }
        return BandCholesky(m);
    };
    const BandCholesky factor = factorFor();

    std::array<std::vector<double>, 4> z;
    std::array<std::vector<double>, 4> u;
    z[0] = x;
    u[0].assign(n, 0.0);
    for (std::size_t order = 1; order <= 3; ++order) {
        const std::size_t rows = n > order ? n - order : 0;
        z[order].assign(rows, 0.0);
        u[order].assign(rows, 0.0);
    }
    const auto difference = [&](std::size_t order, const std::vector<double>& v, std::size_t r) {
        const auto& c = kStencils[order - 1];
        double sum = 0.0;
        for (std::size_t a = 0; a <= order; ++a) {
            sum += c[a] * v[r + a];
        }
        return sum;
    };

    std::vector<double> rhs(n);
    for (int iteration = 0; iteration < options.maxIterations; ++iteration) {
        // x = argmin sum_k rho_k/2 |A_k x - z_k + u_k|^2
        for (std::size_t i = 0; i < n; ++i) {
            rhs[i] = rho[0] * (z[0][i] - u[0][i]);
        }
        for (std::size_t order = 1; order <= 3; ++order) {
            const auto& c = kStencils[order - 1];
            for (std::size_t r = 0; r < z[order].size(); ++r) {
                const double v = rho[order] * (z[order][r] - u[order][r]);
                for (std::size_t a = 0; a <= order; ++a) {
                    rhs[r + a] += c[a] * v;
                }
            }
        }
        factor.solve(rhs);
        x = rhs;

        // z_0: the data term and the bounds; z_k: soft-thresholded differences. Over-relaxed (alpha) for speed.
        double primal = 0.0; // largest |A x - z|
        double dual = 0.0;   // largest rho |z - z_previous|
        for (std::size_t i = 0; i < n; ++i) {
            const double previous = z[0][i];
            const double relaxed = kAlpha * x[i] + (1.0 - kAlpha) * previous;
            const double v = relaxed + u[0][i];
            const double w = weightOf(i) * dt / rho[0];
            const double d = problem.desired[i];
            const double free = w > 0.0 ? d + softThreshold(v - d, w) : v;
            z[0][i] = std::clamp(free, lower[i], upper[i]);
            u[0][i] += relaxed - z[0][i];
            primal = std::max(primal, std::abs(x[i] - z[0][i]));
            dual = std::max(dual, rho[0] * std::abs(z[0][i] - previous));
        }
        for (std::size_t order = 1; order <= 3; ++order) {
            for (std::size_t r = 0; r < z[order].size(); ++r) {
                const double previous = z[order][r];
                const double dx = difference(order, x, r);
                const double relaxed = kAlpha * dx + (1.0 - kAlpha) * previous;
                z[order][r] = softThreshold(relaxed + u[order][r], lambda[order - 1] / rho[order]);
                u[order][r] += relaxed - z[order][r];
                primal = std::max(primal, std::abs(dx - z[order][r]));
                dual = std::max(dual, rho[order] * std::abs(z[order][r] - previous));
            }
        }
        if (iteration > 20 && primal < options.tolerance && dual < options.tolerance) {
            break;
        }
    }
    // z_0 is feasible by construction (x only converges to it).
    return z[0];
}

// --- Keyframes ------------------------------------------------------------------------------------------------------

double maxCurveError(const Curve& curve, std::span<const Ticks> times, std::span<const double> values) {
    double worst = 0.0;
    for (std::size_t i = 0; i < times.size() && i < values.size(); ++i) {
        worst = std::max(worst, std::abs(curve.valueAt(times[i]) - values[i]));
    }
    return worst;
}

Curve fitCurve(std::span<const Ticks> times, std::span<const double> values, double tolerance, std::span<const double> lower,
               std::span<const double> upper, bool holdLast) {
    Curve curve;
    const std::size_t n = std::min(times.size(), values.size());
    if (n == 0) {
        curve.keys.push_back(Keyframe{Ticks{}, 0.0, Interpolation::Linear, {}, {}});
        return curve;
    }
    const auto [lo, hi] = std::minmax_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(n));
    const bool bounded = lower.size() >= n && upper.size() >= n;
    if (*hi - *lo <= tolerance) { // still: one key, no motion at all
        double value = 0.5 * (*lo + *hi);
        if (bounded) {
            for (std::size_t i = 0; i < n; ++i) {
                value = std::clamp(value, lower[i], upper[i]);
            }
        }
        curve.keys.push_back(Keyframe{times[0], value, holdLast ? Interpolation::Hold : Interpolation::Linear, {}, {}});
        return curve;
    }

    const auto seconds = [&](std::size_t a, std::size_t b) { return (times[b] - times[a]).toSeconds(); };
    // Tangents of the sampled path: arriving from the previous sample, leaving to the next.
    const auto slopeIn = [&](std::size_t i) { return i == 0 ? (values[1] - values[0]) / seconds(0, 1) : (values[i] - values[i - 1]) / seconds(i - 1, i); };
    const auto slopeOut = [&](std::size_t i) {
        return i + 1 >= n ? (values[i] - values[i - 1]) / seconds(i - 1, i) : (values[i + 1] - values[i]) / seconds(i, i + 1);
    };
    std::vector<std::size_t> chosen{0, n - 1};
    const auto build = [&] {
        Curve c;
        for (const std::size_t i : chosen) {
            Keyframe k;
            k.time = times[i];
            k.value = values[i];
            k.interpolation = Interpolation::Bezier;
            k.in = BezierHandle{slopeIn(i), 1.0 / 3.0};
            k.out = BezierHandle{slopeOut(i), 1.0 / 3.0};
            c.keys.push_back(k);
        }
        if (holdLast) {
            c.keys.back().interpolation = Interpolation::Hold;
        }
        return c;
    };
    for (;;) {
        curve = build();
        // The worst sample: off by more than the tolerance, or outside its bounds.
        std::size_t worst = n;
        double worstScore = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double v = curve.valueAt(times[i]);
            double score = std::abs(v - values[i]) - tolerance;
            if (bounded) {
                score = std::max(score, (std::max(lower[i] - v, v - upper[i]) - 1e-9) * 1e6);
            }
            if (score > worstScore && !std::ranges::binary_search(chosen, i)) {
                worstScore = score;
                worst = i;
            }
        }
        if (worst == n) {
            return curve;
        }
        chosen.insert(std::ranges::upper_bound(chosen, worst), worst);
    }
}

} // namespace wabcut::ai::reframe
