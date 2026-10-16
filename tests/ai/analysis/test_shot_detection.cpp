// Shot detection on synthetic frames (C.5): frame-exact hard cuts, flashes ignored, fades, dissolves, steady motion,
// the individual detectors and the JSON form.

#include "AnalysisTestUtil.h"

#include <wabcut/ai/analysis/AnalysisJson.h>
#include <wabcut/ai/analysis/ShotDetection.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai::analysis;
using namespace wabcut::ai::analysis::test;

namespace {

ShotAnalysis run(std::vector<AnalysisFrame> frames, ShotOptions options = {}) {
    VectorFrameSource source(std::move(frames));
    auto result = detectShots(source, options);
    REQUIRE(result);
    return *result;
}

/// "frame:kind" for each cut.
std::vector<std::string> summary(const ShotAnalysis& a) {
    std::vector<std::string> out;
    for (const ShotCut& c : a.cuts) {
        out.push_back(std::to_string(c.frame) + ":" + std::string(toString(c.kind)));
    }
    return out;
}

} // namespace

TEST_CASE("Hard cuts are found on the exact frame", "[ai][analysis][shots]") {
    std::vector<AnalysisFrame> frames;
    for (std::int64_t n = 0; n < 120; ++n) {
        frames.push_back(sceneFrame(n < 30 ? 1 : n < 75 ? 2 : 3, n));
    }
    const ShotAnalysis a = run(frames);
    CHECK(summary(a) == std::vector<std::string>{"30:cut", "75:cut"});
    CHECK(a.cuts[0].time == framesToTicks(30, kFps));
    CHECK(a.cuts[0].confidence > 0.6f);
    CHECK(a.frames == 120);
    CHECK(a.end == framesToTicks(120, kFps));

    for (const ShotDetector d : {ShotDetector::Content, ShotDetector::Adaptive}) {
        INFO(toString(d));
        ShotOptions o;
        o.detector = d;
        o.contentThreshold = 15.0; // the synthetic scenes differ less than real shots
        CHECK(summary(run(frames, o)) == std::vector<std::string>{"30:cut", "75:cut"});
    }
    ShotOptions threshold;
    threshold.detector = ShotDetector::Threshold;
    CHECK(run(frames, threshold).cuts.empty()); // no fades here
}

TEST_CASE("Flashes are not cuts", "[ai][analysis][shots]") {
    std::vector<AnalysisFrame> frames;
    for (std::int64_t n = 0; n < 100; ++n) {
        AnalysisFrame f = sceneFrame(n < 80 ? 4 : 5, n);
        if (n == 20 || n == 21 || n == 50) {
            f = flat(n, 255); // white flashes (2 frames, then 1)
        }
        frames.push_back(std::move(f));
    }
    CHECK(summary(run(frames)) == std::vector<std::string>{"80:cut"});
    ShotOptions content;
    content.detector = ShotDetector::Content;
    CHECK(summary(run(frames, content)) == std::vector<std::string>{"80:cut"});
    ShotOptions noFilter;
    noFilter.flashFrames = 0;
    noFilter.minShotFrames = 1;
    CHECK(run(frames, noFilter).cuts.size() > 1); // without the flash filter they would be
}

