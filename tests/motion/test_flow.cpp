// Dense optical flow and flow interpolation (P.2, wabcut_motion) on synthetic moving textures with exact truth.

#include "MovingTexture.h"

#include <wabcut/motion/Flow.h>
#include <wabcut/motion/Interpolate.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

using namespace wabcut::motion;
using wabcut::motion::test::MovingTexture;
using wabcut::motion::test::rgbaOf;

namespace {

/// Mean end-point error of `flow` against a constant (u, v), away from the borders.
double meanEndPointError(const FlowField& flow, double u, double v) {
    double sum = 0.0;
    int n = 0;
    for (int y = flow.height / 8; y < flow.height - flow.height / 8; ++y) {
        for (int x = flow.width / 8; x < flow.width - flow.width / 8; ++x) {
            sum += std::hypot(flow.u(x, y) - u, flow.v(x, y) - v);
            ++n;
        }
    }
    return n > 0 ? sum / n : 0.0;
}

} // namespace

TEST_CASE("Optical flow recovers a sub-pixel translation", "[motion][flow]") {
    const MovingTexture texture(7);
    const Plane a = texture.picture(320, 180, 0.0, 0.0);
    const Plane b = texture.picture(320, 180, 3.3, -1.7);
    const FlowField flow = estimateFlow(a, b);
    REQUIRE(flow.width == 320);
    REQUIRE(flow.height == 180);
    CHECK(meanEndPointError(flow, 3.3, -1.7) < 0.1);
}

TEST_CASE("Optical flow finds a large translation coarse to fine", "[motion][flow]") {
    const MovingTexture texture(11);
    const Plane a = texture.picture(640, 360, 0.0, 0.0);
    const Plane b = texture.picture(640, 360, 25.0, 12.0);
    const FlowField flow = estimateFlow(a, b, analysisFlowOptions());
    CHECK(meanEndPointError(flow, 25.0, 12.0) < 0.25);
}

TEST_CASE("Optical flow both ways agrees with itself", "[motion][flow]") {
    const MovingTexture texture(3);
    const Plane a = texture.picture(480, 270, 0.0, 0.0);
    const Plane b = texture.picture(480, 270, -6.0, 4.5);
    const FlowPair pair = estimateFlowPair(a, b, analysisFlowOptions());
    REQUIRE_FALSE(pair.empty());
    // finestLevel 1: the pair is half the pictures' size.
    CHECK(pair.width == 240);
    CHECK(pair.height == 135);
    // In fractions: forward about (-6, 4.5) px of 480 x 270, backward the opposite.
    const std::array<float, 4> mid = sampleFlow(pair, 0.5f, 0.5f);
    CHECK(mid[0] * 480.0f == Catch::Approx(-6.0).margin(0.15));
    CHECK(mid[1] * 270.0f == Catch::Approx(4.5).margin(0.15));
    CHECK(mid[2] * 480.0f == Catch::Approx(6.0).margin(0.15));
    CHECK(mid[3] * 270.0f == Catch::Approx(-4.5).margin(0.15));
    CHECK(meanConsistencyError(pair) < 0.2);
}

TEST_CASE("Optical flow interpolation beats frame blending which beats frame sampling", "[motion][interpolation]") {
    const MovingTexture texture(7);
    const int w = 640;
    const int h = 360;
    // The texture moves 9 px right and 4 px up between A and B; the true in-between picture is half way.
    const RgbaImage a = rgbaOf(texture.picture(w, h, 0.0, 0.0));
    const RgbaImage b = rgbaOf(texture.picture(w, h, 9.0, -4.0));
    const RgbaImage truth = rgbaOf(texture.picture(w, h, 4.5, -2.0));
    const FlowPair pair = estimateFlowPair(texture.picture(w, h, 0.0, 0.0), texture.picture(w, h, 9.0, -4.0),
                                           analysisFlowOptions());
    const double sampling = psnr(truth, a, 0.05);
    const double blending = psnr(truth, blend(a, b, 0.5), 0.05);
    const double flow = psnr(truth, interpolate(a, b, pair, 0.5), 0.05);
    INFO("sampling " << sampling << " dB, blending " << blending << " dB, optical flow " << flow << " dB");
    CHECK(blending > sampling + 3.0);
    CHECK(flow > blending + 15.0);
    CHECK(flow > 50.0);
    // At the ends the interpolation is the pictures themselves.
    CHECK(psnr(a, interpolate(a, b, pair, 0.0), 0.05) > 60.0);
    CHECK(psnr(b, interpolate(a, b, pair, 1.0), 0.05) > 60.0);
}

