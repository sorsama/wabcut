// The stabilizer's camera path (C.15): compose the analysed motions, smooth the path's parameters (Gaussian, or the
// L1-optimal path of auto reframe) or lock it (No Motion), and turn "smoothed after the inverse of the real path" into the
// compensating transform per frame, with the framing (auto scale, crop edges).
//
// Parameters of the path, per method: Position, the motion of the frame centre; Similarity, the closest similarity
// about the centre (centre motion, rotation unwrapped over time, log scale); Perspective, the four frame corners.
// Translations and corners are smoothed in units of the picture's longer side (the L1 solver's steps are tuned for
// fractions of a picture), rotations in radians.
//
// Gaussian: sigma = smoothness seconds. Each sample is a Gaussian-weighted local LINEAR fit, which in the middle of a
// clip is exactly the Gaussian average and at its ends continues the path's trend (a reflection about the end that
// does not keep the end frame's own jitter), so a pan keeps its speed to the last frame. Lost frames weigh nothing.

#include <wabcut/ai/reframe/CameraPath.h>
#include <wabcut/ai/stabilize/Stabilize.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace wabcut::ai::stabilize {

using masks::Point2;

namespace {

constexpr double kMaxSmoothness = 10.0;
constexpr double kInfinity = std::numeric_limits<double>::infinity();

double clampFinite(double v, double lo, double hi, double fallback) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; }

Point2 centreOf(int w, int h) { return {(w - 1) / 2.0, (h - 1) / 2.0}; }

std::array<Point2, 4> cornersOf(int w, int h) {
    const double r = w - 0.5;
    const double b = h - 0.5;
    return {{{-0.5, -0.5}, {r, -0.5}, {r, b}, {-0.5, b}}};
}

bool finite(const Homography& h) {
    return std::all_of(h.m.begin(), h.m.end(), [](double v) { return std::isfinite(v); });
}

/// Rotate (radians, clockwise on screen) and scale about `c`, then move by (x, y).
Homography similarityAbout(Point2 c, double x, double y, double radians, double scale) {
    const double a = scale * std::cos(radians);
    const double d = scale * std::sin(radians);
    Homography h;
    h.m = {a, -d, c.x + x - (a * c.x - d * c.y), d, a, c.y + y - (d * c.x + a * c.y), 0.0, 0.0, 1.0};
    return h;
}

Homography scaleAbout(Point2 c, double s) { return similarityAbout(c, 0.0, 0.0, 0.0, s); }

/// The closest similarity about `c`: the motion of c, the rotation, the scale.
struct SimilarityParts {
    double x = 0.0;
    double y = 0.0;
    double radians = 0.0;
    double scale = 1.0;
};

SimilarityParts similarityOf(const Homography& h, Point2 c) {
    const Point2 moved = h.map(c);
    const double p = 0.5 * (h.m[0] + h.m[4]);
    const double q = 0.5 * (h.m[3] - h.m[1]);
    return {moved.x - c.x, moved.y - c.y, std::atan2(q, p), std::hypot(p, q)};
}

/// Point mapped with its homogeneous weight (> 0 in front of the camera).
bool mapInFront(const Homography& h, Point2 p, Point2& out) {
    const double w = h.m[6] * p.x + h.m[7] * p.y + h.m[8];
    if (!(w > 0.0)) {
        return false;
    }
    out = {(h.m[0] * p.x + h.m[1] * p.y + h.m[2]) / w, (h.m[3] * p.x + h.m[4] * p.y + h.m[5]) / w};
    return std::isfinite(out.x) && std::isfinite(out.y);
}

// ---- Smoothing --------------------------------------------------------------------------------------------------

