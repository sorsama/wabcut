// p02 audio: Premiere Pro's Filter family of audio effects, measured offline: the graphic and parametric equalizers,
// the simple filters, the Notch, Scientific and FFT filters, the Stereo Expander and the channel effects. Responses are
// steady-state sine gains through an EffectChain (the path preview and export take), checked against the cookbook's
// analytic curves (dsp::magnitudeDb) where the effect is a cookbook section, and against the filter type's defining
// points (-3 dB corner, ripple, stopband) otherwise.

#include "DspTestUtil.h"

#include <wabcut/audio/dsp/Biquad.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using namespace wabcut::audio::test;
using Catch::Approx;

namespace {

constexpr int kRate = 48000;

EffectInstance with(std::string id, std::vector<std::pair<std::string, double>> values) {
    std::vector<EffectParam> params;
    for (auto& [key, v] : values) {
        params.push_back({key, ParamValue(v)});
    }
    return effectOf(std::move(id), std::move(params));
}

/// Amplitude of `frequency` in `channel` over a whole number of its periods ending at frame `last`.
double amplitudeOf(const std::vector<float>& x, int channels, double frequency, std::size_t last, int channel = 0,
                   double span = 0.25) {
    const double cycles = std::max(1.0, std::floor(span * frequency));
    const auto frames = static_cast<std::size_t>(std::llround(cycles * kRate / frequency));
    const std::size_t first = last > frames ? last - frames : 0;
    std::complex<double> sum{};
    for (std::size_t i = first; i < last; ++i) {
        const double phase = -2.0 * std::numbers::pi * frequency * static_cast<double>(i) / kRate;
        sum += static_cast<double>(x[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)]) *
               std::polar(1.0, phase);
    }
    return 2.0 * std::abs(sum) / static_cast<double>(last - first);
}

/// Steady-state gain (dB) of `effect` for a sine at `frequency` (mono unless `channels`), after `seconds` of it.
double gainDb(const EffectInstance& effect, double frequency, double seconds = 1.0, int channels = 1) {
    const auto in = sine(frequency, 0.25, seconds, channels, kRate);
    const auto out = runEffect(effect, in, channels, kRate);
    const std::size_t frames = in.size() / static_cast<std::size_t>(channels);
    return ampToDb(amplitudeOf(out, channels, frequency, frames) / amplitudeOf(in, channels, frequency, frames));
}

/// Largest |out - in| of an effect over noise (bypass-like settings).
double deviation(const EffectInstance& effect, int channels = 2) {
    const auto in = noise(0.5, 0.5, channels, kRate);
    const auto out = runEffect(effect, in, channels, kRate);
    double worst = 0.0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(out[i] - in[i])));
    }
    return worst;
}

/// Interleaved stereo from two channel signals.
std::vector<float> stereo(const std::vector<float>& left, const std::vector<float>& right) {
    std::vector<float> out(left.size() * 2);
    for (std::size_t i = 0; i < left.size(); ++i) {
        out[2 * i] = left[i];
        out[2 * i + 1] = right[i];
    }
    return out;
}

double cookbookDb(const dsp::BiquadCoefficients& c, double frequency) { return dsp::magnitudeDb(c, frequency, kRate); }

/// The bilinear Butterworth low-pass of `order` at `cutoff`: -10 log10(1 + r^2n), r the prewarped frequency ratio (the
/// high-pass: 1 / r).
double butterworthDb(int order, double cutoff, double frequency, bool high = false) {
    const double r = std::tan(std::numbers::pi * frequency / kRate) / std::tan(std::numbers::pi * cutoff / kRate);
    return -10.0 * std::log10(1.0 + std::pow(high ? 1.0 / r : r, 2.0 * order));
}

} // namespace

// ---- Graphic Equalizer -------------------------------------------------------------------------------------------

TEST_CASE("Graphic equalizers at 0 dB are bit-exact identity", "[audio][dsp][premiere][filter]") {
    for (const char* id : {"wabcut.audio.graphicEq10", "wabcut.audio.graphicEq20", "wabcut.audio.graphicEq30"}) {
        INFO(id);
        CHECK(deviation(effectOf(id)) == 0.0);
    }
}

TEST_CASE("Graphic equalizer 10 bands boosts one octave band by its slider", "[audio][dsp][premiere][filter]") {
    const auto e = with("wabcut.audio.graphicEq10", {{"band6", 12.0}}); // 31.25 Hz x 2^5 = 1 kHz
    CHECK(gainDb(e, 1000.0) == Approx(12.0).margin(0.1));
    // The neighbours' centres stay where their sliders are (the solver cancels the band's skirt there).
    CHECK(gainDb(e, 500.0) == Approx(0.0).margin(0.3));
    CHECK(gainDb(e, 2000.0) == Approx(0.0).margin(0.3));
    CHECK(gainDb(e, 125.0) == Approx(0.0).margin(0.2));
    CHECK(gainDb(e, 8000.0) == Approx(0.0).margin(0.2));
    // Between the centres the curve passes smoothly: half an octave off it is well up, but below the peak.
    const double half = gainDb(e, 1414.0);
    CHECK(half > 3.0);
    CHECK(half < 11.0);
}

TEST_CASE("Graphic equalizer with every slider up is flat", "[audio][dsp][premiere][filter]") {
    std::vector<std::pair<std::string, double>> all;
    for (int b = 1; b <= 10; ++b) {
        all.emplace_back("band" + std::to_string(b), 6.0);
    }
    const auto e = with("wabcut.audio.graphicEq10", all);
    for (const double f : {62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0}) {
        INFO(f << " Hz");
        CHECK(gainDb(e, f) == Approx(6.0).margin(0.15));
    }
    for (const double f : {88.4, 353.6, 1414.2, 5657.0}) {
        INFO(f << " Hz (between bands)");
        CHECK(gainDb(e, f) == Approx(6.0).margin(1.0));
    }
}

