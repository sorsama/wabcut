// The region tracker (wabcut/masks/Tracker.h) on synthetic moving textures with exact sub-pixel truth: translation to
// a few hundredths of a pixel, similarity and affine motion, loss detection when the picture changes, backward
// tracking undoing forward tracking, RANSAC with outliers, and the tracker's speed (perf).

#include "SyntheticMotion.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace wabcut::masks;
using namespace wabcut::masks::test;
using Catch::Approx;

namespace {

constexpr int kW = 320;
constexpr int kH = 240;

/// Frame k shows the content moved by motion(k) (frame 0 -> frame k).
template <class MotionAt>
std::vector<GrayImage> clip(const Texture& texture, int frames, MotionAt motionAt) {
    std::vector<GrayImage> out;
    for (int k = 0; k < frames; ++k) {
        out.push_back(render(texture, kW, kH, *motionAt(k).inverted()));
    }
    return out;
}

} // namespace

TEST_CASE("Tracker: a translating texture is followed to a few hundredths of a pixel", "[masks][tracker]") {
    const Texture texture(7);
    const double vx = 0.37, vy = -0.23;
    const auto frames = clip(texture, 31, [&](int k) { return Motion{1, 0, vx * k, 0, 1, vy * k}; });
    TrackerOptions options;
    options.model = MotionModel::Translation;
    RegionTracker tracker(options);
    tracker.start(frames[0], box(110, 80, 210, 160));
    double worstStep = 0.0;
    for (int k = 1; k < 31; ++k) {
        const TrackStep step = tracker.step(frames[static_cast<std::size_t>(k)]);
        REQUIRE_FALSE(step.lost);
        CHECK(step.confidence > 0.8);
        worstStep = std::max({worstStep, std::abs(step.motion.c - vx), std::abs(step.motion.f - vy)});
    }
    WARN("worst per-frame error " << worstStep << " px; total after 30 frames (" << tracker.total().c << ", " << tracker.total().f
                                  << ") vs (" << 30 * vx << ", " << 30 * vy << ")");
    CHECK(worstStep < 0.03);
    CHECK(tracker.total().c == Approx(30 * vx).margin(0.15));
    CHECK(tracker.total().f == Approx(30 * vy).margin(0.15));
}

TEST_CASE("Tracker: similarity and affine motion of a region", "[masks][tracker]") {
    const Texture texture(11);
    SECTION("turning, growing and moving") {
        const auto truth = [](int k) { return similarity(0.6 * k, std::pow(1.006, k), 0.8 * k, 0.35 * k, 160, 120); };
        const auto frames = clip(texture, 21, truth);
        RegionTracker tracker; // similarity
        tracker.start(frames[0], box(100, 70, 220, 170));
        for (int k = 1; k < 21; ++k) {
            const TrackStep step = tracker.step(frames[static_cast<std::size_t>(k)]);
            REQUIRE_FALSE(step.lost);
        }
        const double error = cornerError(tracker.total(), truth(20), 100, 70, 220, 170);
        WARN("similarity: corner error after 20 frames " << error << " px");
        CHECK(error < 0.6);
    }
    SECTION("an affine shear") {
        const auto truth = [](int k) { return Motion{1.0 + 0.004 * k, 0.006 * k, 0.5 * k, -0.003 * k, 1.0 - 0.002 * k, 0.2 * k}; };
        const auto frames = clip(texture, 16, truth);
        TrackerOptions options;
        options.model = MotionModel::Affine;
        RegionTracker tracker(options);
        tracker.start(frames[0], box(90, 60, 230, 180));
        for (int k = 1; k < 16; ++k) {
            REQUIRE_FALSE(tracker.step(frames[static_cast<std::size_t>(k)]).lost);
        }
        const double error = cornerError(tracker.total(), truth(15), 90, 60, 230, 180);
        WARN("affine: corner error after 15 frames " << error << " px");
        CHECK(error < 0.6);
    }
}

TEST_CASE("Tracker: a cut to another picture is detected as lost; the tracker stays put", "[masks][tracker]") {
    const Texture texture(3);
    const Texture other(99);
    auto frames = clip(texture, 10, [](int k) { return Motion{1, 0, 0.5 * k, 0, 1, 0.2 * k}; });
    frames.push_back(render(other, kW, kH, Motion{}));
    RegionTracker tracker;
    tracker.start(frames[0], box(100, 70, 220, 170));
    for (int k = 1; k < 10; ++k) {
        const TrackStep step = tracker.step(frames[static_cast<std::size_t>(k)]);
        REQUIRE_FALSE(step.lost);
        CHECK(step.confidence > 0.8);
    }
    const Motion before = tracker.total();
    const TrackStep cut = tracker.step(frames.back());
    WARN("confidence at the cut " << cut.confidence << " (" << cut.inliers << " of " << cut.features << " features agree)");
    CHECK(cut.lost);
    CHECK(cut.confidence < 0.3);
    CHECK(tracker.total().c == before.c);
    // Back on the right picture it goes on from where it was.
    const TrackStep resumed = tracker.step(render(texture, kW, kH, *Motion{1, 0, 0.5 * 10, 0, 1, 0.2 * 10}.inverted()));
    CHECK_FALSE(resumed.lost);
    CHECK(tracker.total().c == Approx(5.0).margin(0.15));
}