/// Gaussian-weighted local linear regression (see the top of the file).
std::vector<double> gaussianSmooth(const std::vector<double>& x, const std::vector<double>& weight, double sigma) {
    const std::size_t n = x.size();
    std::vector<double> out(x);
    if (n < 2) {
        return out;
    }
    const auto radius = static_cast<std::size_t>(std::min(std::ceil(3.0 * sigma), static_cast<double>(n)));
    std::vector<double> kernel(radius + 1);
    for (std::size_t k = 0; k <= radius; ++k) {
        const double u = static_cast<double>(k) / sigma;
        kernel[k] = std::exp(-0.5 * u * u);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t lo = i > radius ? i - radius : 0;
        const std::size_t hi = std::min(n - 1, i + radius);
        double s0 = 0.0, s1 = 0.0, s2 = 0.0, t0 = 0.0, t1 = 0.0;
        for (std::size_t j = lo; j <= hi; ++j) {
            const double w = kernel[j > i ? j - i : i - j] * weight[j];
            if (w <= 0.0) {
                continue;
            }
            const double d = static_cast<double>(j) - static_cast<double>(i);
            s0 += w;
            s1 += w * d;
            s2 += w * d * d;
            t0 += w * x[j];
            t1 += w * d * x[j];
        }
        if (!(s0 > 0.0)) {
            continue; // nothing measured nearby: keep the path
        }
        const double det = s0 * s2 - s1 * s1;
        out[i] = det > 1e-9 * s0 * std::max(s2, 1.0) ? (s2 * t0 - s1 * t1) / det : t0 / s0;
    }
    return out;
}

/// Auto reframe's L1-optimal path for one parameter: still, then constant velocity, then constant acceleration, within
/// `bound` of the real path. Smoothness scales the path's costs linearly (weightsForSmoothness's 0.05 .. 5 range,
/// extended beyond it), so it is a time constant like the Gaussian's sigma.
std::vector<double> l1Smooth(const std::vector<double>& x, const std::vector<double>& weight, double bound, double dt, double smoothness) {
    reframe::PathProblem problem;
    problem.dt = dt;
    problem.desired = x;
    problem.weight = weight;
    if (std::isfinite(bound)) {
        problem.lower.reserve(x.size());
        problem.upper.reserve(x.size());
        for (const double v : x) {
            problem.lower.push_back(v - bound);
            problem.upper.push_back(v + bound);
        }
    }
    const double k = std::max(smoothness, 1e-3) / 0.5;                      // 1 at the default
    const double setting = std::clamp(0.5 + 0.5 * std::log10(k), 0.0, 1.0); // weightsForSmoothness(s) scales by 10^(2s - 1)
    reframe::PathWeights weights = reframe::weightsForSmoothness(setting);
    const double beyond = k / std::pow(10.0, 2.0 * setting - 1.0);
    weights.velocity *= beyond;
    weights.acceleration *= beyond;
    weights.jerk *= beyond;
    return reframe::solveCameraPath(problem, weights);
}

// ---- Framing ----------------------------------------------------------------------------------------------------

/// Whether the output frame, shrunk by `a` about its centre, lies inside the compensated source picture (`inverse`
/// maps the output back to the source).
bool covers(const Homography& inverse, int w, int h, double a) {
    constexpr double kSlack = 1e-9;
    const Point2 c = centreOf(w, h);
    for (const Point2 p : cornersOf(w, h)) {
        Point2 s;
        if (!mapInFront(inverse, {c.x + (p.x - c.x) / a, c.y + (p.y - c.y) / a}, s)) {
            return false;
        }
        if (s.x < -0.5 - kSlack || s.x > w - 0.5 + kSlack || s.y < -0.5 - kSlack || s.y > h - 0.5 + kSlack) {
            return false;
        }
    }
    return true;
}

