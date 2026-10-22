// C.15 video stabilization (wabcut_ai_stabilize) on synthetic shaky clips with exact sub-pixel truth (the I.6 tracker
// tests' band-limited textures, tests/masks/SyntheticMotion.h, 320x180): a shaky static shot stays put (Gaussian and L1
// smoothing), a pan keeps its speed while its jitter goes, No Motion locks to the first frame, perspective jitter, the
// Position method, cancellation and refused input, a blank frame in the middle, the analysis JSON (round trip and hostile
// input), and the framing (auto scale, its cap, crop edges). Accuracy figures are printed with the prefix "C.15 stabilize:".
//
// Residuals: a grid of scene points is followed through each frame's true content motion and then the frame's
// compensation (compensationAt). "Stay put" is each point's distance from its mean stabilized position; "vs ideal" is the
// distance from where the same smoothing of the TRUE camera path puts it (the analysis' accuracy); "shake" is the
// frame-to-frame movement left.

#include <wabcut/ai/stabilize/Stabilize.h>

#include "../../masks/SyntheticMotion.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai::stabilize;
using Catch::Approx;
using masks::GrayImage;
using masks::Point2;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr std::size_t kFrames = 90;
constexpr double kCx = (kW - 1) / 2.0;
constexpr double kCy = (kH - 1) / 2.0;

using Positions = std::vector<std::vector<Point2>>; // [frame][scene point]

std::vector<Ticks> frameTimes(std::size_t n) {
    std::vector<Ticks> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(Ticks{static_cast<std::int64_t>(i) * (kTicksPerSecond / 30)});
    }
    return out;
}

std::array<Point2, 4> frameCorners() { return {{{-0.5, -0.5}, {kW - 0.5, -0.5}, {kW - 0.5, kH - 0.5}, {-0.5, kH - 0.5}}}; }

/// The picture whose pixel p shows the texture at toTexture(p): SyntheticMotion's render() for a projective map.
GrayImage renderProjective(const masks::test::Texture& texture, const Homography& toTexture) {
    GrayImage image(kW, kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Point2 t = toTexture.map({static_cast<double>(x), static_cast<double>(y)});
            image.px[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)] = static_cast<float>(texture.at(t.x, t.y));
        }
    }
    return image;
}

/// A hand-held camera: the content of frame i moves (texture -> picture) by a uniform jitter of +-shift px and
/// +-degrees about the centre, plus a pan of panPerFrame px per frame to the right.
std::vector<Homography> shakyCamera(std::size_t n, std::uint32_t seed, double panPerFrame, double shift = 6.0, double degrees = 1.5) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<Homography> out;
    for (std::size_t i = 0; i < n; ++i) {
        const double tx = shift * u(rng);
        const double ty = shift * u(rng);
        const double r = degrees * u(rng);
        out.push_back(Homography::fromMotion(masks::test::similarity(r, 1.0, tx + panPerFrame * static_cast<double>(i), ty, kCx, kCy)));
    }
    return out;
}

/// Projective jitter: each corner of the frame moves by a uniform +-offset px in x and in y.
std::vector<Homography> projectiveJitter(std::size_t n, std::uint32_t seed, double offset) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const auto corners = frameCorners();
    std::vector<Homography> out;
    for (std::size_t i = 0; i < n; ++i) {
        std::array<Point2, 4> moved = corners;
        for (Point2& p : moved) {
            const double dx = offset * u(rng);
            const double dy = offset * u(rng);
            p.x += dx;
            p.y += dy;
        }
        out.push_back(homographyFromCorners(corners, moved).value());
    }
    return out;
}

struct Clip {
    std::vector<GrayImage> frames;
    std::vector<Homography> truth; ///< texture -> picture of each frame
};

Clip renderClip(const masks::test::Texture& texture, std::vector<Homography> truth) {
    Clip clip;
    for (const Homography& j : truth) {
        clip.frames.push_back(renderProjective(texture, j.inverted().value()));
    }
    clip.truth = std::move(truth);
    return clip;
}

FrameReader readerOf(const std::vector<GrayImage>& frames, const std::vector<Ticks>& times, int* reads = nullptr) {
    return [&frames, &times, reads](Ticks t) -> std::expected<GrayImage, Error> {
        if (reads) {
            ++*reads;
        }
        const auto it = std::lower_bound(times.begin(), times.end(), t);
        if (it == times.end() || *it != t || static_cast<std::size_t>(it - times.begin()) >= frames.size()) {
            return std::unexpected(Error{ErrorCode::NotFound, "no such frame"});
        }
        return frames[static_cast<std::size_t>(it - times.begin())];
    };
}

/// The shaky static shot of several tests (rendered and analysed once).
const Clip& staticShot() {
    static const Clip clip = renderClip(masks::test::Texture(31), shakyCamera(kFrames, 1234, 0.0));
    return clip;
}

const std::expected<MotionAnalysis, Error>& staticAnalysis() {
    static const std::vector<Ticks> times = frameTimes(kFrames);
    static const std::expected<MotionAnalysis, Error> analysis = analyzeMotion(times, readerOf(staticShot().frames, times));
    return analysis;
}

bool contains(std::span<const std::size_t> list, std::size_t i) { return std::find(list.begin(), list.end(), i) != list.end(); }

/// What a perfect analyser reports: each frame's true motion from the last good frame; `lost` frames identity.
MotionAnalysis truthAnalysis(const std::vector<Homography>& truth, const std::vector<Ticks>& times, Method method,
                             std::span<const std::size_t> lost = {}) {
    MotionAnalysis a;
    a.width = kW;
    a.height = kH;
    a.method = method;
    std::size_t previous = 0;
    for (std::size_t i = 0; i < truth.size(); ++i) {
        FrameMotion f{times[i], Homography{}, 1.0, false};
        if (contains(lost, i)) {
            f.lost = true;
            f.confidence = 0.0;
        } else if (i > 0) {
            f.motion = truth[i].after(truth[previous].inverted().value());
            previous = i;
        }
        a.frames.push_back(f);
    }
    return a;
}