TEST_CASE("Graphic equalizer 20 bands follows alternating sliders", "[audio][dsp][premiere][filter]") {
    std::vector<std::pair<std::string, double>> zig;
    for (int b = 1; b <= 20; ++b) {
        zig.emplace_back("band" + std::to_string(b), b % 2 == 0 ? 6.0 : -6.0);
    }
    const auto e = with("wabcut.audio.graphicEq20", zig);
    // Band b centres at 31.25 Hz x 2^((b - 1) / 2).
    for (const int b : {5, 6, 9, 10, 11, 12, 15, 16}) {
        const double f = 31.25 * std::pow(2.0, (b - 1) / 2.0);
        INFO("band " << b << " at " << f << " Hz");
        CHECK(gainDb(e, f, 1.5) == Approx(b % 2 == 0 ? 6.0 : -6.0).margin(0.3));
    }
}

TEST_CASE("Graphic equalizer 30 bands cuts one third octave", "[audio][dsp][premiere][filter]") {
    const auto e = with("wabcut.audio.graphicEq30", {{"band16", -12.0}}); // 31.25 Hz x 2^(15 / 3) = 1 kHz
    CHECK(gainDb(e, 1000.0) == Approx(-12.0).margin(0.15));
    CHECK(gainDb(e, 1259.9) == Approx(0.0).margin(0.3));
    CHECK(gainDb(e, 793.7) == Approx(0.0).margin(0.3));
    CHECK(gainDb(e, 2000.0) == Approx(0.0).margin(0.3));
    CHECK(gainDb(e, 100.0) == Approx(0.0).margin(0.1));
    // The top band that fits at 48 kHz (31.25 Hz x 2^(28 / 3) = 20.2 kHz) works too.
    const auto top = with("wabcut.audio.graphicEq30", {{"band29", 6.0}});
    CHECK(gainDb(top, 20158.7) == Approx(6.0).margin(0.2));
}

TEST_CASE("Graphic equalizer gain range and accuracy", "[audio][dsp][premiere][filter]") {
    const auto master = with("wabcut.audio.graphicEq10", {{"gain", -6.0}});
    CHECK(gainDb(master, 300.0) == Approx(-6.0).margin(0.01));
    CHECK(gainDb(master, 7000.0) == Approx(-6.0).margin(0.01));
    // A slider beyond the range is held at its edge: Range 24 dB = +-12 dB.
    const auto clamped = with("wabcut.audio.graphicEq10", {{"band6", 20.0}, {"range", 24.0}});
    CHECK(gainDb(clamped, 1000.0) == Approx(12.0).margin(0.15));
    // Accuracy: steep patterns stretch the linear solution (the bands' shape changes with their gain, a few dB off at
    // Accuracy 10); the refinement passes of higher settings (Premiere's default is 1000) bring the centres onto the
    // sliders.
    std::vector<std::pair<std::string, double>> steep{{"band4", 18.0}, {"band5", -18.0}, {"band6", 18.0}};
    const auto at = [&steep](double accuracy) {
        auto v = steep;
        v.emplace_back("accuracy", accuracy);
        return with("wabcut.audio.graphicEq10", v);
    };
    const auto rough = at(10.0);
    const auto standard = at(1000.0);
    const auto exact = at(8192.0);
    for (const auto& [f, g] : {std::pair{250.0, 18.0}, std::pair{500.0, -18.0}, std::pair{1000.0, 18.0}, std::pair{4000.0, 0.0}}) {
        INFO(f << " Hz");
        CHECK(gainDb(rough, f) == Approx(g).margin(5.0));
        CHECK(gainDb(standard, f) == Approx(g).margin(0.1));
        CHECK(gainDb(exact, f) == Approx(g).margin(0.05));
    }
}

// ---- Parametric Equalizer ----------------------------------------------------------------------------------------

TEST_CASE("Parametric equalizer at its defaults is identity", "[audio][dsp][premiere][filter]") {
    // Premiere's defaults enable the shelves and the five bands, all at 0 dB, and leave the pass filters off.
    CHECK(deviation(effectOf("wabcut.audio.parametricEq")) == 0.0);
}

TEST_CASE("Parametric equalizer high and low pass follow Butterworth slopes", "[audio][dsp][premiere][filter]") {
    // High pass at 100 Hz, 24 dB per octave (order 4, Premiere's default slope).
    const auto hp = with("wabcut.audio.parametricEq", {{"highPassEnabled", 1.0}, {"highPassFrequency", 100.0}});
    for (const double f : {40.0, 50.0, 100.0, 200.0, 2000.0}) {
        INFO(f << " Hz");
        CHECK(gainDb(hp, f, 1.5) == Approx(butterworthDb(4, 100.0, f, true)).margin(0.05));
    }
    CHECK(gainDb(hp, 100.0, 1.5) == Approx(-3.01).margin(0.05));
    CHECK(gainDb(hp, 50.0, 1.5) == Approx(-24.1).margin(0.2)); // 24 dB an octave below
    // Low pass at 5 kHz, 6 dB per octave (first order).
    const auto lp = with("wabcut.audio.parametricEq", {{"lowPassEnabled", 1.0}, {"lowPassFrequency", 5000.0}, {"lowPassSlope", 1.0}});
    for (const double f : {200.0, 2500.0, 5000.0, 10000.0, 15000.0}) {
        INFO(f << " Hz");
        CHECK(gainDb(lp, f) == Approx(butterworthDb(1, 5000.0, f)).margin(0.05));
    }
    // 48 dB per octave (order 8): an octave above the corner is 48 dB down (49.4 with the bilinear warping at 4 kHz).
    const auto steep = with("wabcut.audio.parametricEq", {{"lowPassEnabled", 1.0}, {"lowPassFrequency", 2000.0}, {"lowPassSlope", 8.0}});
    CHECK(gainDb(steep, 2000.0) == Approx(-3.01).margin(0.05));
    CHECK(gainDb(steep, 4000.0) == Approx(butterworthDb(8, 2000.0, 4000.0)).margin(0.1));
    CHECK(gainDb(steep, 4000.0) < -48.0);
}