/// The smallest uniform scale >= 1 about the centre with which `k`'s picture covers the frame - its inner `inner` part
/// (P.2's action-safe margin; 1 = all of it); infinity above `cap`.
double neededScale(const Homography& k, int w, int h, double cap, double inner = 1.0) {
    const auto inverse = k.inverted();
    if (!inverse) {
        return kInfinity;
    }
    const auto coversAt = [&](double a) { return covers(*inverse, w, h, a / inner); };
    if (coversAt(1.0)) {
        return 1.0;
    }
    if (!coversAt(cap)) {
        return kInfinity;
    }
    double lo = 1.0;
    double hi = cap;
    for (int i = 0; i < 60 && hi - lo > 1e-12; ++i) {
        const double mid = 0.5 * (lo + hi);
        (coversAt(mid) ? hi : lo) = mid;
    }
    return hi;
}

struct Edges {
    double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
};

/// Fractions of the frame the compensated picture leaves uncovered at each edge: the frame minus them lies inside it.
Edges uncovered(const Homography& k, int w, int h) {
    const auto c = cornersOf(w, h);
    std::array<Point2, 4> q{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!mapInFront(k, c[i], q[i])) {
            return {0.5, 0.5, 0.5, 0.5};
        }
    }
    const auto fraction = [](double v, double size) { return std::clamp(v / size, 0.0, 0.5); };
    return {fraction(std::max(q[0].x, q[3].x) + 0.5, w), fraction(std::max(q[0].y, q[1].y) + 0.5, h),
            fraction(w - 0.5 - std::min(q[1].x, q[2].x), w), fraction(h - 0.5 - std::min(q[2].y, q[3].y), h)};
}

/// The rolling shutter's skew of a picture whose scan took `share` of the frame time while it moved (vx, vy) pixels per
/// frame: the line at y (pixel centres) was read (y + 0.5) / h - 0.5 of a frame from mid-frame, so it shows the scene
/// moved by that much of the motion.
Homography skewOf(double share, double vx, double vy, int h) {
    const double kx = share * vx / h;
    const double ky = share * vy / h;
    const double offset = 0.5 - 0.5 * h; // (y + 0.5) / h - 0.5 = (y + offset) / h
    Homography s;
    s.m = {1.0, kx, kx * offset, 0.0, 1.0 + ky, ky * offset, 0.0, 0.0, 1.0};
    return s;
}

/// The camera path of `analysis`: picture 0 -> picture i, lost or unusable steps holding it (`usable` false).
std::vector<Homography> cameraPath(const MotionAnalysis& analysis, std::vector<double>& usable) {
    const std::size_t n = analysis.frames.size();
    std::vector<Homography> path(n);
    usable.assign(n, 1.0);
    if (n == 0) {
        return path;
    }
    usable[0] = analysis.frames[0].lost ? 0.0 : 1.0;
    for (std::size_t i = 1; i < n; ++i) {
        const FrameMotion& f = analysis.frames[i];
        path[i] = path[i - 1];
        if (f.lost || !finite(f.motion) || !f.motion.inverted()) {
            usable[i] = 0.0;
            continue;
        }
        const Homography next = f.motion.after(path[i - 1]);
        if (finite(next) && next.inverted()) {
            path[i] = next;
        } else {
            usable[i] = 0.0;
        }
    }
    return path;
}

} // namespace