/// A grid of scene points (texture coordinates) over the middle of the picture.
std::vector<Point2> scenePoints() {
    std::vector<Point2> out;
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 9; ++c) {
            out.push_back({40.0 + 30.0 * c, 30.0 + 30.0 * r});
        }
    }
    return out;
}

/// Where each scene point shows in each frame as shot.
Positions shot(const std::vector<Homography>& truth, const std::vector<Point2>& points) {
    Positions out(truth.size());
    for (std::size_t i = 0; i < truth.size(); ++i) {
        for (const Point2 q : points) {
            out[i].push_back(truth[i].map(q));
        }
    }
    return out;
}

/// ... and after each frame's compensation.
Positions stabilized(const Compensation& c, const std::vector<Homography>& truth, const std::vector<Point2>& points) {
    Positions out(truth.size());
    for (std::size_t i = 0; i < truth.size(); ++i) {
        const Homography k = compensationAt(c, i, kW, kH);
        for (const Point2 q : points) {
            out[i].push_back(k.map(truth[i].map(q)));
        }
    }
    return out;
}

double distance(Point2 a, Point2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

struct Stats {
    double rms = 0.0;
    double max = 0.0;
};

class Errors {
public:
    void add(double e) {
        m_sum += e * e;
        m_max = std::max(m_max, e);
        ++m_count;
    }
    Stats stats() const { return {m_count > 0 ? std::sqrt(m_sum / static_cast<double>(m_count)) : 0.0, m_max}; }

private:
    double m_sum = 0.0;
    double m_max = 0.0;
    std::size_t m_count = 0;
};

/// Each point's distance from its mean position over the frames.
Stats stayPut(const Positions& p, std::span<const std::size_t> skip = {}) {
    Errors errors;
    for (std::size_t q = 0; q < p.front().size(); ++q) {
        Point2 mean;
        double count = 0.0;
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (!contains(skip, i)) {
                mean.x += p[i][q].x;
                mean.y += p[i][q].y;
                count += 1.0;
            }
        }
        mean = {mean.x / count, mean.y / count};
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (!contains(skip, i)) {
                errors.add(distance(p[i][q], mean));
            }
        }
    }
    return errors.stats();
}

/// How far the points move from one frame to the next.
Stats frameToFrame(const Positions& p) {
    Errors errors;
    for (std::size_t i = 1; i < p.size(); ++i) {
        for (std::size_t q = 0; q < p[i].size(); ++q) {
            errors.add(distance(p[i][q], p[i - 1][q]));
        }
    }
    return errors.stats();
}

Stats between(const Positions& a, const Positions& b, std::span<const std::size_t> skip = {}) {
    Errors errors;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!contains(skip, i)) {
            for (std::size_t q = 0; q < a[i].size(); ++q) {
                errors.add(distance(a[i][q], b[i][q]));
            }
        }
    }
    return errors.stats();
}

/// Largest distance between where two transforms put the corners of the picture.
double cornerError(const Homography& a, const Homography& b) {
    double worst = 0.0;
    for (const Point2 p : frameCorners()) {
        worst = std::max(worst, distance(a.map(p), b.map(p)));
    }
    return worst;
}

SmoothOptions stabilizeOnly(Smoothing smoothing) {
    SmoothOptions options;
    options.smoothing = smoothing;
    options.framing = Framing::StabilizeOnly;
    return options;
}

const char* nameOf(Smoothing smoothing) { return smoothing == Smoothing::L1 ? "L1" : "Gaussian"; }

ErrorCode codeOf(const std::expected<MotionAnalysis, Error>& result) { return result ? ErrorCode::Unknown : result.error().code; }

/// Whether the compensated picture (source pixels -> output pixels) covers the output rectangle [x0, x1] x [y0, y1].
bool coversRect(const Homography& k, double x0, double y0, double x1, double y1) {
    const auto inverse = k.inverted();
    if (!inverse) {
        return false;
    }
    constexpr double kSlack = 1e-6;
    for (const Point2 p : {Point2{x0, y0}, Point2{x1, y0}, Point2{x1, y1}, Point2{x0, y1}}) {
        const Point2 s = inverse->map(p);
        if (s.x < -0.5 - kSlack || s.x > kW - 0.5 + kSlack || s.y < -0.5 - kSlack || s.y > kH - 0.5 + kSlack) {
            return false;
        }
    }
    return true;
}

bool coversFrame(const Homography& k) { return coversRect(k, -0.5, -0.5, kW - 0.5, kH - 0.5); }

Homography scaledAboutCentre(const Homography& k, double s) {
    Homography scale;
    scale.m = {s, 0.0, kCx * (1.0 - s), 0.0, s, kCy * (1.0 - s), 0.0, 0.0, 1.0};
    return scale.after(k);
}

} // namespace