TEST_CASE("Parametric equalizer second order shelves and bands match the cookbook curves", "[audio][dsp][premiere][filter]") {
    const auto e = with("wabcut.audio.parametricEq",
                        {{"lowShelfOrder", 2.0}, {"lowShelfFrequency", 150.0}, {"lowShelfGain", 6.0},
                         {"band3Frequency", 2000.0}, {"band3Gain", -9.0}, {"band3Q", 2.0},
                         {"highShelfOrder", 2.0}, {"highShelfFrequency", 9000.0}, {"highShelfGain", 4.0}});
    const auto ls = dsp::lowShelf(kRate, 150.0, 6.0, 1.0);
    const auto pk = dsp::peaking(kRate, 2000.0, -9.0, 2.0);
    const auto hs = dsp::highShelf(kRate, 9000.0, 4.0, 1.0);
    for (const double f : {40.0, 150.0, 600.0, 1500.0, 2000.0, 3000.0, 9000.0, 15000.0}) {
        INFO(f << " Hz");
        const double expected = cookbookDb(ls, f) + cookbookDb(pk, f) + cookbookDb(hs, f);
        CHECK(gainDb(e, f) == Approx(expected).margin(0.05));
    }
}

TEST_CASE("Parametric equalizer first order shelves rise 6 dB per octave", "[audio][dsp][premiere][filter]") {
    // The bilinear first-order shelf: |H|^2 = (x^2 + g) / (x^2 + 1 / g) (low), x the prewarped frequency ratio.
    const auto shelf = [](double gainDb, double corner, double f, bool high) {
        const double g = std::pow(10.0, gainDb / 20.0);
        double x = std::tan(std::numbers::pi * f / kRate) / std::tan(std::numbers::pi * corner / kRate);
        if (high) {
            x = 1.0 / x;
        }
        return 10.0 * std::log10((x * x + g) / (x * x + 1.0 / g)); // the high shelf is the low one mirrored
    };
    const auto e = with("wabcut.audio.parametricEq", {{"lowShelfFrequency", 200.0}, {"lowShelfGain", 12.0}});
    CHECK(gainDb(e, 200.0) == Approx(6.0).margin(0.05)); // half the gain at the corner
    for (const double f : {20.0, 60.0, 200.0, 700.0, 2000.0, 10000.0}) {
        INFO(f << " Hz");
        CHECK(gainDb(e, f, 1.5) == Approx(shelf(12.0, 200.0, f, false)).margin(0.05));
    }
    CHECK(gainDb(e, 20.0, 1.5) > 11.5);
    const auto high = with("wabcut.audio.parametricEq", {{"highShelfFrequency", 4000.0}, {"highShelfGain", -10.0}});
    CHECK(gainDb(high, 4000.0) == Approx(-5.0).margin(0.05));
    CHECK(gainDb(high, 100.0) == Approx(0.0).margin(0.05));
    CHECK(gainDb(high, 20000.0) < -8.5);
}

TEST_CASE("Parametric equalizer constant width and gain", "[audio][dsp][premiere][filter]") {
    // Constant Width: a 500 Hz wide band at 2 kHz is the Q 4 band.
    const auto e = with("wabcut.audio.parametricEq", {{"constantQ", 0.0}, {"band1Frequency", 2000.0}, {"band1Gain", 10.0},
                                                      {"band1Bandwidth", 500.0}, {"gain", -3.0}});
    const auto pk = dsp::peaking(kRate, 2000.0, 10.0, 4.0);
    for (const double f : {1000.0, 1800.0, 2000.0, 2300.0}) {
        INFO(f << " Hz");
        CHECK(gainDb(e, f) == Approx(cookbookDb(pk, f) - 3.0).margin(0.05));
    }
    // A disabled band does nothing whatever its gain.
    const auto off = with("wabcut.audio.parametricEq", {{"band2Enabled", 0.0}, {"band2Gain", 12.0}});
    CHECK(deviation(off) == 0.0);
}

// ---- Simple filters, Bass and Treble -----------------------------------------------------------------------------

TEST_CASE("Simple parametric EQ highpass lowpass and bandpass are cookbook sections", "[audio][dsp][premiere][filter]") {
    const auto peq = with("wabcut.audio.simpleParametricEq", {{"center", 800.0}, {"q", 3.0}, {"boost", 8.0}});
    const auto hp = with("wabcut.audio.highpass", {{"cutoff", 300.0}});
    const auto lp = with("wabcut.audio.lowpass", {{"cutoff", 3000.0}});
    const auto bp = with("wabcut.audio.bandpass", {{"cutoff", 1500.0}, {"q", 2.0}});
    const auto peqRef = dsp::peaking(kRate, 800.0, 8.0, 3.0);
    const auto hpRef = dsp::highPass(kRate, 300.0);
    const auto lpRef = dsp::lowPass(kRate, 3000.0);
    for (const double f : {100.0, 300.0, 800.0, 1500.0, 3000.0, 9000.0}) {
        INFO(f << " Hz");
        CHECK(gainDb(peq, f) == Approx(cookbookDb(peqRef, f)).margin(0.05));
        CHECK(gainDb(hp, f) == Approx(cookbookDb(hpRef, f)).margin(0.05));
        CHECK(gainDb(lp, f) == Approx(cookbookDb(lpRef, f)).margin(0.05));
    }
    CHECK(gainDb(hp, 300.0) == Approx(-3.01).margin(0.05));
    CHECK(gainDb(lp, 3000.0) == Approx(-3.01).margin(0.05));
    // Band-pass: unity at the centre, -3 dB at the edges of a Q 2 band (centre / Q wide, geometric about it).
    CHECK(gainDb(bp, 1500.0) == Approx(0.0).margin(0.05));
    const double edge = 1500.0 * (std::sqrt(1.0 + 1.0 / 16.0) - 0.25); // lower -3 dB edge of Q 2
    CHECK(gainDb(bp, edge) == Approx(-3.0).margin(0.3));
    CHECK(gainDb(bp, 150.0) < -20.0);
    // Defaults: Simple Parametric EQ at 0 dB is identity; the others filter around 1495 Hz.
    CHECK(deviation(effectOf("wabcut.audio.simpleParametricEq")) == 0.0);
    CHECK(gainDb(effectOf("wabcut.audio.highpass"), 1495.0) == Approx(-3.01).margin(0.05));
}

