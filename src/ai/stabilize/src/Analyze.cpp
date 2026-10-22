// The global-motion analysis of the stabilizer (C.15): per frame, features on a grid of cells of the keyframe, the I.6
// tracker's pyramidal Lucas-Kanade there and back (forward-backward check), and a robust fit of the method's motion.
//
// Keyframes: each frame is matched against the latest keyframe rather than against the frame before it, so the errors
// of the per-frame estimates do not add up into drift while the keyframe lasts (a shaky but static shot keeps one
// keyframe throughout). A new keyframe is taken when the picture has moved far from it (Lucas-Kanade's reach) or
// matches it poorly. When the keyframe cannot be matched at all, the previous picture is tried (a cut: the new shot
// starts from the camera position the lost frames held); when neither matches, the frame is lost (identity) and the
// keyframe is kept, so a blank frame in the middle of a shot does not break the path.

#include <wabcut/ai/stabilize/Stabilize.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace wabcut::ai::stabilize {

namespace {

using masks::GrayImage;
using masks::Point2;
using Pyramid = std::vector<GrayImage>;

constexpr int kMaxPictureSide = 16384;
constexpr int kFeaturesPerCell = 4;
constexpr double kMinRelativeStrength = 0.001; ///< of the strongest feature's: weaker ones are noise in flat cells
constexpr double kRekeyDistance = 0.08;        ///< of the longer side: a corner moved further from the keyframe
constexpr double kRekeyConfidence = 0.5;
constexpr double kMaxStepDistance = 0.5; ///< of the longer side: a corner moving further in one step is not a camera
constexpr double kMinAreaRatio = 0.5;    ///< a step shrinking or growing the picture more is not a camera either

std::unexpected<Error> failure(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, "stabilize: " + std::move(message)});
}

double clampFinite(double v, double lo, double hi, double fallback) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; }

/// The tracker's options made safe, with the model of the method.
masks::TrackerOptions trackerFor(const AnalyzeOptions& options) {
    const masks::TrackerOptions defaults;
    masks::TrackerOptions t = options.tracker;
    t.model = options.method == Method::Position ? masks::MotionModel::Translation : masks::MotionModel::Similarity;
    t.window = std::clamp(t.window | 1, 3, 51);
    t.levels = std::clamp(t.levels, 1, 8);
    t.iterations = std::clamp(t.iterations, 1, 100);
    t.epsilon = clampFinite(t.epsilon, 1e-6, 1.0, defaults.epsilon);
    t.featureQuality = clampFinite(t.featureQuality, 1e-6, 1.0, defaults.featureQuality);
    t.minDistance = clampFinite(t.minDistance, 1.0, 100.0, defaults.minDistance);
    t.forwardBackward = clampFinite(t.forwardBackward, 0.01, 10.0, defaults.forwardBackward);
    t.ransacThreshold = clampFinite(t.ransacThreshold, 0.01, 20.0, defaults.ransacThreshold);
    t.ransacIterations = std::clamp(t.ransacIterations, 1, 5000);
    t.minConfidence = clampFinite(t.minConfidence, 0.0, 1.0, defaults.minConfidence);
    t.minInliers = std::clamp(t.minInliers, options.method == Method::Perspective ? 8 : 2, 1000);
    return t;
}

/// Shi-Tomasi strength at a pixel: the smaller eigenvalue of the 5x5 structure tensor (as goodFeatures scores).
double cornerStrength(const GrayImage& image, Point2 p) {
    const int cx = static_cast<int>(std::lround(p.x));
    const int cy = static_cast<int>(std::lround(p.y));
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (int y = cy - 2; y <= cy + 2; ++y) {
        for (int x = cx - 2; x <= cx + 2; ++x) {
            const double gx = 0.5 * (static_cast<double>(image.at(x + 1, y)) - image.at(x - 1, y));
            const double gy = 0.5 * (static_cast<double>(image.at(x, y + 1)) - image.at(x, y - 1));
            sxx += gx * gx;
            sxy += gx * gy;
            syy += gy * gy;
        }
    }
    return 0.5 * (sxx + syy) - std::sqrt(0.25 * (sxx - syy) * (sxx - syy) + sxy * sxy);
}