TEST_CASE("Stabilize: a shaky static shot stays put", "[ai][stabilize]") {
    const Clip& clip = staticShot();
    const auto times = frameTimes(kFrames);
    const auto& analysis = staticAnalysis();
    REQUIRE(analysis);
    CHECK(analysis->width == kW);
    CHECK(analysis->height == kH);
    CHECK(analysis->method == Method::Similarity);
    REQUIRE(analysis->frames.size() == kFrames);
    double worstStep = 0.0;
    double lowestConfidence = 1.0;
    for (std::size_t i = 1; i < kFrames; ++i) {
        const FrameMotion& f = analysis->frames[i];
        CHECK_FALSE(f.lost);
        CHECK(f.time == times[i]);
        lowestConfidence = std::min(lowestConfidence, f.confidence);
        worstStep = std::max(worstStep, cornerError(f.motion, clip.truth[i].after(clip.truth[i - 1].inverted().value())));
    }
    std::printf("C.15 stabilize: static shot analysis (Similarity, %zu frames of %dx%d, jitter +-6 px +-1.5 deg): worst per-frame "
                "motion error %.4f px at the picture corners, lowest confidence %.2f\n",
                kFrames, kW, kH, worstStep, lowestConfidence);
    CHECK(worstStep < 0.5);
    CHECK(lowestConfidence > 0.5);

    const auto points = scenePoints();
    const Stats shaky = stayPut(shot(clip.truth, points));
    const MotionAnalysis truth = truthAnalysis(clip.truth, times, Method::Similarity);
    for (const Smoothing smoothing : {Smoothing::Gaussian, Smoothing::L1}) {
        INFO(nameOf(smoothing));
        const SmoothOptions options = stabilizeOnly(smoothing);
        const Compensation result = stabilize(*analysis, options, kW, kH);
        REQUIRE(result.times.size() == kFrames);
        REQUIRE(result.x.size() == kFrames);
        CHECK(result.corners.empty());
        const Positions positions = stabilized(result, clip.truth, points);
        const Stats still = stayPut(positions);
        const Stats shake = frameToFrame(positions);
        const Stats accuracy = between(positions, stabilized(stabilize(truth, options, kW, kH), clip.truth, points));
        std::printf("C.15 stabilize: static shot, %s smoothing 0.5: stay put RMS %.3f max %.3f px (as shot RMS %.3f max %.3f); "
                    "frame-to-frame shake RMS %.3f max %.3f px; vs ideal analysis RMS %.3f max %.3f px\n",
                    nameOf(smoothing), still.rms, still.max, shaky.rms, shaky.max, shake.rms, shake.max, accuracy.rms, accuracy.max);
        CHECK(accuracy.rms < 0.3);
        CHECK(accuracy.max < 1.0);
        CHECK(shake.rms < 0.3);
        if (smoothing == Smoothing::L1) {
            CHECK(still.rms < 0.3);
            CHECK(still.max < 1.0);
        } else {
            // A Gaussian of sigma 0.5 s averages ~53 frames of white jitter: the smoothed path itself wanders by about
            // 6 / sqrt(3) / sqrt(53) = 0.48 px per axis, which "stay put" measures on top of the analysis error. At the
            // clip's ends the local linear fit sees half the kernel and wanders about twice as far (P.2: measured max
            // 3.2 px, at the ends).
            CHECK(still.rms < 1.0);
            CHECK(still.max < 4.0);
        }
    }
}

TEST_CASE("Stabilize: a pan is kept while its jitter goes", "[ai][stabilize]") {
    const Clip clip = renderClip(masks::test::Texture(47), shakyCamera(kFrames, 99, 2.0));
    const auto times = frameTimes(kFrames);
    const auto analysis = analyzeMotion(times, readerOf(clip.frames, times));
    REQUIRE(analysis);
    CHECK(std::none_of(analysis->frames.begin(), analysis->frames.end(), [](const FrameMotion& f) { return f.lost; }));
    const auto points = scenePoints();
    const auto measure = [&](const Positions& p, double& meanX, double& meanY, Stats& line, Stats& jitter) {
        double sumX = 0.0;
        double sumY = 0.0;
        double steps = 0.0;
        Errors lineErrors;
        Errors jitterErrors;
        const double n = static_cast<double>(p.size());
        const double meanI = (n - 1.0) / 2.0;
        double sii = 0.0;
        for (std::size_t i = 0; i < p.size(); ++i) {
            sii += (static_cast<double>(i) - meanI) * (static_cast<double>(i) - meanI);
        }
        for (std::size_t q = 0; q < points.size(); ++q) {
            Point2 mean;
            for (std::size_t i = 0; i < p.size(); ++i) {
                mean.x += p[i][q].x / n;
                mean.y += p[i][q].y / n;
                if (i > 0) {
                    sumX += p[i][q].x - p[i - 1][q].x;
                    sumY += p[i][q].y - p[i - 1][q].y;
                    steps += 1.0;
                }
                if (i > 0 && i + 1 < p.size()) {
                    jitterErrors.add(distance(p[i][q], {0.5 * (p[i - 1][q].x + p[i + 1][q].x), 0.5 * (p[i - 1][q].y + p[i + 1][q].y)}));
                }
            }
            double six = 0.0;
            double siy = 0.0;
            for (std::size_t i = 0; i < p.size(); ++i) {
                six += (static_cast<double>(i) - meanI) * (p[i][q].x - mean.x);
                siy += (static_cast<double>(i) - meanI) * (p[i][q].y - mean.y);
            }
            for (std::size_t i = 0; i < p.size(); ++i) {
                const double d = static_cast<double>(i) - meanI;
                lineErrors.add(distance(p[i][q], {mean.x + six / sii * d, mean.y + siy / sii * d}));
            }
        }
        meanX = sumX / steps;
        meanY = sumY / steps;
        line = lineErrors.stats();
        jitter = jitterErrors.stats();
    };
    double shotX = 0.0, shotY = 0.0;
    Stats shotLine, shotJitter;
    measure(shot(clip.truth, points), shotX, shotY, shotLine, shotJitter);
    for (const Smoothing smoothing : {Smoothing::Gaussian, Smoothing::L1}) {
        INFO(nameOf(smoothing));
        const Positions p = stabilized(stabilize(*analysis, stabilizeOnly(smoothing), kW, kH), clip.truth, points);
        double meanX = 0.0, meanY = 0.0;
        Stats line, jitter;
        measure(p, meanX, meanY, line, jitter);
        std::printf("C.15 stabilize: pan 2 px/frame + jitter, %s: motion (%.3f, %.3f) px/frame; off a straight line RMS %.3f "
                    "max %.3f px (as shot %.3f / %.3f); frame-to-frame jitter (off the neighbours' midpoint) RMS %.3f max %.3f px "
                    "(as shot %.3f / %.3f)\n",
                    nameOf(smoothing), meanX, meanY, line.rms, line.max, shotLine.rms, shotLine.max, jitter.rms, jitter.max,
                    shotJitter.rms, shotJitter.max);
        CHECK(meanX == Approx(2.0).margin(0.1));
        CHECK(std::abs(meanY) < 0.1);
        // The Gaussian path wanders about 0.48 px per axis around the true line (see the static shot), 0.68 px in 2D;
        // the L1 path keeps to it.
        CHECK(line.rms < (smoothing == Smoothing::L1 ? 0.3 : 1.0));
        CHECK(jitter.rms < 0.2);
    }
}