TEST_CASE("Simple notch filter removes its cutoff and keeps the rest", "[audio][dsp][premiere][filter]") {
    const auto e = with("wabcut.audio.simpleNotch", {{"cutoff", 1000.0}, {"q", 10.0}});
    CHECK(gainDb(e, 1000.0, 2.0) < -50.0);
    CHECK(gainDb(e, 500.0) == Approx(0.0).margin(0.1));
    CHECK(gainDb(e, 2000.0) == Approx(0.0).margin(0.1));
}

TEST_CASE("Bass and Treble shelve below 200 Hz and above 4 kHz", "[audio][dsp][premiere][filter]") {
    const auto bass = with("wabcut.audio.bass", {{"boost", 6.0}});
    const auto treble = with("wabcut.audio.treble", {{"boost", -8.0}});
    const auto bassRef = dsp::lowShelf(kRate, 200.0, 6.0, 1.0);
    const auto trebleRef = dsp::highShelf(kRate, 4000.0, -8.0, 1.0);
    for (const double f : {30.0, 200.0, 1000.0, 4000.0, 16000.0}) {
        INFO(f << " Hz");
        CHECK(gainDb(bass, f) == Approx(cookbookDb(bassRef, f)).margin(0.05));
        CHECK(gainDb(treble, f) == Approx(cookbookDb(trebleRef, f)).margin(0.05));
    }
    CHECK(gainDb(bass, 30.0) == Approx(6.0).margin(0.3));
    CHECK(gainDb(treble, 16000.0) == Approx(-8.0).margin(0.4));
    CHECK(deviation(effectOf("wabcut.audio.bass")) == 0.0);
    CHECK(deviation(effectOf("wabcut.audio.treble")) == 0.0);
}

// ---- Notch Filter ------------------------------------------------------------------------------------------------

TEST_CASE("Notch filter cuts each notch by its gain and leaves the octave around it", "[audio][dsp][premiere][filter]") {
    for (const int width : {0, 1, 2}) {
        INFO("notch width " << width);
        const auto e = with("wabcut.audio.notchFilter", {{"notchWidth", static_cast<double>(width)},
                                                         {"notch1Enabled", 1.0}, {"notch1Frequency", 1000.0}, {"notch1Gain", -30.0},
                                                         {"notch2Enabled", 1.0}, {"notch2Frequency", 6000.0}, {"notch2Gain", -12.0}});
        CHECK(gainDb(e, 1000.0, 2.0) == Approx(-30.0).margin(0.5));
        CHECK(gainDb(e, 6000.0, 2.0) == Approx(-12.0).margin(0.3));
        CHECK(gainDb(e, 500.0) == Approx(0.0).margin(0.3));
        CHECK(gainDb(e, 2000.0) == Approx(0.0).margin(0.3));
        CHECK(gainDb(e, 3000.0) == Approx(0.0).margin(0.3));
    }
    // The -3 dB edges are a 1/8 (Narrow), 1/16 (Very Narrow), 1/32 (Super Narrow) of the frequency apart, geometric about
    // it, whatever the depth; a narrower setting cuts less beside the notch.
    for (const int width : {0, 1, 2}) {
        INFO("notch width " << width);
        const auto e = with("wabcut.audio.notchFilter", {{"notchWidth", static_cast<double>(width)}, {"notch1Enabled", 1.0},
                                                         {"notch1Frequency", 1000.0}, {"notch1Gain", -40.0}});
        const double half = 1.0 / (16.0 * (1 << width));
        CHECK(gainDb(e, 1000.0 * (std::sqrt(1.0 + half * half) - half), 3.0) == Approx(-3.0).margin(0.25));
        CHECK(gainDb(e, 1000.0 * (std::sqrt(1.0 + half * half) + half), 3.0) == Approx(-3.0).margin(0.25));
    }
    const auto beside = [](int width) {
        return gainDb(with("wabcut.audio.notchFilter", {{"notchWidth", static_cast<double>(width)}, {"notch1Enabled", 1.0},
                                                        {"notch1Frequency", 1000.0}, {"notch1Gain", -30.0}}),
                      1080.0, 2.0);
    };
    const double narrow = beside(0);
    const double veryNarrow = beside(1);
    const double superNarrow = beside(2);
    CHECK(narrow < -1.5);
    CHECK(narrow < veryNarrow - 1.0);
    CHECK(veryNarrow < superNarrow);
    CHECK(superNarrow > -0.01);
}

