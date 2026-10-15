// The 3-band EQ against the cookbook's reference curves: analytic magnitude response and measured sine gains.

#include "DspTestUtil.h"

#include <wabcut/audio/dsp/Equalizer.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace wabcut::audio;
using namespace wabcut::audio::test;
using Catch::Approx;

namespace {

/// Gain the EQ applies to a steady sine, measured after the filter settled (RMS out vs RMS in over the same
/// half second, a whole number of periods for every test frequency).
double measuredGainDb(const dsp::Eq3Settings& settings, double frequency) {
    dsp::Eq3 eq(1, 48000.0);
    eq.setSettings(settings);
    const auto in = sine(frequency, 0.25, 1.0, 1);
    auto x = in;
    eq.process(x.data(), static_cast<int>(x.size()));
    return ampToDb(rms(x, 1, 24000, 48000) / rms(in, 1, 24000, 48000));
}

} // namespace

TEST_CASE("Shelves and the peaking band follow the cookbook curves", "[audio][dsp][eq]") {
    constexpr double fs = 48000.0;
    SECTION("low shelf: full gain far below, half gain at the corner, unity far above") {
        const auto c = dsp::lowShelf(fs, 100.0, 6.0);
        CHECK(dsp::magnitudeDb(c, 5.0, fs) == Approx(6.0).margin(0.02));
        CHECK(dsp::magnitudeDb(c, 100.0, fs) == Approx(3.0).margin(0.02));
        CHECK(dsp::magnitudeDb(c, 5000.0, fs) == Approx(0.0).margin(0.02));
    }
    SECTION("high shelf") {
        const auto c = dsp::highShelf(fs, 8000.0, -9.0);
        CHECK(dsp::magnitudeDb(c, 23900.0, fs) == Approx(-9.0).margin(0.05));
        CHECK(dsp::magnitudeDb(c, 8000.0, fs) == Approx(-4.5).margin(0.05));
        CHECK(dsp::magnitudeDb(c, 100.0, fs) == Approx(0.0).margin(0.02));
    }
    SECTION("peaking band: exact gain at the centre, symmetric in octaves, unity far away") {
        const auto c = dsp::peaking(fs, 1000.0, 12.0, 1.0);
        CHECK(dsp::magnitudeDb(c, 1000.0, fs) == Approx(12.0).margin(1e-9));
        CHECK(dsp::magnitudeDb(c, 500.0, fs) == Approx(dsp::magnitudeDb(c, 2000.0, fs)).margin(0.25));
        CHECK(dsp::magnitudeDb(c, 20.0, fs) == Approx(0.0).margin(0.05));
        CHECK(dsp::magnitudeDb(c, 20000.0, fs) == Approx(0.0).margin(0.1));
        // A wider band (lower Q) lifts the octave neighbours more.
        const auto wide = dsp::peaking(fs, 1000.0, 12.0, 0.5);
        CHECK(dsp::magnitudeDb(wide, 2000.0, fs) > dsp::magnitudeDb(c, 2000.0, fs) + 1.0);
    }
    SECTION("0 dB is exactly the identity") {
        for (const auto& c : {dsp::lowShelf(fs, 100.0, 0.0), dsp::highShelf(fs, 8000.0, 0.0), dsp::peaking(fs, 1000.0, 0.0, 2.0)}) {
            CHECK(c.b0 == 1.0);
            CHECK(c.b1 == c.a1);
            CHECK(c.b2 == c.a2);
        }
    }
}

TEST_CASE("The 3-band EQ processes what its response says", "[audio][dsp][eq]") {
    dsp::Eq3Settings s;
    s.lowFrequency = 120.0;
    s.lowGainDb = 6.0;
    s.midFrequency = 2500.0;
    s.midGainDb = -9.0;
    s.midQ = 2.0;
    s.highFrequency = 10000.0;
    s.highGainDb = 4.0;
    dsp::Eq3 eq(1, 48000.0);
    eq.setSettings(s);
    for (const double f : {40.0, 120.0, 400.0, 1000.0, 2500.0, 5000.0, 10000.0, 16000.0}) {
        INFO(f << " Hz");
        CHECK(measuredGainDb(s, f) == Approx(eq.magnitudeDb(f)).margin(0.05));
    }
    CHECK(eq.magnitudeDb(2500.0) == Approx(-9.0).margin(0.15)); // the shelves barely reach the mid band
}

TEST_CASE("A flat EQ is bit-exact and settles out after a change", "[audio][dsp][eq]") {
    dsp::Eq3 eq(2, 48000.0);
    auto x = noise(0.5, 0.2);
    auto y = x;
    eq.process(y.data(), static_cast<int>(y.size() / 2));
    CHECK(y == x);

    dsp::Eq3Settings boost;
    boost.midGainDb = 12.0;
    eq.setSettings(boost);
    eq.process(y.data(), static_cast<int>(y.size() / 2));
    CHECK(y != x);
    // Back to flat: the ringing of the boost dies out, then the EQ is exact identity again.
    eq.setSettings(dsp::Eq3Settings{});
    std::vector<float> silence(48000 * 2, 0.0f);
    eq.process(silence.data(), 48000);
    auto z = x;
    eq.process(z.data(), static_cast<int>(z.size() / 2));
    CHECK(z == x);
}

TEST_CASE("Sweeping EQ gain keeps the output finite and continuous", "[audio][dsp][eq]") {
    dsp::Eq3 eq(1, 48000.0);
    auto x = sine(1000.0, 0.5, 1.0, 1);
    double maxStep = 0.0;
    float previous = 0.0f;
    for (std::size_t block = 0; block < x.size() / 32; ++block) {
        dsp::Eq3Settings s;
        s.midGainDb = -24.0 + 48.0 * static_cast<double>(block) / static_cast<double>(x.size() / 32);
        eq.setSettings(s);
        eq.process(x.data() + block * 32, 32);
        for (std::size_t i = block * 32; i < block * 32 + 32; ++i) {
            REQUIRE(std::isfinite(x[i]));
            maxStep = std::max(maxStep, static_cast<double>(std::fabs(x[i] - previous)));
            previous = x[i];
        }
    }
    // A +24 dB 1 kHz sine moves at most 2*pi*1000/48000 * 0.5 * 15.85 = 1.04 per sample; no jumps beyond that.
    CHECK(maxStep < 1.2);
}