TEST_CASE("Stabilize: No Motion locks every frame to the first", "[ai][stabilize]") {
    const Clip& clip = staticShot();
    const auto& analysis = staticAnalysis();
    REQUIRE(analysis);
    const auto points = scenePoints();
    for (const Smoothing smoothing : {Smoothing::Gaussian, Smoothing::L1}) {
        INFO(nameOf(smoothing));
        SmoothOptions options = stabilizeOnly(smoothing);
        options.noMotion = true;
        const Positions p = stabilized(stabilize(*analysis, options, kW, kH), clip.truth, points);
        Errors errors;
        for (std::size_t i = 0; i < kFrames; ++i) {
            for (std::size_t q = 0; q < points.size(); ++q) {
                errors.add(distance(p[i][q], clip.truth[0].map(points[q])));
            }
        }
        const Stats locked = errors.stats();
        std::printf("C.15 stabilize: No Motion (%s): distance from the first frame's view RMS %.3f max %.3f px\n", nameOf(smoothing),
                    locked.rms, locked.max);
        CHECK(locked.max < 0.5);
    }
}

TEST_CASE("Stabilize: perspective jitter is taken out", "[ai][stabilize]") {
    const Clip clip = renderClip(masks::test::Texture(53), projectiveJitter(kFrames, 7, 3.0));
    const auto times = frameTimes(kFrames);
    AnalyzeOptions analyze;
    analyze.method = Method::Perspective;
    const auto analysis = analyzeMotion(times, readerOf(clip.frames, times), analyze);
    REQUIRE(analysis);
    CHECK(analysis->method == Method::Perspective);
    double worstStep = 0.0;
    for (std::size_t i = 1; i < kFrames; ++i) {
        CHECK_FALSE(analysis->frames[i].lost);
        worstStep = std::max(worstStep, cornerError(analysis->frames[i].motion, clip.truth[i].after(clip.truth[i - 1].inverted().value())));
    }
    const auto points = scenePoints();
    const SmoothOptions options = stabilizeOnly(Smoothing::Gaussian);
    const Compensation result = stabilize(*analysis, options, kW, kH);
    CHECK(result.x.empty());
    REQUIRE(result.corners.size() == kFrames);
    const Positions positions = stabilized(result, clip.truth, points);
    const Stats shaky = stayPut(shot(clip.truth, points));
    const Stats still = stayPut(positions);
    const Stats accuracy =
        between(positions, stabilized(stabilize(truthAnalysis(clip.truth, times, Method::Perspective), options, kW, kH), clip.truth, points));
    std::printf("C.15 stabilize: perspective jitter (corners +-3 px), Gaussian: worst per-frame motion error %.4f px; stay put RMS "
                "%.3f max %.3f px (as shot RMS %.3f max %.3f); vs ideal analysis RMS %.3f max %.3f px\n",
                worstStep, still.rms, still.max, shaky.rms, shaky.max, accuracy.rms, accuracy.max);
    CHECK(accuracy.rms < 0.3);
    CHECK(accuracy.max < 1.0);
    CHECK(still.max < 1.0);
    CHECK(still.rms < shaky.rms / 3.0);
}

TEST_CASE("Stabilize: the Position method takes out translation only", "[ai][stabilize]") {
    constexpr std::size_t kShort = 60;
    const Clip clip = renderClip(masks::test::Texture(61), shakyCamera(kShort, 5, 0.0, 6.0, 0.0));
    const auto times = frameTimes(kShort);
    AnalyzeOptions analyze;
    analyze.method = Method::Position;
    const auto analysis = analyzeMotion(times, readerOf(clip.frames, times), analyze);
    REQUIRE(analysis);
    for (const FrameMotion& f : analysis->frames) {
        CHECK_FALSE(f.lost);
        CHECK(f.motion.m[0] == 1.0); // a translation
        CHECK(f.motion.m[1] == 0.0);
    }
    const Compensation result = stabilize(*analysis, stabilizeOnly(Smoothing::L1), kW, kH);
    REQUIRE(result.rotation.size() == kShort);
    CHECK(std::all_of(result.rotation.begin(), result.rotation.end(), [](double r) { return r == 0.0; }));
    CHECK(std::all_of(result.scale.begin(), result.scale.end(), [](double s) { return s == Approx(1.0); }));
    const auto points = scenePoints();
    const Stats still = stayPut(stabilized(result, clip.truth, points));
    std::printf("C.15 stabilize: Position, translation jitter +-6 px, L1: stay put RMS %.3f max %.3f px\n", still.rms, still.max);
    CHECK(still.rms < 0.3);
    CHECK(still.max < 1.0);
}

TEST_CASE("Stabilize: cancelling stops reading frames and bad input is refused", "[ai][stabilize]") {
    const Clip& clip = staticShot();
    const auto times = frameTimes(kFrames);
    int reads = 0;
    int calls = 0;
    double last = 0.0;
    const auto cancelled = analyzeMotion(times, readerOf(clip.frames, times, &reads), {}, [&](double fraction) {
        ++calls;
        CHECK(fraction > last);
        last = fraction;
        return fraction < 0.5;
    });
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
    CHECK(calls == 45);
    CHECK(reads == 45);
    std::printf("C.15 stabilize: cancelled at %.0f %% after %d of %zu frames read\n", last * 100.0, reads, kFrames);

    // A reader's error ends the analysis with it.
    const std::vector<Ticks> missing{times[0], times[1], Ticks{times[1].value + 1}};
    CHECK(codeOf(analyzeMotion(missing, readerOf(clip.frames, times))) == ErrorCode::NotFound);
    // Unsorted or repeated times, no times, no reader, pictures of different sizes or broken.
    const std::vector<Ticks> unsorted{times[2], times[1]};
    const std::vector<Ticks> repeated{times[1], times[1]};
    CHECK(codeOf(analyzeMotion(unsorted, readerOf(clip.frames, times))) == ErrorCode::InvalidArgument);
    CHECK(codeOf(analyzeMotion(repeated, readerOf(clip.frames, times))) == ErrorCode::InvalidArgument);
    CHECK(codeOf(analyzeMotion({}, readerOf(clip.frames, times))) == ErrorCode::InvalidArgument);
    CHECK(codeOf(analyzeMotion(times, FrameReader{})) == ErrorCode::InvalidArgument);
    const std::vector<GrayImage> mixed{clip.frames[0], GrayImage(kW / 2, kH / 2)};
    CHECK(codeOf(analyzeMotion(std::span(times).first(2), readerOf(mixed, times))) == ErrorCode::InvalidArgument);
    GrayImage broken(kW, kH);
    broken.px.resize(10);
    const std::vector<GrayImage> brokenFrames{broken};
    CHECK(codeOf(analyzeMotion(std::span(times).first(1), readerOf(brokenFrames, times))) == ErrorCode::InvalidArgument);
    // A single frame is a valid (still) analysis; stabilize() of nothing is empty.
    const auto single = analyzeMotion(std::span(times).first(1), readerOf(clip.frames, times));
    REQUIRE(single);
    CHECK(single->frames.size() == 1);
    CHECK(stabilize(*single, SmoothOptions{}, kW, kH).x.size() == 1);
    CHECK(stabilize(MotionAnalysis{}, SmoothOptions{}, kW, kH).times.empty());
    CHECK(stabilize(*single, SmoothOptions{}, 0, kH).times.empty());
}