TEST_CASE("Notch filter boosts amplify all and defaults", "[audio][dsp][premiere][filter]") {
    // A positive gain is a narrow boost of the same shape.
    const auto boost = with("wabcut.audio.notchFilter", {{"notch3Enabled", 1.0}, {"notch3Frequency", 2000.0}, {"notch3Gain", 12.0}});
    CHECK(gainDb(boost, 2000.0, 2.0) == Approx(12.0).margin(0.2));
    CHECK(gainDb(boost, 1000.0) == Approx(0.0).margin(0.3));
    // Amplify All: every enabled notch takes the Fixed Gain.
    const auto fixed = with("wabcut.audio.notchFilter", {{"fixGains", 1.0}, {"fixedGain", -20.0}, {"notch1Enabled", 1.0},
                                                         {"notch1Frequency", 1000.0}, {"notch1Gain", -40.0}});
    CHECK(gainDb(fixed, 1000.0, 2.0) == Approx(-20.0).margin(0.4));
    // Every notch off (the defaults): identity.
    CHECK(deviation(effectOf("wabcut.audio.notchFilter")) == 0.0);
}

// ---- Scientific Filter -------------------------------------------------------------------------------------------

namespace {

EffectInstance scientific(int type, int mode, int order, double cutoff, double high = 2000.0, double ripple = 1.0,
                          double stop = 60.0) {
    return with("wabcut.audio.scientificFilter", {{"type", static_cast<double>(type)}, {"mode", static_cast<double>(mode)},
                                                  {"order", static_cast<double>(order)}, {"cutoff", cutoff}, {"highCutoff", high},
                                                  {"passRipple", ripple}, {"stopAttenuation", stop}});
}

constexpr int kBessel = 0;
constexpr int kButterworth = 1;
constexpr int kChebyshev = 2;
constexpr int kElliptical = 3;
constexpr int kLowPass = 0;
constexpr int kHighPass = 1;
constexpr int kBandPass = 2;
constexpr int kBandStop = 3;

} // namespace

TEST_CASE("Scientific filter Butterworth is 3 dB down at the cutoff in every mode", "[audio][dsp][premiere][filter]") {
    const auto lp = scientific(kButterworth, kLowPass, 4, 1000.0);
    const auto hp = scientific(kButterworth, kHighPass, 3, 500.0);
    for (const double f : {100.0, 500.0, 1000.0, 2000.0, 4000.0}) {
        INFO(f << " Hz");
        CHECK(gainDb(lp, f, 1.5) == Approx(butterworthDb(4, 1000.0, f)).margin(0.05));
        CHECK(gainDb(hp, f, 1.5) == Approx(butterworthDb(3, 500.0, f, true)).margin(0.05));
    }
    CHECK(gainDb(lp, 1000.0, 1.5) == Approx(-3.01).margin(0.05));
    CHECK(gainDb(hp, 500.0, 1.5) == Approx(-3.01).margin(0.05));
    CHECK(gainDb(hp, 250.0, 1.5) == Approx(-18.1).margin(0.2)); // 3rd order: 18 dB an octave below
    const auto bp = scientific(kButterworth, kBandPass, 3, 500.0, 2000.0);
    CHECK(gainDb(bp, 1000.0, 1.5) == Approx(0.0).margin(0.05));
    CHECK(gainDb(bp, 500.0, 1.5) == Approx(-3.01).margin(0.1));
    CHECK(gainDb(bp, 2000.0, 1.5) == Approx(-3.01).margin(0.1));
    CHECK(gainDb(bp, 100.0, 1.5) < -35.0);
    const auto bs = scientific(kButterworth, kBandStop, 2, 800.0, 1250.0);
    CHECK(gainDb(bs, 1000.0, 2.0) < -40.0);
    CHECK(gainDb(bs, 800.0, 2.0) == Approx(-3.01).margin(0.1));
    CHECK(gainDb(bs, 1250.0, 2.0) == Approx(-3.01).margin(0.1));
    CHECK(gainDb(bs, 100.0) == Approx(0.0).margin(0.05));
    CHECK(gainDb(bs, 10000.0) == Approx(0.0).margin(0.05));
    // Premiere's defaults: Butterworth low pass of order 10 at 1440 Hz.
    const auto defaults = effectOf("wabcut.audio.scientificFilter");
    CHECK(gainDb(defaults, 1440.0, 1.5) == Approx(-3.01).margin(0.05));
    CHECK(gainDb(defaults, 2880.0, 1.5) == Approx(butterworthDb(10, 1440.0, 2880.0)).margin(0.2));
}

TEST_CASE("Scientific filter Chebychev keeps its ripple and ends the passband at the cutoff", "[audio][dsp][premiere][filter]") {
    const auto lp = scientific(kChebyshev, kLowPass, 5, 2000.0, 0.0, 1.0);
    CHECK(gainDb(lp, 2000.0, 1.5) == Approx(-1.0).margin(0.06));
    for (const double f : {100.0, 400.0, 700.0, 1000.0, 1300.0, 1600.0, 1900.0}) {
        INFO(f << " Hz");
        const double g = gainDb(lp, f, 1.5);
        CHECK(g <= 0.05);
        CHECK(g >= -1.06);
    }
    // Steeper than the Butterworth of the same order an octave up.
    CHECK(gainDb(lp, 4000.0, 1.5) < gainDb(scientific(kButterworth, kLowPass, 5, 2000.0), 4000.0, 1.5) - 10.0);
    const auto hp = scientific(kChebyshev, kHighPass, 4, 1000.0, 0.0, 0.5);
    CHECK(gainDb(hp, 1000.0, 1.5) == Approx(-0.5).margin(0.05));
    CHECK(gainDb(hp, 6000.0) >= -0.55);
    CHECK(gainDb(hp, 6000.0) <= 0.05);
}

