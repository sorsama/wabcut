// The virtual camera of auto reframe (AI-9) on synthetic trajectories: a still subject gives zero motion, the path
// always keeps the subject inside the safe region, smoothness bounds the travel, moves are steady (still, then
// constant velocity), and the keyframes fitted to a path are sparse and stay within tolerance and bounds.

#include <wabcut/ai/reframe/CameraPath.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numeric>
#include <random>

using namespace wabcut;
using namespace wabcut::ai::reframe;
using Catch::Approx;

namespace {

double travel(const std::vector<double>& x) {
    double sum = 0.0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        sum += std::abs(x[i] - x[i - 1]);
    }
    return sum;
}

double largestStep(const std::vector<double>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    double worst = 0.0;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, x.size()); ++i) {
        worst = std::max(worst, std::abs(x[i] - x[i - 1]));
    }
    return worst;
}

PathProblem problemOf(std::vector<double> desired, double halfWindow = 0.15, double subjectHalf = 0.04) {
    PathProblem p;
    p.dt = 0.1;
    p.desired = std::move(desired);
    for (const double d : p.desired) {
        // The subject [d - subjectHalf, d + subjectHalf] inside a window of half-width halfWindow, within [0, 1].
        p.lower.push_back(std::max(halfWindow, d + subjectHalf - halfWindow));
        p.upper.push_back(std::min(1.0 - halfWindow, d - subjectHalf + halfWindow));
    }
    return p;
}

std::vector<Ticks> grid(std::size_t n, double dt = 0.1) {
    std::vector<Ticks> t;
    for (std::size_t i = 0; i < n; ++i) {
        t.push_back(Ticks{static_cast<std::int64_t>(std::llround(static_cast<double>(i) * dt * static_cast<double>(kTicksPerSecond)))});
    }
    return t;
}

} // namespace

TEST_CASE("A still subject gives a still camera", "[ai][reframe][path]") {
    const auto path = solveCameraPath(problemOf(std::vector<double>(80, 0.42)), PathWeights{});
    REQUIRE(path.size() == 80);
    CHECK(largestStep(path) < 1e-5);
    CHECK(path[40] == Approx(0.42).margin(1e-4));

    // Keyframes of a still path: one key, no motion at all.
    const Curve curve = fitCurve(grid(80), path, 0.5 / 1080.0);
    CHECK(curve.keys.size() == 1);

    // Degenerate sizes.
    CHECK(solveCameraPath(PathProblem{}, PathWeights{}).empty());
    PathProblem one = problemOf({0.9});
    CHECK(solveCameraPath(one, PathWeights{})[0] == Approx(one.upper[0]));
}

TEST_CASE("The subject always stays inside the safe region", "[ai][reframe][path]") {
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 0.03);
    std::vector<double> desired;
    for (int i = 0; i < 300; ++i) { // a wandering subject with detector jitter
        desired.push_back(std::clamp(0.5 + 0.3 * std::sin(i * 0.03) + noise(rng), 0.05, 0.95));
    }
    const PathProblem p = problemOf(desired);
    for (const double smoothness : {0.0, 0.5, 1.0}) {
        INFO(smoothness);
        const auto path = solveCameraPath(p, weightsForSmoothness(smoothness));
        for (std::size_t i = 0; i < path.size(); ++i) {
            REQUIRE(path[i] >= p.lower[i] - 1e-12);
            REQUIRE(path[i] <= p.upper[i] + 1e-12);
        }
        // Keyframes too: the fitted curve keeps every sample inside its bounds.
        const auto times = grid(path.size());
        const Curve curve = fitCurve(times, path, 2.0 / 1080.0, p.lower, p.upper);
        CHECK(curve.isWellFormed());
        CHECK(curve.keys.size() < path.size() / 3);
        CHECK(maxCurveError(curve, times, path) <= 2.0 / 1080.0 + 1e-9);
        for (std::size_t i = 0; i < path.size(); ++i) {
            const double v = curve.valueAt(times[i]);
            REQUIRE(v >= p.lower[i] - 1e-9);
            REQUIRE(v <= p.upper[i] + 1e-9);
        }
    }
}