std::vector<double> rollingShutterShares(const MotionAnalysis& analysis, RippleReduction mode, double frameRate) {
    const std::size_t n = analysis.frames.size();
    const int w = analysis.width;
    const int h = analysis.height;
    if (mode == RippleReduction::None || analysis.method != Method::Perspective || n < 3 || w < 1 || h < 1) {
        return {};
    }
    std::vector<double> usable;
    const std::vector<Homography> path = cameraPath(analysis, usable);
    const FrameVelocity v = velocities(analysis, w, h);
    if (v.x.size() != n) {
        return {};
    }
    // Per frame: the path's skew at the frame centre, and the motion it should follow. A picture scanned in `share` of
    // a frame while moving (vx, vy) is sheared: d x' / d y = share vx / h, d y' / d y = 1 + share vy / h (relative to
    // picture 0's). A rotation adds -r to d x' / d y and +r to d y' / d x, a zoom the same to both diagonal terms, so
    // the sums below keep the shear and drop those.
    struct Sample {
        double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
        bool ok = false;
    };
    const Point2 c = centreOf(w, h);
    std::vector<Sample> samples(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Homography& p = path[i];
        const double z = p.m[6] * c.x + p.m[7] * c.y + p.m[8];
        Point2 q;
        if (!(usable[i] > 0.0) || !(z > 0.0) || !mapInFront(p, c, q)) {
            continue;
        }
        const double jxx = (p.m[0] - q.x * p.m[6]) / z;
        const double jxy = (p.m[1] - q.x * p.m[7]) / z;
        const double jyx = (p.m[3] - q.y * p.m[6]) / z;
        const double jyy = (p.m[4] - q.y * p.m[7]) / z;
        Sample& s = samples[i];
        s.ax = (jxy + jyx) * h;
        s.ay = (jyy - jxx) * h;
        s.bx = v.x[i] - v.x[0];
        s.by = v.y[i] - v.y[0];
        s.ok = std::isfinite(s.ax) && std::isfinite(s.ay) && std::isfinite(s.bx) && std::isfinite(s.by);
    }
    // Least squares of skew = share * motion over [lo, hi]; `support` is the motion's mean square (px^2 per frame^2).
    const auto shareOver = [&](std::size_t lo, std::size_t hi, double& support) {
        double ab = 0.0;
        double bb = 0.0;
        int count = 0;
        for (std::size_t j = lo; j <= hi; ++j) {
            const Sample& s = samples[j];
            if (s.ok) {
                ab += s.ax * s.bx + s.ay * s.by;
                bb += s.bx * s.bx + s.by * s.by;
                ++count;
            }
        }
        support = count > 0 ? bb / count : 0.0;
        return bb > 0.0 ? ab / bb : 0.0;
    };
    // Too little motion to tell, or a share no camera has (negative, tiny): none.
    constexpr double kMinSupport = 1.0; // the motion must vary by about a pixel per frame (analysis pixels)
    constexpr double kMinShare = 0.05;
    const auto accepted = [&](double share, double support) {
        return support >= kMinSupport && std::isfinite(share) && share >= kMinShare ? std::min(share, 1.0) : 0.0;
    };
    double support = 0.0;
    const double clipShare = shareOver(0, n - 1, support);
    const double whole = accepted(clipShare, support);
    std::vector<double> out(n, whole);
    if (mode == RippleReduction::Enhanced) {
        const auto radius = static_cast<std::size_t>(std::max(2.0, std::round(std::isfinite(frameRate) ? frameRate : 30.0)));
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t lo = i > radius ? i - radius : 0;
            const std::size_t hi = std::min(n - 1, i + radius);
            double local = 0.0;
            const double share = shareOver(lo, hi, local);
            out[i] = local >= kMinSupport ? accepted(share, local) : whole;
        }
    }
    return out;
}