TEST_CASE("Scientific filter elliptical reaches its stopband attenuation", "[audio][dsp][premiere][filter]") {
    const auto lp = scientific(kElliptical, kLowPass, 6, 1000.0, 0.0, 0.5, 60.0);
    CHECK(gainDb(lp, 1000.0, 1.5) == Approx(-0.5).margin(0.06));
    for (const double f : {200.0, 500.0, 800.0, 950.0}) {
        INFO(f << " Hz");
        const double g = gainDb(lp, f, 1.5);
        CHECK(g <= 0.05);
        CHECK(g >= -0.56);
    }
    for (const double f : {1500.0, 2000.0, 5000.0, 12000.0}) {
        INFO(f << " Hz");
        CHECK(gainDb(lp, f, 1.5) < -59.0);
    }
    const auto odd = scientific(kElliptical, kHighPass, 5, 2000.0, 0.0, 1.0, 50.0);
    CHECK(gainDb(odd, 2000.0, 1.5) == Approx(-1.0).margin(0.08));
    CHECK(gainDb(odd, 900.0, 1.5) < -49.0);
    CHECK(gainDb(odd, 8000.0) <= 0.05);
    CHECK(gainDb(odd, 8000.0) >= -1.06);
    const auto bs = scientific(kElliptical, kBandStop, 4, 900.0, 1100.0, 0.5, 40.0);
    CHECK(gainDb(bs, 1000.0, 2.0) < -39.0);
    CHECK(gainDb(bs, 300.0) >= -0.56);
    CHECK(gainDb(bs, 4000.0) >= -0.56);
}

TEST_CASE("Scientific filter Bessel is 3 dB down at the cutoff and falls smoothly", "[audio][dsp][premiere][filter]") {
    for (const int order : {2, 5, 8}) {
        INFO("order " << order);
        const auto lp = scientific(kBessel, kLowPass, order, 1000.0);
        CHECK(gainDb(lp, 1000.0, 1.5) == Approx(-3.01).margin(0.05));
        double previous = 1.0;
        for (const double f : {100.0, 300.0, 600.0, 1000.0, 2000.0, 4000.0}) {
            const double g = gainDb(lp, f, 1.5);
            CHECK(g < previous);
            previous = g;
        }
    }
    const auto hp = scientific(kBessel, kHighPass, 4, 1000.0);
    CHECK(gainDb(hp, 1000.0, 1.5) == Approx(-3.01).margin(0.05));
}

TEST_CASE("Scientific filter transition bandwidth mode picks the lowest order that meets Stop Attn", "[audio][dsp][premiere][filter]") {
    // Low pass at 1 kHz, 40 dB down by 2 kHz: Butterworth needs order 7, Chebychev (1 dB) 5, elliptical (1 dB) 3.
    for (const int type : {kButterworth, kChebyshev, kElliptical}) {
        INFO("type " << type);
        auto e = scientific(type, kLowPass, 30, 1000.0, 0.0, 1.0, 40.0);
        e.set("orderMode", ParamValue(0.0));
        e.set("transitionBandwidth", ParamValue(1000.0));
        const double stopEdge = gainDb(e, 2000.0, 1.5);
        CHECK(stopEdge < -40.0);
        CHECK(gainDb(e, 1000.0, 1.5) == Approx(type == kButterworth ? -3.01 : -1.0).margin(0.06));
        if (type != kElliptical) {
            // The lowest such order: no more than a few dB to spare at the stop edge (an order more adds 6 dB / octave).
            CHECK(stopEdge > -48.0);
        }
    }
    // Band pass from 1 to 2 kHz, 30 dB down 500 Hz beyond each edge.
    auto bp = scientific(kButterworth, kBandPass, 30, 1000.0, 2000.0, 1.0, 30.0);
    bp.set("orderMode", ParamValue(0.0));
    bp.set("transitionBandwidth", ParamValue(500.0));
    bp.set("highWidth", ParamValue(500.0));
    CHECK(gainDb(bp, 500.0, 1.5) < -30.0);
    CHECK(gainDb(bp, 2500.0, 1.5) < -30.0);
    CHECK(gainDb(bp, 1414.0, 1.5) == Approx(0.0).margin(0.05));
}

TEST_CASE("Scientific filter designs stay stable at every order", "[audio][dsp][premiere][filter]") {
    const auto x = noise(0.5, 0.25, 1, kRate);
    for (const int type : {kBessel, kButterworth, kChebyshev, kElliptical}) {
        for (int order = 1; order <= 32; ++order) {
            INFO("type " << type << " order " << order);
            for (const auto& [ripple, stop] : {std::pair{0.1, 100.0}, std::pair{3.0, 20.0}}) {
                const auto e = scientific(type, kLowPass, order, 2000.0, 0.0, ripple, stop);
                const double pass = gainDb(e, 250.0, 1.0);
                CHECK(pass <= 0.05);
                CHECK(pass >= -ripple - 0.2);
                const auto y = runEffect(e, x, 1, kRate);
                bool finite = true;
                for (const float v : y) {
                    finite = finite && std::isfinite(v) && std::fabs(v) < 8.0f;
                }
                CHECK(finite);
            }
        }
    }
}

TEST_CASE("Scientific filter gain scales the output", "[audio][dsp][premiere][filter]") {
    auto e = scientific(kButterworth, kLowPass, 2, 5000.0);
    e.set("gain", ParamValue(-6.0));
    CHECK(gainDb(e, 200.0) == Approx(-6.0).margin(0.02));
}

// ---- FFT Filter --------------------------------------------------------------------------------------------------