TEST_CASE("Stabilize: a blank frame is lost without derailing the path", "[ai][stabilize]") {
    constexpr std::size_t kBlank = 45;
    Clip clip = staticShot();
    clip.frames[kBlank] = GrayImage(kW, kH);
    std::fill(clip.frames[kBlank].px.begin(), clip.frames[kBlank].px.end(), 0.5f);
    const auto times = frameTimes(kFrames);
    const auto analysis = analyzeMotion(times, readerOf(clip.frames, times));
    REQUIRE(analysis);
    CHECK(analysis->frames[kBlank].lost);
    CHECK(analysis->frames[kBlank].motion == Homography{});
    CHECK(std::count_if(analysis->frames.begin(), analysis->frames.end(), [](const FrameMotion& f) { return f.lost; }) == 1);
    const std::array<std::size_t, 1> lost{kBlank};
    const auto points = scenePoints();
    for (const Smoothing smoothing : {Smoothing::Gaussian, Smoothing::L1}) {
        INFO(nameOf(smoothing));
        const SmoothOptions options = stabilizeOnly(smoothing);
        const Positions positions = stabilized(stabilize(*analysis, options, kW, kH), clip.truth, points);
        const Positions ideal =
            stabilized(stabilize(truthAnalysis(clip.truth, times, Method::Similarity, lost), options, kW, kH), clip.truth, points);
        const Stats accuracy = between(positions, ideal, lost);
        const Stats still = stayPut(positions, lost);
        double jump = 0.0; // across the blank frame
        for (std::size_t q = 0; q < points.size(); ++q) {
            jump = std::max(jump, distance(positions[kBlank + 1][q], positions[kBlank - 1][q]));
        }
        std::printf("C.15 stabilize: blank frame %zu of %zu (%s): lost; vs ideal analysis RMS %.3f max %.3f px; stay put RMS %.3f "
                    "max %.3f px; largest move across the blank %.3f px\n",
                    kBlank, kFrames, nameOf(smoothing), accuracy.rms, accuracy.max, still.rms, still.max, jump);
        CHECK(accuracy.rms < 0.3);
        CHECK(accuracy.max < 1.0);
        CHECK(jump < 0.5);
    }
}