/// About `count` features spread over a grid of cells (the strongest few of each cell), inside the margin the
/// Lucas-Kanade window needs; noise-level features of flat cells are dropped.
std::vector<Point2> gridFeatures(const GrayImage& image, int count, const masks::TrackerOptions& tracker) {
    std::vector<Point2> out;
    const int margin = tracker.window / 2 + 1;
    const double x0 = margin - 0.5;
    const double y0 = margin - 0.5;
    const double width = image.width - 2.0 * margin;
    const double height = image.height - 2.0 * margin;
    if (width < 4.0 || height < 4.0) {
        return out;
    }
    const int cells = std::max(1, count / kFeaturesPerCell);
    const int cols = std::clamp(static_cast<int>(std::lround(std::sqrt(cells * width / height))), 1, cells);
    const int rows = std::max(1, static_cast<int>(std::lround(static_cast<double>(cells) / cols)));
    const int perCell = std::max(1, (count + cols * rows - 1) / (cols * rows));
    std::vector<double> strength;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const double left = x0 + width * c / cols;
            const double right = x0 + width * (c + 1) / cols;
            const double top = y0 + height * r / rows;
            const double bottom = y0 + height * (r + 1) / rows;
            const std::array<Point2, 4> cell{{{left, top}, {right, top}, {right, bottom}, {left, bottom}}};
            for (const Point2& p : masks::goodFeatures(image, cell, perCell, tracker.featureQuality, tracker.minDistance)) {
                out.push_back(p);
                strength.push_back(cornerStrength(image, p));
            }
        }
    }
    const double strongest = strength.empty() ? 0.0 : *std::max_element(strength.begin(), strength.end());
    std::vector<Point2> kept;
    kept.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (strength[i] > kMinRelativeStrength * strongest) {
            kept.push_back(out[i]);
        }
    }
    return kept;
}

std::array<Point2, 4> cornersOf(int w, int h) {
    const double r = w - 0.5;
    const double b = h - 0.5;
    return {{{-0.5, -0.5}, {r, -0.5}, {r, b}, {-0.5, b}}};
}

/// Largest distance a corner of the picture moves (infinity if one goes behind the camera).
double cornerDistance(const Homography& h, int w, int ht) {
    double worst = 0.0;
    for (const Point2 p : cornersOf(w, ht)) {
        const double z = h.m[6] * p.x + h.m[7] * p.y + h.m[8];
        if (!(z > 0.0)) {
            return std::numeric_limits<double>::infinity();
        }
        const Point2 q = h.map(p);
        const double d = std::hypot(q.x - p.x, q.y - p.y);
        worst = std::isfinite(d) ? std::max(worst, d) : std::numeric_limits<double>::infinity();
    }
    return worst;
}

/// A motion a camera can make between two frames: corners stay in front and near, the picture's area roughly kept.
bool plausible(const Homography& h, int w, int ht) {
    if (!(cornerDistance(h, w, ht) <= kMaxStepDistance * std::max(w, ht))) {
        return false;
    }
    const auto c = cornersOf(w, ht);
    double area = 0.0;
    for (std::size_t k = 0; k < 4; ++k) {
        const Point2 p = h.map(c[k]);
        const Point2 q = h.map(c[(k + 1) % 4]);
        area += p.x * q.y - q.x * p.y;
    }
    const double ratio = 0.5 * area / (static_cast<double>(w) * ht);
    return ratio >= kMinAreaRatio && ratio <= 1.0 / kMinAreaRatio;
}

struct Step {
    Homography motion;
    double confidence = 0.0;
    bool lost = true;
};

/// The motion from the picture of `from` (with its features) to the picture of `to`.
Step trackStep(const Pyramid& from, std::span<const Point2> features, const Pyramid& to, Method method,
               const masks::TrackerOptions& tracker) {
    Step out;
    if (from.empty() || to.empty() || features.size() < static_cast<std::size_t>(tracker.minInliers)) {
        return out;
    }
    const std::vector<masks::PointTrack> forward = masks::trackPoints(from, to, features, tracker);
    std::vector<Point2> moved;
    std::vector<std::size_t> index;
    moved.reserve(forward.size());
    index.reserve(forward.size());
    for (std::size_t i = 0; i < forward.size(); ++i) {
        if (forward[i].ok) {
            moved.push_back(forward[i].to);
            index.push_back(i);
        }
    }
    const std::vector<masks::PointTrack> backward = masks::trackPoints(to, from, moved, tracker);
    std::vector<Point2> a;
    std::vector<Point2> b;
    a.reserve(moved.size());
    b.reserve(moved.size());
    for (std::size_t k = 0; k < backward.size(); ++k) {
        const Point2 origin = features[index[k]];
        if (backward[k].ok && std::hypot(backward[k].to.x - origin.x, backward[k].to.y - origin.y) <= tracker.forwardBackward) {
            a.push_back(origin);
            b.push_back(moved[k]);
        }
    }
    std::vector<char> inliers;
    std::optional<Homography> fit;
    if (method == Method::Perspective) {
        fit = fitHomography(a, b, tracker.ransacThreshold, tracker.ransacIterations, &inliers);
    } else if (const auto motion = masks::fitMotion(a, b, tracker.model, tracker.ransacThreshold, tracker.ransacIterations, &inliers)) {
        fit = Homography::fromMotion(*motion);
    }
    if (!fit) {
        return out;
    }
    const auto count = std::count(inliers.begin(), inliers.end(), char{1});
    const int w = from.front().width;
    const int h = from.front().height;
    out.motion = *fit;
    out.confidence = std::clamp(static_cast<double>(count) / static_cast<double>(features.size()), 0.0, 1.0);
    out.lost = count < tracker.minInliers || out.confidence < tracker.minConfidence || !plausible(*fit, w, h);
    return out;
}

} // namespace

