// Compressor static curve and timing, limiter ceiling (sample and true peak), transparency and latency.

#include "DspTestUtil.h"

#include <wabcut/audio/dsp/Compressor.h>
#include <wabcut/audio/dsp/Limiter.h>
#include <wabcut/audio/dsp/Loudness.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace wabcut::audio;
using namespace wabcut::audio::test;
using Catch::Approx;

namespace {

/// A signal whose |x| is constant (alternating sign): the peak detector sees one level, so the compressor's steady
/// gain is exactly its static curve.
std::vector<float> constantLevel(double db, double seconds, int channels = 2) {
    const auto frames = static_cast<std::size_t>(seconds * 48000.0);
    std::vector<float> x(frames * static_cast<std::size_t>(channels));
    const auto a = static_cast<float>(dbToAmp(db));
    for (std::size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            x[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = (i % 2 == 0) ? a : -a;
        }
    }
    return x;
}

} // namespace

TEST_CASE("The compressor's static curve: identity below, 1/ratio above, a smooth knee between", "[audio][dsp][dynamics]") {
    dsp::CompressorSettings s;
    s.thresholdDb = -20.0;
    s.ratio = 4.0;
    s.kneeDb = 6.0;
    CHECK(dsp::compressorCurveDb(-40.0, s) == -40.0);
    CHECK(dsp::compressorCurveDb(-23.0, s) == -23.0); // knee starts at -23
    CHECK(dsp::compressorCurveDb(-17.0, s) == Approx(-20.0 + 3.0 / 4.0)); // knee ends at -17
    CHECK(dsp::compressorCurveDb(0.0, s) == Approx(-15.0));
    CHECK(dsp::compressorCurveDb(-20.0, s) == Approx(-20.0 + (0.25 - 1.0) * 6.0 / 8.0)); // knee midpoint
    // Monotonic and continuous across the knee.
    double previous = dsp::compressorCurveDb(-30.0, s);
    for (double x = -29.99; x <= 0.0; x += 0.01) {
        const double y = dsp::compressorCurveDb(x, s);
        CHECK(y >= previous);
        CHECK(y - previous < 0.011);
        previous = y;
    }
    s.kneeDb = 0.0;
    CHECK(dsp::compressorCurveDb(-20.0, s) == -20.0);
    CHECK(dsp::compressorCurveDb(-10.0, s) == Approx(-17.5));
}

TEST_CASE("The compressor settles on its static curve plus makeup", "[audio][dsp][dynamics]") {
    dsp::CompressorSettings s;
    s.thresholdDb = -24.0;
    s.ratio = 3.0;
    s.attackMs = 5.0;
    s.releaseMs = 50.0;
    s.makeupDb = 4.0;
    for (const double level : {-40.0, -27.0, -24.0, -21.0, -12.0, -3.0}) {
        INFO("input " << level << " dBFS");
        dsp::Compressor comp(2, 48000.0);
        comp.setSettings(s);
        auto x = constantLevel(level, 1.0);
        comp.process(x.data(), static_cast<int>(x.size() / 2));
        const double out = ampToDb(std::fabs(x[x.size() - 1]));
        CHECK(out == Approx(dsp::compressorCurveDb(level, s) + s.makeupDb).margin(0.01));
        CHECK(comp.gainReductionDb() == Approx(level - dsp::compressorCurveDb(level, s)).margin(0.01));
    }
}

TEST_CASE("Compressor attack and release follow their time constants", "[audio][dsp][dynamics]") {
    dsp::CompressorSettings s;
    s.thresholdDb = -20.0;
    s.ratio = 4.0;
    s.kneeDb = 0.0;
    s.attackMs = 10.0;
    s.releaseMs = 100.0;
    dsp::Compressor comp(1, 48000.0);
    comp.setSettings(s);
    auto quiet = constantLevel(-40.0, 0.5, 1);
    comp.process(quiet.data(), static_cast<int>(quiet.size()));
    CHECK(comp.gainReductionDb() == 0.0);
    // Step up to -8 dBFS: the target reduction is 9 dB; after one attack time constant 63 % of it is there.
    const double target = -8.0 - dsp::compressorCurveDb(-8.0, s);
    REQUIRE(target == Approx(9.0));
    auto loud = constantLevel(-8.0, 0.010, 1); // 10 ms
    comp.process(loud.data(), static_cast<int>(loud.size()));
    CHECK(comp.gainReductionDb() == Approx(target * (1.0 - std::exp(-1.0))).margin(0.05));
    loud = constantLevel(-8.0, 0.5, 1);
    comp.process(loud.data(), static_cast<int>(loud.size()));
    CHECK(comp.gainReductionDb() == Approx(target).margin(0.01));
    // Back to quiet: after one release time constant 37 % of the reduction is left.
    quiet = constantLevel(-40.0, 0.100, 1);
    comp.process(quiet.data(), static_cast<int>(quiet.size()));
    CHECK(comp.gainReductionDb() == Approx(target * std::exp(-1.0)).margin(0.05));
    CHECK(comp.settleFrames() == 24000); // 5 x 100 ms
}