TEST_CASE("FFT filter delays by its FFT size and applies its curve", "[audio][dsp][premiere][filter]") {
    const auto flat = effectOf("wabcut.audio.fftFilter");
    const int latency = effectLatency(flat, 2);
    CHECK(latency == 4096);
    // Flat: the input, delayed.
    const auto in = noise(0.5, 0.5, 1, kRate);
    const auto out = runEffect(flat, in, 1, kRate);
    double worst = 0.0;
    for (std::size_t i = static_cast<std::size_t>(latency); i < in.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(out[i] - in[i - static_cast<std::size_t>(latency)])));
    }
    CHECK(worst < 1e-5);
    // A 30 dB cut from 4 to 8 kHz (straight lines in log frequency between the points; the pairs need not be sorted).
    const auto cut = with("wabcut.audio.fftFilter", {{"pairs", 6.0},
                                                     {"x1", 20.0}, {"y1", 0.0}, {"x2", 3000.0}, {"y2", 0.0},
                                                     {"x3", 8000.0}, {"y3", -30.0}, {"x4", 4000.0}, {"y4", -30.0},
                                                     {"x5", 10000.0}, {"y5", 0.0}, {"x6", 20000.0}, {"y6", 0.0}});
    CHECK(gainDb(cut, 6000.0) == Approx(-30.0).margin(1.0));
    CHECK(gainDb(cut, 500.0) == Approx(0.0).margin(0.1));
    CHECK(gainDb(cut, 15000.0) == Approx(0.0).margin(0.1));
    // Points past Number of X/Y pairs are ignored.
    const auto two = with("wabcut.audio.fftFilter", {{"y3", -30.0}, {"x3", 6000.0}});
    CHECK(gainDb(two, 6000.0) == Approx(0.0).margin(0.05));
    // A tilt: -12 dB at 100 Hz rising to 0 dB at 10 kHz, half way (-6 dB) at 1 kHz on the logarithmic scale.
    const auto tilt = with("wabcut.audio.fftFilter", {{"x1", 100.0}, {"y1", -12.0}, {"x2", 10000.0}, {"y2", 0.0}});
    CHECK(gainDb(tilt, 1000.0) == Approx(-6.0).margin(0.3));
    CHECK(gainDb(tilt, 50.0) == Approx(-12.0).margin(0.3));
    // The FFT size sets the latency (a power of two, 64 at least).
    CHECK(effectLatency(with("wabcut.audio.fftFilter", {{"fftSize", 1024.0}}), 1) == 1024);
    CHECK(effectLatency(with("wabcut.audio.fftFilter", {{"fftSize", 1000.0}}), 1) == 1024);
    CHECK(effectLatency(with("wabcut.audio.fftFilter", {{"fftSize", 32.0}}), 1) == 64);
}

TEST_CASE("Filter effects give the same samples whatever the block size", "[audio][dsp][premiere][filter]") {
    const auto x = noise(0.5, 0.4, 2, kRate);
    const std::vector<EffectInstance> effects{
        with("wabcut.audio.graphicEq30", {{"band10", 6.0}, {"band20", -9.0}, {"gain", -2.0}}),
        with("wabcut.audio.parametricEq", {{"highPassEnabled", 1.0}, {"band2Gain", 5.0}, {"lowShelfGain", 3.0}}),
        scientific(kElliptical, kBandPass, 4, 700.0, 1800.0),
        with("wabcut.audio.notchFilter", {{"notch1Enabled", 1.0}, {"notch1Frequency", 440.0}, {"notchWidth", 2.0}}),
        with("wabcut.audio.fftFilter", {{"y2", -20.0}, {"fftSize", 2048.0}, {"splines", 1.0}}),
        with("wabcut.audio.stereoExpander", {{"stereoExpand", 180.0}, {"centerPan", 30.0}}),
        with("wabcut.audio.channelVolume", {{"left", -3.0}, {"right", 2.0}}),
    };
    for (const EffectInstance& e : effects) {
        INFO(e.effect);
        CHECK(runEffect(e, x, 2, kRate, 512) == runEffect(e, x, 2, kRate, 37));
    }
}

// ---- Stereo Expander ---------------------------------------------------------------------------------------------

TEST_CASE("Stereo expander widens the side and pans the centre", "[audio][dsp][premiere][filter]") {
    CHECK(deviation(effectOf("wabcut.audio.stereoExpander")) == 0.0); // Normal (100) and centred: the input
    const auto s = sine(500.0, 0.25, 0.5, 1, kRate);
    std::vector<float> negative(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        negative[i] = -s[i];
    }
    const auto mid = stereo(s, s);         // L = R: all centre
    const auto side = stereo(s, negative); // L = -R: all side
    const std::size_t end = s.size();
    // Wide (300): three times the side; the centre is untouched.
    const auto wide = with("wabcut.audio.stereoExpander", {{"stereoExpand", 300.0}});
    const auto sideOut = runEffect(wide, side, 2, kRate);
    CHECK(amplitudeOf(sideOut, 2, 500.0, end, 0) == Approx(0.75).margin(1e-4));
    CHECK(amplitudeOf(sideOut, 2, 500.0, end, 1) == Approx(0.75).margin(1e-4));
    const auto midOut = runEffect(wide, mid, 2, kRate);
    CHECK(amplitudeOf(midOut, 2, 500.0, end, 0) == Approx(0.25).margin(1e-4));
    CHECK(amplitudeOf(midOut, 2, 500.0, end, 1) == Approx(0.25).margin(1e-4));
    // Narrow (0): mono, the side is gone.
    const auto narrow = runEffect(with("wabcut.audio.stereoExpander", {{"stereoExpand", 0.0}}), side, 2, kRate);
    CHECK(amplitudeOf(narrow, 2, 500.0, end, 0) < 1e-6);
    // Center Channel Pan -100: the centre goes left (equal power), the side stays.
    const auto left = with("wabcut.audio.stereoExpander", {{"centerPan", -100.0}});
    const auto panned = runEffect(left, mid, 2, kRate);
    CHECK(amplitudeOf(panned, 2, 500.0, end, 0) == Approx(0.25 * std::numbers::sqrt2).margin(1e-4));
    CHECK(amplitudeOf(panned, 2, 500.0, end, 1) < 1e-6);
    const auto sidePanned = runEffect(left, side, 2, kRate);
    CHECK(amplitudeOf(sidePanned, 2, 500.0, end, 0) == Approx(0.25).margin(1e-4));
    CHECK(amplitudeOf(sidePanned, 2, 500.0, end, 1) == Approx(0.25).margin(1e-4));
    // Mono is untouched.
    CHECK(deviation(wide, 1) == 0.0);
}

