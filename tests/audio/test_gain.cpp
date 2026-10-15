#include <wabcut/audio/Gain.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace wabcut::audio;
using Catch::Approx;

TEST_CASE("dB <-> linear gain matches reference values", "[audio][gain]") {
    CHECK(dbToLinear(0.0f) == 1.0f);
    CHECK(dbToLinear(-6.0206f) == Approx(0.5f).epsilon(1e-5));
    CHECK(dbToLinear(6.0206f) == Approx(2.0f).epsilon(1e-5));
    CHECK(dbToLinear(-20.0f) == Approx(0.1f).epsilon(1e-6));
    CHECK(dbToLinear(-60.0f) == Approx(0.001f).epsilon(1e-6));
    CHECK(dbToLinear(kSilenceDb) == 0.0f);
    CHECK(dbToLinear(-std::numeric_limits<float>::infinity()) == 0.0f);
    CHECK(dbToLinear(std::numeric_limits<float>::quiet_NaN()) == 0.0f);

    CHECK(linearToDb(1.0f) == 0.0f);
    CHECK(linearToDb(0.5f) == Approx(-6.0206f).margin(1e-4));
    CHECK(linearToDb(10.0f) == Approx(20.0f).margin(1e-5));
    CHECK(linearToDb(0.0f) == kSilenceDb);
    CHECK(linearToDb(-1.0f) == kSilenceDb);
    for (float db = -120.0f; db <= 24.0f; db += 0.5f) {
        CHECK(linearToDb(dbToLinear(db)) == Approx(db).margin(1e-3));
    }
}

TEST_CASE("Constant-power pan: -3 dB centre, unit power everywhere", "[audio][gain][pan]") {
    const StereoGains centre = constantPowerPan(0.0f);
    CHECK(centre.left == Approx(0.70710678f).epsilon(1e-6));
    CHECK(centre.right == Approx(0.70710678f).epsilon(1e-6));
    CHECK(linearToDb(centre.left) == Approx(-3.0103f).margin(1e-3));

    const StereoGains left = constantPowerPan(-1.0f);
    CHECK(left.left == Approx(1.0f));
    CHECK(left.right == Approx(0.0f).margin(1e-7));
    const StereoGains right = constantPowerPan(1.0f);
    CHECK(right.left == Approx(0.0f).margin(1e-7));
    CHECK(right.right == Approx(1.0f));

    // pan +0.5 -> theta = 3pi/8
    const StereoGains half = constantPowerPan(0.5f);
    CHECK(half.left == Approx(0.38268343f).epsilon(1e-5));
    CHECK(half.right == Approx(0.92387953f).epsilon(1e-5));

    for (float p = -1.0f; p <= 1.0f; p += 0.05f) {
        const StereoGains g = constantPowerPan(p);
        CHECK(g.left * g.left + g.right * g.right == Approx(1.0f).epsilon(1e-5));
    }
    // Clamped and NaN-safe.
    CHECK(constantPowerPan(-7.0f).left == Approx(1.0f));
    CHECK(constantPowerPan(std::numeric_limits<float>::quiet_NaN()).left == Approx(0.70710678f));
}

TEST_CASE("Stereo balance is unity at centre and silences the far side at the extremes", "[audio][gain][pan]") {
    const StereoGains centre = stereoBalance(0.0f);
    CHECK(centre.left == Approx(1.0f).epsilon(1e-6));
    CHECK(centre.right == Approx(1.0f).epsilon(1e-6));
    CHECK(stereoBalance(-1.0f).left == Approx(1.0f));
    CHECK(stereoBalance(-1.0f).right == Approx(0.0f).margin(1e-6));
    const StereoGains half = stereoBalance(0.5f); // sqrt2 * (0.38268, 0.92388) = (0.5412, 1.3066 -> 1)
    CHECK(half.left == Approx(0.5411961f).epsilon(1e-5));
    CHECK(half.right == 1.0f);
}

TEST_CASE("Soft clip guard is transparent below -1 dBFS and bounded above", "[audio][gain][clip]") {
    for (float x = -kSoftClipThreshold; x <= kSoftClipThreshold; x += 0.001f) {
        CHECK(softClip(x) == x); // bit-exact below the knee
    }
    // Reference: softClip(1) = t + (1 - t) * tanh(1)
    const double t = kSoftClipThreshold;
    CHECK(softClip(1.0f) == Approx(t + (1.0 - t) * std::tanh(1.0)).epsilon(1e-6));
    CHECK(softClip(-1.0f) == Approx(-(t + (1.0 - t) * std::tanh(1.0))).epsilon(1e-6));

    // Continuous at the knee with unit slope.
    const float above = kSoftClipThreshold + 1e-4f;
    CHECK(softClip(above) == Approx(above).margin(1e-6));

    float previous = softClip(-1e6f);
    bool bounded = true;
    bool monotonic = true;
    bool odd = true;
    for (int i = -40000; i <= 40000; ++i) {
        const float x = static_cast<float>(i) * 0.0005f; // -20 .. +20 (+26 dBFS)
        const float y = softClip(x);
        bounded = bounded && std::fabs(y) <= 1.0f;
        monotonic = monotonic && y >= previous;
        odd = odd && softClip(-x) == -y;
        previous = y;
    }
    CHECK(bounded);
    CHECK(monotonic);
    CHECK(odd);
    CHECK(softClip(1e30f) == 1.0f);
    CHECK(softClip(std::numeric_limits<float>::infinity()) == 1.0f);
    CHECK(softClip(-std::numeric_limits<float>::infinity()) == -1.0f);
    CHECK(softClip(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    CHECK(softClip(1.2f) < 1.0f); // still below full scale for moderate overs
}