TEST_CASE("Stabilize: the analysis JSON round-trips and refuses hostile input", "[ai][stabilize][json]") {
    MotionAnalysis a;
    a.width = 640;
    a.height = 360;
    a.method = Method::Perspective;
    a.frames.push_back(FrameMotion{Ticks{0}, Homography{}, 1.0, false});
    FrameMotion moved{Ticks{kTicksPerSecond / 30}, Homography{}, 0.8333333333333334, false};
    moved.motion.m = {1.0001, -0.0123456789012345, 3.141592653589793, 0.0123, 0.9999, -2.5e-7, 1e-6, -3.3e-7, 1.0};
    a.frames.push_back(moved);
    a.frames.push_back(FrameMotion{Ticks{2 * (kTicksPerSecond / 30)}, Homography{}, 0.0, true});
    const std::string text = toJson(a);
    const auto back = motionAnalysisFromJson(text);
    REQUIRE(back);
    CHECK(*back == a);
    // A real analysis round-trips exactly too.
    const auto& real = staticAnalysis();
    REQUIRE(real);
    const auto realBack = motionAnalysisFromJson(toJson(*real));
    REQUIRE(realBack);
    CHECK(*realBack == *real);
    // Position and an empty frame list.
    a.method = Method::Position;
    a.frames.clear();
    const auto empty = motionAnalysisFromJson(toJson(a));
    REQUIRE(empty);
    CHECK(*empty == a);

    using nlohmann::json;
    const json base = json::parse(text);
    const auto edited = [&](auto edit) {
        json j = base;
        edit(j);
        return j.dump();
    };
    const auto replaced = [&](const std::string& from, const std::string& to) {
        std::string s = text;
        const auto at = s.find(from);
        REQUIRE(at != std::string::npos);
        s.replace(at, from.size(), to);
        return s;
    };
    struct Case {
        std::string text;
        ErrorCode code;
    };
    const std::vector<Case> cases{
        {"", ErrorCode::ParseError},
        {"null", ErrorCode::ParseError},
        {"[]", ErrorCode::ParseError},
        {"{}", ErrorCode::ParseError},
        {text.substr(0, text.size() / 2), ErrorCode::ParseError},
        {replaced("\"c\":1.0", "\"c\":NaN"), ErrorCode::ParseError},
        {replaced("\"m\":[1.0", "\"m\":[1e999"), ErrorCode::ParseError},
        {replaced("\"width\":640", "\"width\":99999999999999999999"), ErrorCode::ParseError},
        {edited([](json& j) { j["schema"] = "wabcut.shots"; }), ErrorCode::ParseError},
        {edited([](json& j) { j["version"] = 2; }), ErrorCode::ParseError},
        {edited([](json& j) { j["version"] = "1"; }), ErrorCode::ParseError},
        {edited([](json& j) { j["width"] = "640"; }), ErrorCode::ParseError},
        {edited([](json& j) { j["width"] = 640.5; }), ErrorCode::ParseError},
        {edited([](json& j) { j["width"] = 0; }), ErrorCode::InvalidArgument},
        {edited([](json& j) { j["height"] = 16385; }), ErrorCode::InvalidArgument},
        {edited([](json& j) { j["width"] = -1; }), ErrorCode::InvalidArgument},
        {edited([](json& j) { j["method"] = "zoom"; }), ErrorCode::ParseError},
        {edited([](json& j) { j["method"] = 1; }), ErrorCode::ParseError},
        {edited([](json& j) { j["frames"] = json::object(); }), ErrorCode::ParseError},
        {edited([](json& j) { j["frames"] = json::array({1}); }), ErrorCode::ParseError},
        {edited([](json& j) { j.erase("frames"); }), ErrorCode::ParseError},
        {edited([](json& j) { j["frames"][1].erase("t"); }), ErrorCode::ParseError},
        {edited([](json& j) { j["frames"][1]["t"] = 1.5; }), ErrorCode::ParseError},
        {edited([](json& j) { j["frames"][1]["t"] = 1e30; }), ErrorCode::ParseError},
        {edited([](json& j) { j["frames"][1]["t"] = 0; }), ErrorCode::InvalidArgument},  // repeated time
        {edited([](json& j) { j["frames"][2]["t"] = 10; }), ErrorCode::InvalidArgument}, // unsorted
        {edited([](json& j) { j["frames"][1]["m"].erase(8); }), ErrorCode::ParseError},
        {edited([](json& j) { j["frames"][1]["m"][3] = "x"; }), ErrorCode::ParseError},
        {edited([](json& j) { j["frames"][1]["m"][8] = 0.0; }), ErrorCode::InvalidArgument},
        {edited([](json& j) { j["frames"][1]["m"][8] = 1e-300; }), ErrorCode::InvalidArgument},
        {edited([](json& j) { j["frames"][1]["c"] = 1.5; }), ErrorCode::InvalidArgument},
        {edited([](json& j) { j["frames"][1]["c"] = -0.1; }), ErrorCode::InvalidArgument},
        {edited([](json& j) { j["frames"][1]["c"] = nullptr; }), ErrorCode::ParseError},
        {edited([](json& j) { j["frames"][1]["lost"] = 0; }), ErrorCode::ParseError},
        {std::string(100000, '['), ErrorCode::InvalidArgument}, // nesting
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        INFO("case " << i << ": " << cases[i].text.substr(0, 200));
        const auto parsed = motionAnalysisFromJson(cases[i].text);
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().code == cases[i].code);
    }
    // Huge counts: one frame too many, and an array too long to build at all.
    std::string tooMany = R"({"schema":"wabcut.stabilize","version":1,"width":640,"height":360,"method":"similarity","frames":[0)";
    tooMany.reserve(tooMany.size() + 2 * kMaxAnalysisFrames + 8);
    for (std::size_t i = 0; i < kMaxAnalysisFrames; ++i) {
        tooMany += ",0";
    }
    tooMany += "]}";
    const auto many = motionAnalysisFromJson(tooMany);
    REQUIRE_FALSE(many);
    CHECK(many.error().code == ErrorCode::InvalidArgument);
    std::string flood = "[0";
    flood.reserve(12'000'000);
    while (flood.size() < 11'000'000) {
        flood += ",0";
    }
    flood += "]";
    const auto flooded = motionAnalysisFromJson(flood);
    REQUIRE_FALSE(flooded);
    CHECK(flooded.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Stabilize: auto scale hides every border and crop edges bound what is uncovered", "[ai][stabilize][framing]") {
    constexpr std::size_t kShort = 60;
    const auto times = frameTimes(kShort);
    const MotionAnalysis analysis = truthAnalysis(shakyCamera(kShort, 2024, 0.0, 10.0, 2.0), times, Method::Similarity);
    SmoothOptions options; // Crop, auto scale; maximum 150 %
    const Compensation c = stabilize(analysis, options, kW, kH);
    REQUIRE(c.x.size() == kShort);
    CHECK(c.autoScale > 1.0);
    CHECK(c.autoScale < 1.5);
    CHECK_FALSE(c.scaleCapped);
    bool allCovered = true;
    bool smallerShowsBorder = false;
    for (std::size_t i = 0; i < kShort; ++i) {
        const Homography k = compensationAt(c, i, kW, kH);
        CHECK(c.scale[i] == Approx(c.autoScale).epsilon(1e-9)); // the jitter has no zoom of its own
        allCovered = allCovered && coversFrame(k);
        smallerShowsBorder = smallerShowsBorder || !coversFrame(scaledAboutCentre(k, 0.995));
    }
    CHECK(allCovered);
    CHECK(smallerShowsBorder);
    std::printf("C.15 stabilize: framing, jitter +-10 px +-2 deg: auto scale %.4f (maximum 1.5); crop edges L %.3f T %.3f R %.3f "
                "B %.3f of the frame\n",
                c.autoScale, c.cropLeft, c.cropTop, c.cropRight, c.cropBottom);

    // The cap: borders may show, and the result says so.
    options.maxScale = 1.01;
    const Compensation capped = stabilize(analysis, options, kW, kH);
    CHECK(capped.scaleCapped);
    CHECK(capped.autoScale == Approx(1.01));
    CHECK(capped.scale.front() == Approx(1.01));

    // Crop: no scale; every frame covers the frame minus the crop edges.
    options.maxScale = 1.5;
    options.framing = Framing::Crop;
    const Compensation crop = stabilize(analysis, options, kW, kH);
    CHECK(crop.autoScale == Approx(c.autoScale));
    CHECK(crop.cropLeft > 0.0);
    CHECK(crop.cropTop > 0.0);
    CHECK(crop.cropRight > 0.0);
    CHECK(crop.cropBottom > 0.0);
    bool cropCovered = true;
    for (std::size_t i = 0; i < kShort; ++i) {
        CHECK(crop.scale[i] == Approx(1.0));
        cropCovered = cropCovered && coversRect(compensationAt(crop, i, kW, kH), -0.5 + crop.cropLeft * kW, -0.5 + crop.cropTop * kH,
                                                kW - 0.5 - crop.cropRight * kW, kH - 0.5 - crop.cropBottom * kH);
    }
    CHECK(cropCovered);

    // Stabilize only, with an additional scale.
    options.framing = Framing::StabilizeOnly;
    options.additionalScale = 1.2;
    const Compensation only = stabilize(analysis, options, kW, kH);
    CHECK(only.scale.front() == Approx(1.2));
    CHECK(only.autoScale == Approx(c.autoScale));

    // A perspective corner pin hides its borders too.
    const MotionAnalysis perspective = truthAnalysis(projectiveJitter(kShort, 11, 6.0), times, Method::Perspective);
    const Compensation pin = stabilize(perspective, SmoothOptions{}, kW, kH);
    REQUIRE(pin.corners.size() == kShort);
    CHECK(pin.autoScale > 1.0);
    CHECK_FALSE(pin.scaleCapped);
    bool pinCovered = true;
    for (std::size_t i = 0; i < kShort; ++i) {
        pinCovered = pinCovered && coversFrame(compensationAt(pin, i, kW, kH));
    }
    CHECK(pinCovered);

    // Source pictures larger than the analysis: lengths scale with them.
    options = SmoothOptions{};
    options.framing = Framing::StabilizeOnly;
    const Compensation small = stabilize(analysis, options, kW, kH);
    const Compensation large = stabilize(analysis, options, 4 * kW, 4 * kH);
    REQUIRE(large.x.size() == kShort);
    for (std::size_t i = 0; i < kShort; ++i) {
        CHECK(large.x[i] == Approx(4.0 * small.x[i]).margin(1e-6));
        CHECK(large.y[i] == Approx(4.0 * small.y[i]).margin(1e-6));
        CHECK(large.rotation[i] == Approx(small.rotation[i]).margin(1e-9));
    }
}