// ---- Channel effects ---------------------------------------------------------------------------------------------

TEST_CASE("Balance turns the other side down", "[audio][dsp][premiere][filter]") {
    const auto l = sine(300.0, 0.4, 0.25, 1, kRate);
    const auto r = sine(700.0, 0.4, 0.25, 1, kRate);
    const auto x = stereo(l, r);
    const std::size_t end = l.size();
    const auto right = runEffect(with("wabcut.audio.balance", {{"balance", 50.0}}), x, 2, kRate);
    CHECK(amplitudeOf(right, 2, 300.0, end, 0) == Approx(0.2).margin(1e-4));
    CHECK(amplitudeOf(right, 2, 700.0, end, 1) == Approx(0.4).margin(1e-4));
    const auto left = runEffect(with("wabcut.audio.balance", {{"balance", -100.0}}), x, 2, kRate);
    CHECK(amplitudeOf(left, 2, 300.0, end, 0) == Approx(0.4).margin(1e-4));
    CHECK(rmsOf(left, 2, 1, 0, end) == 0.0);
    CHECK(deviation(effectOf("wabcut.audio.balance")) == 0.0);
}

TEST_CASE("Channel volume sets each channel's level", "[audio][dsp][premiere][filter]") {
    const auto s = sine(440.0, 0.4, 0.25, 2, kRate);
    const auto out = runEffect(with("wabcut.audio.channelVolume", {{"left", -6.0}, {"right", 3.0}}), s, 2, kRate);
    const std::size_t end = s.size() / 2;
    CHECK(ampToDb(amplitudeOf(out, 2, 440.0, end, 0) / 0.4) == Approx(-6.0).margin(0.01));
    CHECK(ampToDb(amplitudeOf(out, 2, 440.0, end, 1) / 0.4) == Approx(3.0).margin(0.01));
    CHECK(deviation(effectOf("wabcut.audio.channelVolume")) == 0.0);
    // 5.1 (L R C LFE Ls Rs): each channel its own; -144 dB is off.
    const auto six = sine(440.0, 0.4, 0.25, 6, kRate);
    const auto surround = runEffect(with("wabcut.audio.channelVolume", {{"center", -12.0}, {"lfe", -144.0}, {"rightSurround", 6.0}}),
                                    six, 6, kRate);
    CHECK(ampToDb(amplitudeOf(surround, 6, 440.0, end, 0) / 0.4) == Approx(0.0).margin(0.01));
    CHECK(ampToDb(amplitudeOf(surround, 6, 440.0, end, 2) / 0.4) == Approx(-12.0).margin(0.01));
    CHECK(rmsOf(surround, 6, 3, 0, end) == 0.0);
    CHECK(ampToDb(amplitudeOf(surround, 6, 440.0, end, 4) / 0.4) == Approx(0.0).margin(0.01));
    CHECK(ampToDb(amplitudeOf(surround, 6, 440.0, end, 5) / 0.4) == Approx(6.0).margin(0.01));
    // Mono takes Left.
    const auto mono = runEffect(with("wabcut.audio.channelVolume", {{"left", -6.0}, {"right", 6.0}}), sine(440.0, 0.4, 0.25, 1, kRate), 1, kRate);
    CHECK(ampToDb(amplitudeOf(mono, 1, 440.0, end, 0) / 0.4) == Approx(-6.0).margin(0.01));
}

TEST_CASE("Fill swap and invert move the channels", "[audio][dsp][premiere][filter]") {
    const auto x = noise(0.5, 0.1, 2, kRate);
    const std::size_t frames = x.size() / 2;
    const auto fillLeft = runEffect(effectOf("wabcut.audio.fillLeftWithRight"), x, 2, kRate);
    const auto fillRight = runEffect(effectOf("wabcut.audio.fillRightWithLeft"), x, 2, kRate);
    const auto swapped = runEffect(effectOf("wabcut.audio.swapChannels"), x, 2, kRate);
    const auto inverted = runEffect(effectOf("wabcut.audio.invert"), x, 2, kRate);
    bool ok = true;
    for (std::size_t i = 0; i < frames; ++i) {
        const float l = x[2 * i];
        const float r = x[2 * i + 1];
        ok = ok && fillLeft[2 * i] == r && fillLeft[2 * i + 1] == r;
        ok = ok && fillRight[2 * i] == l && fillRight[2 * i + 1] == l;
        ok = ok && swapped[2 * i] == r && swapped[2 * i + 1] == l;
        ok = ok && inverted[2 * i] == -l && inverted[2 * i + 1] == -r;
    }
    CHECK(ok);
    // Mono: fill and swap leave it, invert inverts it.
    const auto m = noise(0.5, 0.05, 1, kRate);
    CHECK(runEffect(effectOf("wabcut.audio.swapChannels"), m, 1, kRate) == m);
    CHECK(runEffect(effectOf("wabcut.audio.fillLeftWithRight"), m, 1, kRate) == m);
    const auto mi = runEffect(effectOf("wabcut.audio.invert"), m, 1, kRate);
    CHECK(mi[10] == -m[10]);
    // 5.1: only the front pair moves.
    const auto six = noise(0.5, 0.05, 6, kRate);
    const auto six2 = runEffect(effectOf("wabcut.audio.swapChannels"), six, 6, kRate);
    CHECK(six2[0] == six[1]);
    CHECK(six2[1] == six[0]);
    CHECK(six2[2] == six[2]);
    CHECK(six2[5] == six[5]);
}