Compensation stabilize(const MotionAnalysis& analysis, const SmoothOptions& options, int sourceWidth, int sourceHeight) {
    Compensation out;
    const std::size_t n = analysis.frames.size();
    const int w = analysis.width;
    const int h = analysis.height;
    if (n == 0 || w < 1 || h < 1 || sourceWidth < 1 || sourceHeight < 1) {
        return out;
    }
    out.times.reserve(n);
    for (const FrameMotion& f : analysis.frames) {
        out.times.push_back(f.time);
    }
    // P.2 Crop Less <-> Smooth More: when the framing crops or scales, it trades smoothing for border size.
    const bool cropping = options.framing == Framing::Crop || options.framing == Framing::CropAutoScale;
    const double tradeOff = cropping ? 0.5 + clampFinite(options.cropLessSmoothMore, 0.0, 1.0, 0.5) : 1.0;
    const double smoothness = clampFinite(options.smoothness, 0.0, kMaxSmoothness, 0.5) * tradeOff;
    const double frameRate = options.frameRate > 0.0 ? clampFinite(options.frameRate, 1e-3, 1000.0, 30.0) : 30.0;
    const double maxScale = clampFinite(options.maxScale, 1.0, 100.0, 1.5);
    const double additional = options.additionalScale > 0.0 ? clampFinite(options.additionalScale, 0.01, 100.0, 1.0) : 1.0;
    const Method method = analysis.method;

    // 1. The camera path: analysis picture 0 -> picture i. Lost (or unusable) steps hold the camera and weigh nothing.
    std::vector<double> weight;
    std::vector<Homography> path = cameraPath(analysis, weight);

    // 1b. P.2 Rolling Shutter Ripple: the path between the pictures as a global shutter would have taken them, and per
    //     frame the de-skew that comes first in its compensation.
    std::vector<Homography> deskew(n);
    if (method == Method::Perspective && options.rippleReduction != RippleReduction::None) {
        const std::vector<double> shares = rollingShutterShares(analysis, options.rippleReduction, frameRate);
        const FrameVelocity v = velocities(analysis, w, h);
        if (shares.size() == n && v.x.size() == n) {
            std::vector<Homography> skew(n);
            for (std::size_t i = 0; i < n; ++i) {
                skew[i] = skewOf(shares[i], v.x[i], v.y[i], h);
            }
            double sum = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                const auto inverse = skew[i].inverted();
                const Homography straight = inverse ? inverse->after(path[i].after(skew[0])) : path[i];
                if (inverse && finite(straight) && straight.inverted()) {
                    path[i] = straight;
                    deskew[i] = *inverse;
                    sum += shares[i];
                }
            }
            out.rollingShutterShare = sum / static_cast<double>(n);
        }
    }

    // 2. Its parameters, in units of the longer side (translations, corners) and radians.
    const Point2 centre = centreOf(w, h);
    const auto base = cornersOf(w, h);
    const double unit = std::max(w, h);
    const double margin = std::max(0.5 * (1.0 - 1.0 / maxScale), 0.02); // how far the L1 path may leave the real one
    const double boundX = margin * w / unit;
    const double boundY = margin * h / unit;
    std::vector<std::vector<double>> dims;
    std::vector<double> bounds;
    switch (method) {
    case Method::Position:
        dims.assign(2, std::vector<double>(n));
        bounds = {boundX, boundY};
        break;
    case Method::Similarity:
        dims.assign(4, std::vector<double>(n));
        bounds = {boundX, boundY, kInfinity, kInfinity};
        break;
    case Method::Perspective:
        dims.assign(8, std::vector<double>(n));
        bounds = {boundX, boundY, boundX, boundY, boundX, boundY, boundX, boundY};
        break;
    }
    for (std::size_t i = 0; i < n; ++i) {
        std::array<double, 8> v{};
        bool ok = true;
        if (method == Method::Perspective) {
            for (std::size_t k = 0; k < 4 && ok; ++k) {
                Point2 q;
                ok = mapInFront(path[i], base[k], q);
                v[2 * k] = q.x / unit;
                v[2 * k + 1] = q.y / unit;
            }
        } else {
            const SimilarityParts s = similarityOf(path[i], centre);
            v = {s.x / unit, s.y / unit, s.radians, std::log(std::max(s.scale, 1e-9))};
            if (i > 0 && method == Method::Similarity) { // unwrap the rotation
                v[2] = dims[2][i - 1] + std::remainder(v[2] - dims[2][i - 1], 2.0 * std::numbers::pi);
            }
        }
        for (std::size_t d = 0; d < dims.size(); ++d) {
            ok = ok && std::isfinite(v[d]);
        }
        for (std::size_t d = 0; d < dims.size(); ++d) {
            // Unusable: hold the previous value (the identity's at the start).
            const double start = method == Method::Perspective ? (d % 2 == 0 ? base[d / 2].x : base[d / 2].y) / unit : 0.0;
            dims[d][i] = ok ? v[d] : (i > 0 ? dims[d][i - 1] : start);
        }
        if (!ok) {
            weight[i] = 0.0;
        }
    }

    // 3. The smoothed (or locked) path.
    std::vector<std::vector<double>> smooth(dims.size());
    for (std::size_t d = 0; d < dims.size(); ++d) {
        if (options.preserveScale && method == Method::Similarity && d == 3) {
            smooth[d] = dims[d]; // P.2 Preserve Scale: the camera's push-ins and pull-outs stay
        } else if (options.noMotion) {
            smooth[d].assign(n, dims[d][0]);
        } else if (options.smoothing == Smoothing::L1) {
            smooth[d] = l1Smooth(dims[d], weight, bounds[d], 1.0 / frameRate, smoothness);
        } else {
            smooth[d] = gaussianSmooth(dims[d], weight, std::max(smoothness * frameRate, 0.5));
        }
    }

    // 4. Compensation = smoothed after the inverse of the real path, then from analysis to source pixels.
    const double sx = static_cast<double>(sourceWidth) / w;
    const double sy = static_cast<double>(sourceHeight) / h;
    Homography toSource;
    toSource.m = {sx, 0.0, 0.5 * sx - 0.5, 0.0, sy, 0.5 * sy - 0.5, 0.0, 0.0, 1.0};
    const Homography fromSource = toSource.inverted().value_or(Homography{});
    const auto transformOf = [&](const std::vector<std::vector<double>>& p, std::size_t i) {
        switch (method) {
        case Method::Position: {
            Homography t;
            t.m[2] = p[0][i] * unit;
            t.m[5] = p[1][i] * unit;
            return t;
        }
        case Method::Similarity:
            return similarityAbout(centre, p[0][i] * unit, p[1][i] * unit, p[2][i], std::exp(p[3][i]));
        case Method::Perspective: {
            std::array<Point2, 4> to{};
            for (std::size_t k = 0; k < 4; ++k) {
                to[k] = {p[2 * k][i] * unit, p[2 * k + 1][i] * unit};
            }
            return homographyFromCorners(base, to).value_or(path[i]);
        }
        }
        return Homography{};
    };
    std::vector<Homography> compensation(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto inverse = transformOf(dims, i).inverted();
        const Homography local = inverse ? transformOf(smooth, i).after(*inverse).after(deskew[i]) : Homography{};
        compensation[i] = finite(local) ? toSource.after(local.after(fromSource)) : Homography{};
    }


    // 5. Framing: the scale that hides every border, the edges ever uncovered; then the transforms.
    const Point2 sourceCentre = centreOf(sourceWidth, sourceHeight);
    const double inner = 1.0 - 2.0 * clampFinite(options.actionSafeMargin, 0.0, 0.49, 0.0); // P.2 action-safe margin
    double needed = 1.0;
    Edges crop;
    const Homography extra = scaleAbout(sourceCentre, additional);
    for (const Homography& k : compensation) {
        needed = std::max(needed, neededScale(k, sourceWidth, sourceHeight, maxScale, inner));
        const Edges e = uncovered(extra.after(k), sourceWidth, sourceHeight);
        crop.left = std::max(crop.left, e.left);
        crop.top = std::max(crop.top, e.top);
        crop.right = std::max(crop.right, e.right);
        crop.bottom = std::max(crop.bottom, e.bottom);
    }
    out.scaleCapped = needed > maxScale;
    out.autoScale = std::min(needed, maxScale);
    out.cropLeft = crop.left;
    out.cropTop = crop.top;
    out.cropRight = crop.right;
    out.cropBottom = crop.bottom;
    const double finalScale = (options.framing == Framing::CropAutoScale ? out.autoScale : 1.0) * additional;
    const Homography framing = scaleAbout(sourceCentre, finalScale);
    if (method == Method::Perspective) {
        out.corners.reserve(n);
    } else {
        out.x.reserve(n);
        out.y.reserve(n);
        out.rotation.reserve(n);
        out.scale.reserve(n);
    }
    const auto sourceCorners = cornersOf(sourceWidth, sourceHeight);
    for (const Homography& k : compensation) {
        const Homography result = framing.after(k);
        if (method == Method::Perspective) {
            std::array<double, 8> offsets{};
            for (std::size_t c = 0; c < 4; ++c) {
                Point2 q;
                if (!mapInFront(result, sourceCorners[c], q)) {
                    q = sourceCorners[c];
                }
                offsets[2 * c] = q.x - sourceCorners[c].x;
                offsets[2 * c + 1] = q.y - sourceCorners[c].y;
            }
            out.corners.push_back(offsets);
        } else {
            const SimilarityParts s = similarityOf(result, sourceCentre);
            out.x.push_back(s.x);
            out.y.push_back(s.y);
            out.rotation.push_back(method == Method::Position ? 0.0 : s.radians * 180.0 / std::numbers::pi);
            out.scale.push_back(s.scale);
        }
    }
    return out;
}