TEST_CASE("Tracker: tracking back over the same frames undoes tracking forward", "[masks][tracker]") {
    const Texture texture(21);
    const auto truth = [](int k) { return similarity(-0.4 * k, std::pow(0.995, k), 0.6 * k, -0.45 * k, 160, 120); };
    const auto frames = clip(texture, 21, truth);
    RegionTracker forward;
    forward.start(frames[0], box(105, 75, 215, 165));
    for (int k = 1; k <= 20; ++k) {
        REQUIRE_FALSE(forward.step(frames[static_cast<std::size_t>(k)]).lost);
    }
    RegionTracker backward;
    backward.start(frames[20], forward.region());
    for (int k = 19; k >= 0; --k) {
        REQUIRE_FALSE(backward.step(frames[static_cast<std::size_t>(k)]).lost);
    }
    const Motion roundTrip = backward.total().after(forward.total());
    const double error = cornerError(roundTrip, Motion{}, 105, 75, 215, 165);
    WARN("forward then backward over 20 frames: corner error " << error << " px");
    CHECK(error < 0.5);
    const Motion inverse = *forward.total().inverted();
    CHECK(cornerError(backward.total(), inverse, 105, 75, 215, 165) < 0.5);
}

TEST_CASE("Tracker: RANSAC finds the motion among outliers; pieces behave", "[masks][tracker]") {
    const Motion truth = similarity(12.0, 1.1, 3.0, -2.0, 50, 40);
    std::vector<Point2> a;
    std::vector<Point2> b;
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> u(0.0, 100.0);
    for (int i = 0; i < 60; ++i) {
        const Point2 p{u(rng), u(rng)};
        a.push_back(p);
        b.push_back(i % 3 == 0 ? Point2{u(rng), u(rng)} : truth.map(p)); // a third are wrong
    }
    std::vector<char> inliers;
    const auto fit = fitMotion(a, b, MotionModel::Similarity, 0.5, 300, &inliers);
    REQUIRE(fit);
    CHECK(cornerError(*fit, truth, 0, 0, 100, 100) < 1e-6);
    CHECK(std::count(inliers.begin(), inliers.end(), 1) == 40);
    CHECK_FALSE(fitMotion(std::span(a).first(1), std::span(b).first(1), MotionModel::Similarity, 0.5, 10));
    // A flat picture has no features; a composed motion and its inverse cancel.
    CHECK(goodFeatures(GrayImage(64, 64), {}, 50, 0.01, 4.0).empty());
    CHECK(cornerError(truth.inverted()->after(truth), Motion{}, 0, 0, 100, 100) < 1e-9);
    CHECK(insidePolygon(box(0, 0, 10, 10), {5, 5}));
    CHECK_FALSE(insidePolygon(box(0, 0, 10, 10), {15, 5}));
}

// Codex wave-4 P2-37 (hardening): Lucas-Kanade never converts an out-of-range coordinate to int (undefined behaviour past
// INT_MAX) and gives up on a point whose window leaves the picture or whose guess is not a number, instead of reporting
// it tracked. Points far outside the picture, at the int limits and NaN come back "not ok"; a point inside is unaffected.
// Before the fix this crashed (access violation reading far outside the image).
TEST_CASE("Tracker: points far outside the picture or not a number are lost, never undefined", "[masks][tracker]") {
    const Texture texture(11);
    const GrayImage a = render(texture, 160, 90, Motion{});
    const GrayImage b = render(texture, 160, 90, Motion{1, 0, 0.6, 0, 1, -0.4});
    const auto from = buildPyramid(a, 3);
    const auto to = buildPyramid(b, 3);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<Point2> points{{80.0, 45.0}, {1e15, -1e15}, {3e9, 45.0}, {80.0, -3e9}, {nan, 45.0}, {-500.0, 40.0}};
    const std::vector<PointTrack> tracked = trackPoints(from, to, points, TrackerOptions{});
    REQUIRE(tracked.size() == points.size());
    CHECK(tracked[0].ok);
    CHECK(tracked[0].to.x == Approx(79.4).margin(0.1)); // the content moved by (-0.6, +0.4)
    for (std::size_t i = 1; i < points.size(); ++i) {
        INFO("point " << i);
        CHECK_FALSE(tracked[i].ok);
    }
}

TEST_CASE("Tracker: speed at a 640x360 working size", "[masks][tracker][perf]") {
    const Texture texture(5);
    constexpr int w = 640, h = 360;
    std::vector<GrayImage> frames;
    for (int k = 0; k < 61; ++k) {
        frames.push_back(render(texture, w, h, *similarity(0.2 * k, 1.0, 1.1 * k, 0.4 * k, 320, 180).inverted()));
    }
    const auto timeOf = [&](MotionModel model) {
        TrackerOptions options;
        options.model = model;
        RegionTracker tracker(options);
        tracker.start(frames[0], box(220, 110, 420, 250));
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 1; k <= 60; ++k) {
            REQUIRE_FALSE(tracker.step(frames[static_cast<std::size_t>(k)]).lost);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return 60.0 / seconds;
    };
    const double translation = timeOf(MotionModel::Translation);
    const double sim = timeOf(MotionModel::Similarity);
    const double affine = timeOf(MotionModel::Affine);
    WARN("tracker fps at 640x360, 200x140 region: translation " << translation << ", similarity " << sim << ", affine " << affine);
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "masks";
    std::filesystem::create_directories(dir);
    std::ofstream report(dir / "tracker-perf.md", std::ios::trunc);
    report << "# Tracker speed (640x360 working pictures, a 200x140 region, 60 frames, one thread)\n\n"
           << "| model | frames per second |\n|---|---|\n"
           << "| translation | " << translation << " |\n| similarity | " << sim << " |\n| affine | " << affine << " |\n";
    CHECK(sim > 25.0); // faster than real time for 25 fps material
}