std::expected<MotionAnalysis, Error> analyzeMotion(std::span<const Ticks> frameTimes, const FrameReader& read,
                                                   const AnalyzeOptions& options, const Progress& progress) {
    if (!read) {
        return failure(ErrorCode::InvalidArgument, "no frame reader");
    }
    if (frameTimes.empty()) {
        return failure(ErrorCode::InvalidArgument, "no frames to analyse");
    }
    if (frameTimes.size() > kMaxAnalysisFrames) {
        return failure(ErrorCode::InvalidArgument, "more than " + std::to_string(kMaxAnalysisFrames) + " frames");
    }
    for (std::size_t i = 1; i < frameTimes.size(); ++i) {
        if (frameTimes[i] <= frameTimes[i - 1]) {
            return failure(ErrorCode::InvalidArgument, "frame times must increase");
        }
    }
    masks::TrackerOptions tracker = trackerFor(options);
    int featureCount = std::clamp(options.gridFeatures, 8, 4000);
    if (options.detailed) {
        // P.2 Detailed Analysis: more features, a stricter round trip.
        featureCount = std::min(4000, featureCount * 5 / 2);
        tracker.forwardBackward = std::max(0.01, tracker.forwardBackward * 0.6);
    }
    const std::size_t n = frameTimes.size();

    MotionAnalysis out;
    out.method = options.method;
    out.frames.reserve(n);
    Pyramid key;                    // the keyframe's picture
    std::vector<Point2> keyFeatures; // ... and its features
    Homography keyToPrevious;        // the keyframe's picture -> the previous frame's camera position
    Pyramid previous;                // the previous frame's picture
    bool previousIsKey = true;
    for (std::size_t i = 0; i < n; ++i) {
        auto picture = read(frameTimes[i]);
        if (!picture) {
            return std::unexpected(picture.error());
        }
        const int w = picture->width;
        const int h = picture->height;
        if (w < 1 || h < 1 || w > kMaxPictureSide || h > kMaxPictureSide ||
            picture->px.size() != static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
            return failure(ErrorCode::InvalidArgument, "frame " + std::to_string(i) + " is not a valid picture");
        }
        if (i == 0) {
            out.width = w;
            out.height = h;
        } else if (w != out.width || h != out.height) {
            return failure(ErrorCode::InvalidArgument, "frame " + std::to_string(i) + " is " + std::to_string(w) + "x" + std::to_string(h) +
                                                           ", not " + std::to_string(out.width) + "x" + std::to_string(out.height));
        }
        Pyramid current = masks::buildPyramid(*picture, tracker.levels);
        FrameMotion frame{frameTimes[i], Homography{}, 1.0, false};
        bool rekey = i == 0;
        if (i > 0) {
            const Step step = trackStep(key, keyFeatures, current, options.method, tracker);
            const auto back = keyToPrevious.inverted();
            if (!step.lost && back) {
                frame.motion = step.motion.after(*back);
                frame.confidence = step.confidence;
                keyToPrevious = step.motion;
                rekey = !(cornerDistance(step.motion, w, h) <= kRekeyDistance * std::max(w, h)) || step.confidence < kRekeyConfidence;
            } else {
                Step fallback;
                if (!previousIsKey) {
                    const std::vector<Point2> features = gridFeatures(previous.front(), featureCount, tracker);
                    fallback = trackStep(previous, features, current, options.method, tracker);
                }
                if (!fallback.lost) {
                    frame.motion = fallback.motion;
                    frame.confidence = fallback.confidence;
                    rekey = true;
                } else {
                    // Lost: the camera holds; keep the keyframe unless it has nothing to match.
                    frame.lost = true;
                    frame.confidence = step.confidence;
                    rekey = keyFeatures.size() < static_cast<std::size_t>(tracker.minInliers);
                }
            }
        }
        if (rekey) {
            key = current;
            keyFeatures = gridFeatures(key.front(), featureCount, tracker);
            keyToPrevious = Homography{};
        }
        previous = std::move(current);
        previousIsKey = rekey;
        out.frames.push_back(frame);
        if (progress && !progress(static_cast<double>(i + 1) / static_cast<double>(n))) {
            return failure(ErrorCode::Cancelled, "analysis cancelled");
        }
    }
    return out;
}

} // namespace wabcut::ai::stabilize