FrameVelocity velocities(const MotionAnalysis& analysis, int sourceWidth, int sourceHeight) {
    FrameVelocity out;
    const std::size_t n = analysis.frames.size();
    if (n == 0 || analysis.width < 1 || analysis.height < 1 || sourceWidth < 1 || sourceHeight < 1) {
        return out;
    }
    // Where the frame centre's content is, per frame (the camera path, lost steps held), in source pixels.
    const Point2 centre = centreOf(analysis.width, analysis.height);
    const double sx = static_cast<double>(sourceWidth) / analysis.width;
    const double sy = static_cast<double>(sourceHeight) / analysis.height;
    std::vector<Point2> at(n, Point2{0.0, 0.0});
    std::vector<bool> known(n, true);
    Homography path;
    for (std::size_t i = 0; i < n; ++i) {
        const FrameMotion& f = analysis.frames[i];
        if (i > 0) {
            if (f.lost || !finite(f.motion)) {
                known[i] = false;
            } else {
                const Homography next = f.motion.after(path);
                if (finite(next)) {
                    path = next;
                } else {
                    known[i] = false;
                }
            }
        }
        Point2 q;
        if (!mapInFront(path, centre, q)) {
            q = centre;
            known[i] = false;
        }
        at[i] = {(q.x - centre.x) * sx, (q.y - centre.y) * sy};
    }
    out.times.reserve(n);
    out.x.reserve(n);
    out.y.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.times.push_back(analysis.frames[i].time);
        const std::size_t a = i > 0 ? i - 1 : i;
        const std::size_t b = i + 1 < n ? i + 1 : i;
        if (b == a || !known[a] || !known[b]) {
            out.x.push_back(i > 0 ? out.x.back() : 0.0);
            out.y.push_back(i > 0 ? out.y.back() : 0.0);
            continue;
        }
        const double steps = static_cast<double>(b - a);
        out.x.push_back((at[b].x - at[a].x) / steps);
        out.y.push_back((at[b].y - at[a].y) / steps);
    }
    return out;
}

Homography compensationAt(const Compensation& c, std::size_t i, int sourceWidth, int sourceHeight) {
    if (sourceWidth < 1 || sourceHeight < 1) {
        return {};
    }
    if (i < c.corners.size()) {
        const auto from = cornersOf(sourceWidth, sourceHeight);
        std::array<Point2, 4> to{};
        for (std::size_t k = 0; k < 4; ++k) {
            to[k] = {from[k].x + c.corners[i][2 * k], from[k].y + c.corners[i][2 * k + 1]};
        }
        return homographyFromCorners(from, to).value_or(Homography{});
    }
    if (i < c.x.size() && i < c.y.size() && i < c.rotation.size() && i < c.scale.size()) {
        return similarityAbout(centreOf(sourceWidth, sourceHeight), c.x[i], c.y[i], c.rotation[i] * std::numbers::pi / 180.0, c.scale[i]);
    }
    return {};
}

} // namespace wabcut::ai::stabilize