TEST_CASE("Stabilize: homographies fit robustly and compose", "[ai][stabilize]") {
    const auto corners = frameCorners();
    const std::array<Point2, 4> moved{{{-8.0, 3.0}, {kW + 5.0, -6.0}, {kW - 9.0, kH + 7.0}, {4.0, kH - 2.0}}};
    const Homography truth = homographyFromCorners(corners, moved).value();
    for (std::size_t k = 0; k < 4; ++k) {
        CHECK(distance(truth.map(corners[k]), moved[k]) < 1e-9);
    }
    std::mt19937 rng(17);
    std::uniform_real_distribution<double> ux(0.0, kW);
    std::uniform_real_distribution<double> uy(0.0, kH);
    std::vector<Point2> a;
    std::vector<Point2> b;
    for (int i = 0; i < 120; ++i) {
        const Point2 p{ux(rng), uy(rng)};
        a.push_back(p);
        if (i % 4 == 0) { // a quarter are wrong
            const double x = ux(rng);
            const double y = uy(rng);
            b.push_back({x, y});
        } else {
            b.push_back(truth.map(p));
        }
    }
    std::vector<char> inliers;
    const auto fit = fitHomography(a, b, 0.5, 500, &inliers);
    REQUIRE(fit);
    CHECK(cornerError(*fit, truth) < 1e-6);
    CHECK(std::count(inliers.begin(), inliers.end(), char{1}) == 90);
    CHECK_FALSE(fitHomography(std::span(a).first(7), std::span(b).first(7), 0.5, 100));
    // Algebra: inverse, composition, from an affine motion; collinear corners have no homography.
    CHECK(cornerError(truth.inverted().value().after(truth), Homography{}) < 1e-9);
    const masks::Motion motion = masks::test::similarity(3.0, 1.1, 4.0, -2.0, kCx, kCy);
    const Homography fromMotion = Homography::fromMotion(motion);
    CHECK(distance(fromMotion.map({10.0, 20.0}), motion.map({10.0, 20.0})) < 1e-12);
    CHECK(cornerError(fromMotion.after(truth), Homography::fromMotion(motion).after(truth)) < 1e-12);
    const std::array<Point2, 4> line{{{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}, {5.0, 0.0}}};
    CHECK_FALSE(homographyFromCorners(line, corners));
    CHECK_FALSE(Homography{{0, 0, 0, 0, 0, 0, 0, 0, 1}}.inverted());
}

namespace {

/// A rolling-shutter camera's picture of a scene that moved (vx, vy) px per frame: line y shows it moved further by
/// share * ((y + 0.5) / h - 0.5) of that (texture -> picture, after the frame's own motion).
Homography rollingSkew(double share, double vx, double vy) {
    const double kx = share * vx / kH;
    const double ky = share * vy / kH;
    const double offset = 0.5 - 0.5 * kH;
    Homography s;
    s.m = {1.0, kx, kx * offset, 0.0, 1.0 + ky, ky * offset, 0.0, 0.0, 1.0};
    return s;
}

/// A pan that speeds up from rest to about 8 px a frame over 60 frames, with some jitter, shot with a rolling shutter
/// whose scan takes `share` of the frame (texture -> picture of each frame). `rollPerPixel`: the camera also rolls
/// with the pan (degrees per px/frame of motion), which shears no line but must not be taken for the scan's skew.
std::vector<Homography> acceleratingPan(double share, double rollPerPixel = 0.0) {
    constexpr std::size_t n = 60;
    std::vector<double> x(n), y(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto k = static_cast<double>(i);
        x[i] = 0.07 * k * k + 1.5 * std::sin(1.3 * k);
        y[i] = std::sin(0.9 * k + 0.3);
    }
    std::vector<Homography> out;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t a = i > 0 ? i - 1 : i;
        const std::size_t b = i + 1 < n ? i + 1 : i;
        const double steps = static_cast<double>(b - a);
        const double vx = (x[b] - x[a]) / steps;
        const Homography move = Homography::fromMotion(masks::test::similarity(rollPerPixel * vx, 1.0, x[i], y[i], kCx, kCy));
        out.push_back(rollingSkew(share, vx, (y[b] - y[a]) / steps).after(move));
    }
    return out;
}

/// The largest horizontal slant of the stabilized frames: how far apart (x) two scene points one above the other
/// (120 px apart) end up, which a picture without skew shows as 0.
double worstSlant(const Compensation& c, const std::vector<Homography>& truth) {
    double worst = 0.0;
    for (std::size_t i = 0; i < truth.size(); ++i) {
        const Homography k = compensationAt(c, i, kW, kH);
        for (const double x : {60.0, 160.0, 260.0}) {
            const Point2 top = k.map(truth[i].map({x, 30.0}));
            const Point2 bottom = k.map(truth[i].map({x, 150.0}));
            worst = std::max(worst, std::abs(top.x - bottom.x));
        }
    }
    return worst;
}

} // namespace