TEST_CASE("Optical flow interpolation follows a zoom", "[motion][interpolation]") {
    const MovingTexture texture(5);
    const int w = 480;
    const int h = 270;
    // A slow push-in about the centre: B is 4 % larger; the truth half way is 2 %.
    const Plane pa = texture.picture(w, h, 0.0, 0.0, 1.0, w / 2.0, h / 2.0);
    const Plane pb = texture.picture(w, h, 0.0, 0.0, 1.04, w / 2.0, h / 2.0);
    const RgbaImage truth = rgbaOf(texture.picture(w, h, 0.0, 0.0, 1.02, w / 2.0, h / 2.0));
    const FlowPair pair = estimateFlowPair(pa, pb, analysisFlowOptions());
    const double blending = psnr(truth, blend(rgbaOf(pa), rgbaOf(pb), 0.5), 0.05);
    const double flow = psnr(truth, interpolate(rgbaOf(pa), rgbaOf(pb), pair, 0.5), 0.05);
    INFO("blending " << blending << " dB, optical flow " << flow << " dB");
    CHECK(flow > blending + 10.0);
}

TEST_CASE("Optical flow is deterministic on any number of threads", "[motion][flow]") {
    const MovingTexture texture(9);
    const Plane a = texture.picture(256, 144, 0.0, 0.0);
    const Plane b = texture.picture(256, 144, 2.5, 1.25);
    FlowOptions one = analysisFlowOptions();
    one.threads = 1;
    FlowOptions many = analysisFlowOptions();
    many.threads = 6;
    const FlowPair p1 = estimateFlowPair(a, b, one);
    const FlowPair p2 = estimateFlowPair(a, b, many);
    const FlowPair p3 = estimateFlowPair(a, b, many);
    CHECK(p1.data == p2.data);
    CHECK(p2.data == p3.data);
    const RgbaImage i1 = interpolate(rgbaOf(a), rgbaOf(b), p1, 0.3, 1);
    const RgbaImage i2 = interpolate(rgbaOf(a), rgbaOf(b), p1, 0.3, 5);
    CHECK(i1.px == i2.px);
}

TEST_CASE("Optical flow refuses mismatched pictures and survives tiny ones", "[motion][flow]") {
    CHECK(estimateFlow(Plane(10, 10), Plane(11, 10)).empty());
    CHECK(estimateFlow(Plane(), Plane()).empty());
    const FlowField one = estimateFlow(Plane(1, 1, 0.5f), Plane(1, 1, 0.25f));
    CHECK(one.width == 1);
    CHECK(one.height == 1);
    CHECK(std::isfinite(one.u(0, 0)));
    const FlowField flat = estimateFlow(Plane(64, 48, 0.5f), Plane(64, 48, 0.5f));
    CHECK(meanEndPointError(flat, 0.0, 0.0) == 0.0);
    // NaN-free on a picture with infinities in it: flat patches keep their start.
    Plane odd(40, 30, 0.5f);
    odd(3, 3) = 1e30f;
    const FlowField survived = estimateFlow(odd, Plane(40, 30, 0.5f));
    CHECK(survived.width == 40);
    CHECK(interpolate(RgbaImage(4, 4), RgbaImage(5, 4), FlowPair{}, 0.5).empty());
}

TEST_CASE("Optical flow speed at the analysis size", "[motion][flow][timing]") {
    // A 1080p frame is analysed at kAnalysisSide (960 x 540): the measured cost of one pair is reported, not asserted
    // (perf runs check budgets on a quiet machine).
    const MovingTexture texture(13);
    const Plane a = texture.picture(960, 540, 0.0, 0.0);
    const Plane b = texture.picture(960, 540, 7.0, -3.0);
    const auto start = std::chrono::steady_clock::now();
    const FlowPair pair = estimateFlowPair(a, b, analysisFlowOptions());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("flow pair at 960 x 540: %.1f ms\n", ms);
    CHECK_FALSE(pair.empty());
}