TEST_CASE("The limiter holds sample and true peaks at the ceiling", "[audio][dsp][dynamics][limiter]") {
    for (const double ceiling : {-1.0, -2.0, -0.1}) {
        INFO("ceiling " << ceiling << " dBTP");
        dsp::Limiter lim(2, 48000.0, 5.0);
        lim.setSettings(ceiling, 50.0);
        // Loud programme: noise bursts, a +9 dBFS sine and an inter-sample-peak tone (fs/4 at 45 degrees: samples
        // reach 0.707 of the true peak).
        auto x = noise(1.5, 0.5);
        append(x, sine(997.0, dbToAmp(9.0), 0.5));
        append(x, sine(12000.0, dbToAmp(3.0), 0.5, 2, 48000, std::numbers::pi / 4.0));
        append(x, sine(440.0, dbToAmp(-20.0), 0.5));
        lim.process(x.data(), static_cast<int>(x.size() / 2));
        CHECK(peak(x, 2) <= dbToAmp(ceiling) + 1e-6);
        dsp::LoudnessMeter meter(2, 48000);
        meter.add(x.data(), x.size() / 2);
        CHECK(meter.result().truePeakDbtp <= ceiling + 0.02); // measured by libebur128, an independent 4x meter
    }
}

TEST_CASE("Below the ceiling the limiter is a pure delay of latencyFrames()", "[audio][dsp][dynamics][limiter]") {
    for (const double lookahead : {0.0, 1.5, 5.0, 20.0}) {
        INFO("lookahead " << lookahead << " ms");
        dsp::Limiter lim(2, 48000.0, lookahead);
        lim.setSettings(-1.0, 50.0);
        CHECK(lim.latencyFrames() == lim.lookaheadFrames() + dsp::TruePeakDetector::kDelay + 2);
        const auto x = noise(0.5, 0.1);
        auto y = x;
        lim.process(y.data(), static_cast<int>(y.size() / 2));
        const auto d = static_cast<std::size_t>(lim.latencyFrames()) * 2;
        bool exact = true;
        for (std::size_t i = 0; i < d; ++i) {
            exact = exact && y[i] == 0.0f;
        }
        for (std::size_t i = d; i < y.size(); ++i) {
            exact = exact && y[i] == x[i - d];
        }
        CHECK(exact);
        CHECK(lim.gain() == 1.0);
    }
}

TEST_CASE("The limiter ramps down ahead of a peak and releases after it", "[audio][dsp][dynamics][limiter]") {
    dsp::Limiter lim(1, 48000.0, 5.0);
    lim.setSettings(-6.0, 20.0);
    // A quiet tone with one loud click in the middle.
    auto x = sine(200.0, 0.1, 0.5, 1);
    x[12000] = 1.0f;
    const std::vector<float> in = x;
    lim.process(x.data(), static_cast<int>(x.size()));
    const auto latency = static_cast<std::size_t>(lim.latencyFrames());
    const double ceiling = dbToAmp(-6.0);
    // The click arrives at 12000 + latency, at the ceiling less the 0.15 dB true-peak margin.
    CHECK(std::fabs(x[12000 + latency]) <= ceiling + 1e-6);
    CHECK(std::fabs(x[12000 + latency]) >= ceiling * dbToAmp(-0.15) * 0.999);
    // The gain is down over the lookahead before it (a ramp, not a step): halfway down it is between.
    const double before = std::fabs(x[12000 + latency - 120] / in[12000 - 120]);
    CHECK(before < 0.95);
    CHECK(before > ceiling);
    // Released: 200 ms later (10 release constants) the tone passes at full level again.
    CHECK(std::fabs(x[21600 + latency] - in[21600]) < 1e-4);
}