TEST_CASE("Stabilize: rolling shutter ripple is measured and taken out", "[ai][stabilize][rollingShutter]") {
    const auto times = frameTimes(60);
    // A global shutter: nothing to measure, nothing to take out.
    const std::vector<Homography> global = acceleratingPan(0.0);
    const MotionAnalysis still = truthAnalysis(global, times, Method::Perspective);
    for (const RippleReduction mode : {RippleReduction::Automatic, RippleReduction::Enhanced}) {
        const std::vector<double> shares = rollingShutterShares(still, mode, 30.0);
        REQUIRE(shares.size() == 60);
        CHECK(*std::max_element(shares.begin(), shares.end()) == 0.0);
    }
    // A scan of half the frame time: measured, per clip and per frame.
    const std::vector<Homography> rolling = acceleratingPan(0.5);
    const MotionAnalysis skewed = truthAnalysis(rolling, times, Method::Perspective);
    const std::vector<double> clipShares = rollingShutterShares(skewed, RippleReduction::Automatic, 30.0);
    REQUIRE(clipShares.size() == 60);
    CHECK(clipShares.front() == Approx(0.5).margin(0.05));
    CHECK(clipShares.back() == clipShares.front());
    const std::vector<double> frameShares = rollingShutterShares(skewed, RippleReduction::Enhanced, 30.0);
    REQUIRE(frameShares.size() == 60);
    CHECK(frameShares[30] == Approx(0.5).margin(0.05));
    CHECK(rollingShutterShares(skewed, RippleReduction::None, 30.0).empty());
    // A camera that rolls as it pans (0.15 degrees per px/frame, 1.2 degrees at full speed) is not mistaken for a
    // scan: the same share comes out.
    const MotionAnalysis rolled = truthAnalysis(acceleratingPan(0.5, 0.15), times, Method::Perspective);
    CHECK(rollingShutterShares(rolled, RippleReduction::Automatic, 30.0).front() == Approx(0.5).margin(0.05));
    CHECK(rollingShutterShares(truthAnalysis(acceleratingPan(0.0, 0.15), times, Method::Perspective), RippleReduction::Automatic,
                               30.0)
              .front() == 0.0);
    MotionAnalysis similarity = skewed;
    similarity.method = Method::Similarity;
    CHECK(rollingShutterShares(similarity, RippleReduction::Automatic, 30.0).empty());
    // Stabilized with the reduction, vertical lines stand up straight; without, the faster pan leans them over.
    SmoothOptions options = stabilizeOnly(Smoothing::Gaussian);
    const Compensation leaning = stabilize(skewed, options, kW, kH);
    options.rippleReduction = RippleReduction::Automatic;
    const Compensation straight = stabilize(skewed, options, kW, kH);
    CHECK(straight.rollingShutterShare == Approx(0.5).margin(0.05));
    CHECK(leaning.rollingShutterShare == 0.0);
    const double before = worstSlant(leaning, rolling);
    const double after = worstSlant(straight, rolling);
    std::printf("P.2 rolling shutter: scan share measured %.3f (true 0.5); worst slant %.2f px -> %.2f px\n",
                straight.rollingShutterShare, before, after);
    CHECK(before > 1.5);
    CHECK(after < 0.3);
    // On global-shutter footage the reduction changes nothing.
    SmoothOptions plain = stabilizeOnly(Smoothing::Gaussian);
    const Compensation a = stabilize(still, plain, kW, kH);
    plain.rippleReduction = RippleReduction::Enhanced;
    const Compensation b = stabilize(still, plain, kW, kH);
    REQUIRE(a.corners.size() == b.corners.size());
    for (std::size_t i = 0; i < a.corners.size(); ++i) {
        CHECK(a.corners[i] == b.corners[i]);
    }
}
TEST_CASE("Stabilize: analysis speed at the analysis width", "[ai][stabilize][timing]") {
    // 640 x 360 pictures (kAnalysisWidth for 16:9 footage) of a shaky pan; the pictures are made before timing.
    constexpr int w = 640;
    constexpr int h = 360;
    constexpr std::size_t n = 48;
    // Crops of one larger picture at whole-pixel offsets: a 3 px/frame pan with up to 6 px of jitter.
    const GrayImage scene = masks::test::render(masks::test::Texture(77), w + 160, h + 16, masks::Motion{1.0, 0.0, 0.0, 0.0, 1.0, 0.0});
    std::vector<GrayImage> frames;
    std::mt19937 rng(5);
    std::uniform_int_distribution<int> jitter(0, 6);
    for (std::size_t i = 0; i < n; ++i) {
        const int ox = 3 * static_cast<int>(i) + jitter(rng);
        const int oy = jitter(rng);
        GrayImage frame(w, h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                frame.px[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] =
                    scene.px[static_cast<std::size_t>(y + oy) * (w + 160) + static_cast<std::size_t>(x + ox)];
            }
        }
        frames.push_back(std::move(frame));
    }
    const std::vector<Ticks> times = frameTimes(n);
    for (const Method method : {Method::Similarity, Method::Perspective}) {
        for (const bool detailed : {false, true}) {
            AnalyzeOptions options;
            options.method = method;
            options.detailed = detailed;
            const auto start = std::chrono::steady_clock::now();
            const auto analysis = analyzeMotion(times, readerOf(frames, times), options);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            REQUIRE(analysis);
            std::printf("P.2 stabilize: analysis of %zu frames at %d x %d, %s%s: %.1f ms per frame (%.0f frames/s)\n", n, w, h,
                        method == Method::Similarity ? "Position Scale Rotation" : "Perspective", detailed ? ", detailed" : "",
                        1000.0 * seconds / static_cast<double>(n), static_cast<double>(n) / seconds);
            int lost = 0;
            for (const FrameMotion& f : analysis->frames) {
                lost += f.lost ? 1 : 0;
            }
            CHECK(lost == 0);
        }
    }
}