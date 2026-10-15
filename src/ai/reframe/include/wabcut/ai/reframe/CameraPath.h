#pragma once

// The virtual camera of auto reframe (AI-9, docs/ai.md "Auto reframe"), after Google's AutoFlip: per shot, the crop
// window's centre follows an L1-optimal path. Minimising the L1 norms of its first, second and third differences
// makes the path piecewise still, then constant-velocity, then constant-acceleration (an editor's pan, never a
// jittery follow), while a weighted L1 data term pulls it towards where the subject is, and hard bounds keep the
// subject inside the window (the safe region). Solved by ADMM: a banded linear system (factored once) and
// soft-thresholds; deterministic.
//
// The path is then written as sparse keyframes: fitCurve() picks the fewest keys whose curve (evaluated by the model's
// own Curve, Bezier handles from the path's tangents) stays within a tolerance of the path and inside its bounds.

#include <wabcut/model/Parameters.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <span>
#include <vector>

namespace wabcut::ai::reframe {

/// What the path pays for (per second of the shot for the data term; per unit of change for the rest).
struct PathWeights {
    double data = 1.0;         ///< distance from the desired position, per second
    double velocity = 0.3;     ///< |x[i+1] - x[i]|: total travel
    double acceleration = 0.4; ///< |second difference| / dt: changes of speed
    double jerk = 0.4;         ///< |third difference| / dt^2: changes of acceleration
};

/// The weights of a smoothness setting: 0 = follows closely, 0.5 = the default, 1 = moves only when it must.
PathWeights weightsForSmoothness(double smoothness);

/// One shot's path problem on a uniform grid of samples `dt` seconds apart.
struct PathProblem {
    double dt = 0.1;
    std::vector<double> desired; ///< where the window centre wants to be
    std::vector<double> weight;  ///< data weight per sample (0 = no preference); empty = all 1
    std::vector<double> lower;   ///< hard bounds of the centre (the safe region within the frame), lower <= upper
    std::vector<double> upper;
};

struct PathSolveOptions {
    int maxIterations = 4000;
    double tolerance = 1e-7; ///< primal and dual residuals (per sample) at which ADMM stops
};

/// The optimal path (same length as `desired`, within the bounds). An empty problem gives an empty path.
std::vector<double> solveCameraPath(const PathProblem& problem, const PathWeights& weights, const PathSolveOptions& options = {});

/// Sparse keyframes for a sampled path: Bezier keys (tangents from the path) at the fewest samples whose curve stays
/// within `tolerance` of every sample and within [lower, upper] when bounds are given (the bounds must hold for the
/// samples themselves). A path that varies less than `tolerance` becomes one key. `times` increase strictly.
/// `holdLast`: the last key holds its value until whatever key follows (a shot cut), and the fit accounts for it.
Curve fitCurve(std::span<const Ticks> times, std::span<const double> values, double tolerance,
               std::span<const double> lower = {}, std::span<const double> upper = {}, bool holdLast = false);

/// Largest |difference| between the curve and the samples.
double maxCurveError(const Curve& curve, std::span<const Ticks> times, std::span<const double> values);

} // namespace wabcut::ai::reframe