TEST_CASE("Smoothness bounds the camera's travel; jitter is ignored", "[ai][reframe][path]") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> jitter(-0.02, 0.02);
    std::vector<double> desired;
    for (int i = 0; i < 200; ++i) {
        desired.push_back(0.5 + jitter(rng)); // a still subject seen through a noisy detector
    }
    PathProblem p;
    p.dt = 0.1;
    p.desired = desired;
    p.lower.assign(desired.size(), 0.2);
    p.upper.assign(desired.size(), 0.8);
    const double noisy = travel(desired);
    double previous = std::numeric_limits<double>::infinity();
    for (const double smoothness : {0.0, 0.5, 1.0}) {
        INFO(smoothness);
        const double moved = travel(solveCameraPath(p, weightsForSmoothness(smoothness)));
        CHECK(moved <= previous + 1e-6); // smoother never travels further
        previous = moved;
    }
    CHECK(travel(solveCameraPath(p, weightsForSmoothness(0.5))) < 0.05 * noisy);
    CHECK(travel(solveCameraPath(p, weightsForSmoothness(1.0))) < 1e-3);
}

TEST_CASE("A move is still, then steady, then still", "[ai][reframe][path]") {
    // The subject walks from 0.3 to 0.7 between 4 s and 8 s of a 12 s shot.
    std::vector<double> desired;
    for (int i = 0; i < 120; ++i) {
        const double t = i * 0.1;
        desired.push_back(t < 4.0 ? 0.3 : t > 8.0 ? 0.7 : 0.3 + 0.1 * (t - 4.0));
    }
    const auto path = solveCameraPath(problemOf(desired), PathWeights{});
    CHECK(largestStep(path, 0, 30) < 1e-4);   // still before
    CHECK(largestStep(path, 95, 120) < 1e-4); // still after
    CHECK(path[10] == Approx(0.3).margin(0.03));
    CHECK(path[115] == Approx(0.7).margin(0.03));
    // Mid-move the speed is steady (the second difference is about zero).
    double bend = 0.0;
    for (std::size_t i = 52; i < 68; ++i) {
        bend = std::max(bend, std::abs(path[i + 1] - 2.0 * path[i] + path[i - 1]));
    }
    CHECK(bend < 2e-3);
    // Few keyframes describe it (eases in and out, the pan): far fewer than the 120 samples.
    const Curve curve = fitCurve(grid(path.size()), path, 1.0 / 1080.0);
    CHECK(curve.keys.size() <= 14);
}

TEST_CASE("Fitted keyframes hold at a cut and follow the path's tangents", "[ai][reframe][path]") {
    std::vector<double> ramp;
    for (int i = 0; i < 50; ++i) {
        ramp.push_back(100.0 + 4.0 * i); // a constant-velocity pan in pixels
    }
    const auto times = grid(ramp.size());
    const Curve straight = fitCurve(times, ramp, 0.5);
    REQUIRE(straight.keys.size() == 2);
    CHECK(straight.keys[0].out.slope == Approx(40.0)); // pixels per second
    CHECK(maxCurveError(straight, times, ramp) < 1e-6);

    const Curve held = fitCurve(times, ramp, 0.5, {}, {}, true);
    CHECK(held.keys.back().interpolation == Interpolation::Hold);
    CHECK(maxCurveError(held, times, ramp) <= 0.5 + 1e-9);
    const Curve still = fitCurve(times, std::vector<double>(50, 3.0), 0.5, {}, {}, true);
    REQUIRE(still.keys.size() == 1);
    CHECK(still.keys[0].interpolation == Interpolation::Hold);
}

TEST_CASE("The solver converges quickly", "[ai][reframe][path]") {
    // The move above with a small iteration budget: already the optimum (still, then steady).
    std::vector<double> desired;
    for (int i = 0; i < 120; ++i) {
        const double t = i * 0.1;
        desired.push_back(t < 4.0 ? 0.3 : t > 8.0 ? 0.7 : 0.3 + 0.1 * (t - 4.0));
    }
    for (const double smoothness : {0.0, 0.5, 1.0}) {
        INFO(smoothness);
        const auto quick = solveCameraPath(problemOf(desired), weightsForSmoothness(smoothness), PathSolveOptions{500, 1e-9});
        const auto slow = solveCameraPath(problemOf(desired), weightsForSmoothness(smoothness), PathSolveOptions{20000, 1e-12});
        for (std::size_t i = 0; i < quick.size(); ++i) {
            REQUIRE(quick[i] == Approx(slow[i]).margin(5e-4)); // under a pixel of a 1920-pixel picture
        }
    }
}