TEST_CASE("A fade through black is one cut between the fades", "[ai][analysis][shots]") {
    std::vector<AnalysisFrame> frames;
    for (std::int64_t n = 0; n < 100; ++n) {
        if (n < 30) {
            frames.push_back(sceneFrame(6, n));
        } else if (n < 42) {
            frames.push_back(scaled(sceneFrame(6, n), 1.0 - static_cast<double>(n - 29) / 12.0)); // fade out
        } else if (n < 50) {
            frames.push_back(flat(n, 0));
        } else if (n < 62) {
            frames.push_back(scaled(sceneFrame(7, n), static_cast<double>(n - 49) / 12.0)); // fade in
        } else {
            frames.push_back(sceneFrame(7, n));
        }
    }
    const ShotAnalysis a = run(frames);
    REQUIRE(a.cuts.size() == 1);
    CHECK(a.cuts[0].kind == CutKind::Fade);
    CHECK(a.cuts[0].frame >= 42);
    CHECK(a.cuts[0].frame <= 52);
    ShotOptions threshold;
    threshold.detector = ShotDetector::Threshold;
    CHECK(run(frames, threshold).cuts.size() == 1);

    // Starting from black is not a cut.
    std::vector<AnalysisFrame> opening;
    for (std::int64_t n = 0; n < 60; ++n) {
        opening.push_back(n < 10 ? flat(n, 0) : n < 20 ? scaled(sceneFrame(8, n), static_cast<double>(n - 9) / 10.0) : sceneFrame(8, n));
    }
    CHECK(run(opening).cuts.empty());
}

TEST_CASE("A dissolve is one cut half way through it", "[ai][analysis][shots]") {
    std::vector<AnalysisFrame> frames;
    for (std::int64_t n = 0; n < 110; ++n) {
        if (n < 40) {
            frames.push_back(sceneFrame(9, n));
        } else if (n < 65) {
            frames.push_back(mixed(sceneFrame(9, n), sceneFrame(10, n), static_cast<double>(n - 39) / 26.0));
        } else {
            frames.push_back(sceneFrame(10, n));
        }
    }
    const ShotAnalysis a = run(frames);
    REQUIRE(a.cuts.size() == 1);
    CHECK(a.cuts[0].kind == CutKind::Dissolve);
    CHECK(a.cuts[0].frame >= 46);
    CHECK(a.cuts[0].frame <= 58);
    ShotOptions content;
    content.detector = ShotDetector::Content;
    CHECK(run(frames, content).cuts.empty()); // ContentDetector alone misses gradual transitions
}

TEST_CASE("Steady motion is not a cut; cuts closer than the minimum shot keep the strongest", "[ai][analysis][shots]") {
    std::vector<AnalysisFrame> moving;
    for (std::int64_t n = 0; n < 80; ++n) {
        moving.push_back(sceneFrame(11, n, 6)); // the square moves 6 px a frame
    }
    CHECK(run(moving).cuts.empty());

    std::vector<AnalysisFrame> quick;
    for (std::int64_t n = 0; n < 60; ++n) {
        quick.push_back(sceneFrame(n < 20 ? 12 : n < 25 ? 13 : 14, n)); // a 5-frame shot
    }
    ShotOptions o;
    o.flashFrames = 0;
    CHECK(run(quick, o).cuts.size() == 1);
    o.minShotFrames = 3;
    CHECK(summary(run(quick, o)) == std::vector<std::string>{"20:cut", "25:cut"});
}

TEST_CASE("Shot results round-trip through JSON; bad input is refused", "[ai][analysis][shots]") {
    std::vector<AnalysisFrame> frames;
    for (std::int64_t n = 0; n < 60; ++n) {
        frames.push_back(sceneFrame(n < 30 ? 1 : 2, n));
    }
    const ShotAnalysis a = run(frames);
    const auto back = shotsFromJson(toJson(a));
    REQUIRE(back);
    CHECK(*back == a);
    CHECK(run(frames) == a); // deterministic
    CHECK_FALSE(shotsFromJson("{\"schema\":\"wabcut.beats\",\"version\":1}"));
    CHECK_FALSE(shotsFromJson("not json"));

    frames[10].width = 80; // a frame of another size
    VectorFrameSource source(std::move(frames));
    CHECK_FALSE(detectShots(source));

    std::vector<AnalysisFrame> some;
    for (std::int64_t n = 0; n < 60; ++n) {
        some.push_back(sceneFrame(1, n));
    }
    VectorFrameSource stopped(std::move(some));
    const auto cancelled = detectShots(stopped, {}, [](double) { return false; });
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
    CHECK(shotDetectorFromString("adaptive") == ShotDetector::Adaptive);
    CHECK_FALSE(shotDetectorFromString("nope"));
}
