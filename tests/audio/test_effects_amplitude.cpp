// p02 audio: Premiere Pro's Amplitude family of audio effects (Amplitude and Compression, Special, Time and Pitch),
// measured offline through an EffectChain: pitch by zero crossings and DFT, static compressor curves on steady tones,
// limiter ceilings, crossover flatness, harmonics added by the saturating effects, and the identity at neutral settings.

#include "DspTestUtil.h"

#include <wabcut/model/Parameters.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
constexpr std::size_t kFrames1s = 48000;

/// Frequency from the rising zero crossings (linearly interpolated) of `channel` over frames [first, last).
double zeroCrossingFrequency(const std::vector<float>& x, int channels, std::size_t first, std::size_t last, int channel = 0) {
    const auto ch = static_cast<std::size_t>(channels);
    const auto c = static_cast<std::size_t>(channel);
    double firstCrossing = 0.0;
    double lastCrossing = 0.0;
    int count = 0;
    for (std::size_t i = first + 1; i < last; ++i) {
        const double a = x[(i - 1) * ch + c];
        const double b = x[i * ch + c];
        if (a < 0.0 && b >= 0.0) {
            const double t = static_cast<double>(i - 1) + a / (a - b);
            if (count == 0) {
                firstCrossing = t;
            }
            lastCrossing = t;
            ++count;
        }
    }
    return count > 1 ? (count - 1) * kRate / (lastCrossing - firstCrossing) : 0.0;
}

/// Level (dBFS) of the `frequency` component of `channel` over frames [first, last).
double toneDb(const std::vector<float>& x, int channels, double frequency, std::size_t first, std::size_t last, int channel = 0) {
    return ampToDb(toneAmplitude(x, channels, frequency, first, last, channel));
}

/// Interleaves two mono signals of the same length.
std::vector<float> stereo(const std::vector<float>& left, const std::vector<float>& right) {
    std::vector<float> out(left.size() * 2);
    for (std::size_t i = 0; i < left.size(); ++i) {
        out[2 * i] = left[i];
        out[2 * i + 1] = right[i];
    }
    return out;
}

/// Largest |y[n] - x[n - delay]| over the frames after `delay`.
double delayError(const std::vector<float>& x, const std::vector<float>& y, int channels, int delay) {
    const auto ch = static_cast<std::size_t>(channels);
    const auto d = static_cast<std::size_t>(delay);
    double worst = 0.0;
    for (std::size_t i = d * ch; i < y.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(y[i] - x[i - d * ch])));
    }
    return worst;
}

/// First and last frame where |channel 0| exceeds `level`.
std::pair<std::size_t, std::size_t> extent(const std::vector<float>& x, int channels, double level) {
    const auto ch = static_cast<std::size_t>(channels);
    std::size_t first = SIZE_MAX;
    std::size_t last = 0;
    for (std::size_t i = 0; i < x.size() / ch; ++i) {
        if (std::fabs(x[i * ch]) > level) {
            first = std::min(first, i);
            last = i;
        }
    }
    return {first, last};
}

double mean(const std::vector<float>& x, int channels, std::size_t first, std::size_t last) {
    double sum = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        sum += x[i * static_cast<std::size_t>(channels)];
    }
    return sum / static_cast<double>(last - first);
}

const std::string kPitchShifter = "wabcut.audio.pitchShifter";
const std::string kMultiband = "wabcut.audio.multibandCompressor";
const std::string kHardLimiter = "wabcut.audio.hardLimiter";
const std::string kSingleBand = "wabcut.audio.singleBandCompressor";
const std::string kTube = "wabcut.audio.tubeModeledCompressor";
const std::string kDynamics = "wabcut.audio.dynamics";
const std::string kDistortion = "wabcut.audio.distortion";
const std::string kChannelMixer = "wabcut.audio.channelMixer";
const std::string kAmplify = "wabcut.audio.amplify";
const std::string kVocalEnhancer = "wabcut.audio.vocalEnhancer";
const std::string kMastering = "wabcut.audio.mastering";
const std::string kGuitarSuite = "wabcut.audio.guitarSuite";

} // namespace

// ---- Pitch Shifter --------------------------------------------------------------------------------------------------

TEST_CASE("Pitch Shifter raises a 440 Hz tone by an octave and keeps its length", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kPitchShifter, {{"semitones", ParamValue(12.0)}});
    const int latency = effectLatency(e, 2);
    CHECK(latency > 0);
    CHECK(latency < kRate / 20); // under 50 ms at the default (High) precision
    const std::vector<float> x = sine(440.0, 0.5, 2.0);
    const std::vector<float> y = runEffect(e, x, 2);
    REQUIRE(y.size() == x.size());
    const std::size_t first = kFrames1s / 2;
    const std::size_t last = 2 * kFrames1s;
    CHECK(zeroCrossingFrequency(y, 2, first, last) == Approx(880.0).epsilon(0.002));
    CHECK(zeroCrossingFrequency(y, 2, first, last, 1) == Approx(880.0).epsilon(0.002));
    CHECK(toneAmplitude(y, 2, 880.0, first, last) > 0.4);
    CHECK(toneAmplitude(y, 2, 440.0, first, last) < 0.01);
}

TEST_CASE("Pitch Shifter lowers a tone by five semitones", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kPitchShifter, {{"semitones", ParamValue(-5.0)}});
    const std::vector<float> y = runEffect(e, sine(440.0, 0.5, 2.0), 2);
    const double expected = 440.0 * std::exp2(-5.0 / 12.0); // 329.63 Hz
    CHECK(zeroCrossingFrequency(y, 2, kFrames1s / 2, 2 * kFrames1s) == Approx(expected).epsilon(0.002));
    CHECK(toneAmplitude(y, 2, expected, kFrames1s / 2, 2 * kFrames1s) > 0.4);
    CHECK(toneAmplitude(y, 2, 440.0, kFrames1s / 2, 2 * kFrames1s) < 0.01);
}

TEST_CASE("Pitch Shifter Cents and Ratio set the shift too", "[audio][dsp][premiere][amplitude]") {
    const std::vector<float> x = sine(440.0, 0.5, 1.5, 1);
    const std::size_t first = kFrames1s / 2;
    const std::size_t last = 3 * kFrames1s / 2;
    // +100 cents is one semitone.
    const std::vector<float> up = runEffect(effectOf(kPitchShifter, {{"cents", ParamValue(100.0)}}), x, 1);
    CHECK(zeroCrossingFrequency(up, 1, first, last) == Approx(440.0 * std::exp2(1.0 / 12.0)).epsilon(0.002));
    // Ratio applies when Semi-tones and Cents are both 0 ...
    const std::vector<float> fifth = runEffect(effectOf(kPitchShifter, {{"ratio", ParamValue(1.5)}}), x, 1);
    CHECK(zeroCrossingFrequency(fifth, 1, first, last) == Approx(660.0).epsilon(0.002));
    // ... and is only their display otherwise.
    const std::vector<float> octave =
        runEffect(effectOf(kPitchShifter, {{"semitones", ParamValue(12.0)}, {"ratio", ParamValue(0.5)}}), x, 1);
    CHECK(zeroCrossingFrequency(octave, 1, first, last) == Approx(880.0).epsilon(0.002));
}

TEST_CASE("Pitch Shifter without a shift is a pure delay of its latency", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kPitchShifter);
    const int latency = effectLatency(e, 2);
    REQUIRE(latency > 0);
    const std::vector<float> x = noise(0.5, 1.0);
    const std::vector<float> y = runEffect(e, x, 2);
    CHECK(delayError(x, y, 2, latency) < 1e-6);
    CHECK(peak(y, 2, 0, static_cast<std::size_t>(latency)) == 0.0); // nothing comes out before the latency
}

TEST_CASE("Pitch Shifter keeps a tone burst at its time and length", "[audio][dsp][premiere][amplitude]") {
    // 0.4 s of 500 Hz starting at 0.5 s, shifted up a fifth: it comes out 0.4 s long, at 0.5 s plus the latency.
    std::vector<float> x(3 * kFrames1s / 2, 0.0f);
    for (std::size_t i = kFrames1s / 2; i < kFrames1s / 2 + 2 * kFrames1s / 5; ++i) {
        x[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 500.0 * static_cast<double>(i) / kRate));
    }
    const EffectInstance e = effectOf(kPitchShifter, {{"semitones", ParamValue(7.0)}});
    const auto latency = static_cast<std::size_t>(effectLatency(e, 1));
    const std::vector<float> y = runEffect(e, x, 1);
    const auto [first, last] = extent(y, 1, 0.25);
    const std::size_t tolerance = kFrames1s / 40; // 25 ms (the splices smear an edge by about half a splice)
    CHECK(first + tolerance >= kFrames1s / 2 + latency);
    CHECK(first <= kFrames1s / 2 + latency + tolerance);
    CHECK(last + tolerance >= kFrames1s / 2 + 2 * kFrames1s / 5 + latency);
    CHECK(last <= kFrames1s / 2 + 2 * kFrames1s / 5 + latency + tolerance);
    const double expected = 500.0 * std::exp2(7.0 / 12.0);
    CHECK(zeroCrossingFrequency(y, 1, kFrames1s / 2 + latency + 4800, kFrames1s / 2 + latency + 14400) ==
          Approx(expected).epsilon(0.003));
}

TEST_CASE("Pitch Shifter Precision trades latency for quality", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance low = effectOf(kPitchShifter, {{"semitones", ParamValue(12.0)}, {"precision", ParamValue(0.0)}});
    const EffectInstance medium = effectOf(kPitchShifter, {{"semitones", ParamValue(12.0)}, {"precision", ParamValue(1.0)}});
    const EffectInstance high = effectOf(kPitchShifter, {{"semitones", ParamValue(12.0)}, {"precision", ParamValue(2.0)}});
    CHECK(effectLatency(low, 2) < effectLatency(medium, 2));
    CHECK(effectLatency(medium, 2) < effectLatency(high, 2));
    const std::vector<float> x = sine(440.0, 0.5, 1.5, 1);
    for (const EffectInstance* e : {&low, &medium}) {
        const std::vector<float> y = runEffect(*e, x, 1);
        CHECK(zeroCrossingFrequency(y, 1, kFrames1s / 2, 3 * kFrames1s / 2) == Approx(880.0).epsilon(0.002));
    }
    // Explicit splicing settings (Use Appropriate Default Settings off) still shift by the ratio asked.
    const EffectInstance custom = effectOf(kPitchShifter, {{"semitones", ParamValue(-12.0)},
                                                           {"useDefaultSettings", ParamValue(0.0)},
                                                           {"splicingFrequency", ParamValue(50.0)},
                                                           {"overlapping", ParamValue(50.0)}});
    const std::vector<float> y = runEffect(custom, x, 1);
    CHECK(zeroCrossingFrequency(y, 1, kFrames1s / 2, 3 * kFrames1s / 2) == Approx(220.0).epsilon(0.002));
}

// ---- Multiband Compressor ---------------------------------------------------------------------------------------

TEST_CASE("Multiband Compressor bands sum back flat when nothing compresses", "[audio][dsp][premiere][amplitude]") {
    // -30 dBFS is below every band's default threshold (-18 dB); Margin 0 makes the limiter's raise 0 dB.
    const EffectInstance e = effectOf(kMultiband, {{"limiterMargin", ParamValue(0.0)}});
    CHECK(effectLatency(e, 2) == 240); // the brickwall limiter's 5 ms
    for (const double f : {40.0, 120.0, 500.0, 2000.0, 5000.0, 10000.0, 16000.0}) {
        INFO(f << " Hz");
        const std::vector<float> y = runEffect(e, sine(f, dbToAmp(-30.0), 1.0), 2);
        CHECK(toneDb(y, 2, f, kFrames1s / 2, kFrames1s) == Approx(-30.0).margin(0.02));
    }
}

TEST_CASE("Multiband Compressor compresses only the band a tone falls in", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kMultiband, {{"band1Threshold", ParamValue(0.0)},
                                                   {"band2Threshold", ParamValue(-30.0)},
                                                   {"band2Ratio", ParamValue(4.0)},
                                                   {"band3Threshold", ParamValue(0.0)},
                                                   {"band4Threshold", ParamValue(0.0)},
                                                   {"limiterMargin", ParamValue(0.0)}});
    // 500 Hz (band 2, 120..2000 Hz) at -6 dBFS: 24 dB over, 4:1 -> 6 dB over -> -24 dBFS.
    const std::vector<float> low = runEffect(e, sine(500.0, dbToAmp(-6.0), 1.0), 2);
    CHECK(toneDb(low, 2, 500.0, kFrames1s / 2, kFrames1s) == Approx(-24.0).margin(0.5));
    // 5 kHz is in band 3, which is not compressing.
    const std::vector<float> high = runEffect(e, sine(5000.0, dbToAmp(-6.0), 1.0), 2);
    CHECK(toneDb(high, 2, 5000.0, kFrames1s / 2, kFrames1s) == Approx(-6.0).margin(0.1));
    // Band 2's Gain is its make-up.
    const EffectInstance louder = effectOf(kMultiband, {{"band2Gain", ParamValue(6.0)}, {"limiterMargin", ParamValue(0.0)}});
    const std::vector<float> made = runEffect(louder, sine(500.0, dbToAmp(-30.0), 1.0), 2);
    CHECK(toneDb(made, 2, 500.0, kFrames1s / 2, kFrames1s) == Approx(-24.0).margin(0.1));
}

TEST_CASE("Multiband Compressor Solo and Bypass", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance solo = effectOf(kMultiband, {{"band4Solo", ParamValue(1.0)}, {"limiterMargin", ParamValue(0.0)}});
    const std::vector<float> bass = runEffect(solo, sine(100.0, dbToAmp(-30.0), 1.0), 2);
    CHECK(toneDb(bass, 2, 100.0, kFrames1s / 2, kFrames1s) < -100.0);
    const std::vector<float> air = runEffect(solo, sine(20000.0, dbToAmp(-30.0), 1.0), 2); // well inside band 4
    CHECK(toneDb(air, 2, 20000.0, kFrames1s / 2, kFrames1s) == Approx(-30.0).margin(0.05));
    // A bypassed band neither compresses nor applies its gain.
    const EffectInstance bypass = effectOf(kMultiband, {{"band2Threshold", ParamValue(-40.0)},
                                                        {"band2Ratio", ParamValue(30.0)},
                                                        {"band2Gain", ParamValue(12.0)},
                                                        {"band2Bypass", ParamValue(1.0)},
                                                        {"limiterMargin", ParamValue(0.0)}});
    const std::vector<float> mid = runEffect(bypass, sine(500.0, dbToAmp(-30.0), 1.0), 2);
    CHECK(toneDb(mid, 2, 500.0, kFrames1s / 2, kFrames1s) == Approx(-30.0).margin(0.05));
}

TEST_CASE("Multiband Compressor brickwall limiter holds the Margin", "[audio][dsp][premiere][amplitude]") {
    std::vector<EffectParam> params{{"outputGain", ParamValue(18.0)},
                                    {"limiterThreshold", ParamValue(-12.0)},
                                    {"limiterMargin", ParamValue(-3.0)}};
    for (const char* key : {"band1Gain", "band2Gain", "band3Gain", "band4Gain"}) {
        params.push_back({key, ParamValue(18.0)});
    }
    const EffectInstance e = effectOf(kMultiband, params);
    const std::vector<float> y = runEffect(e, noise(0.5, 1.0), 2);
    CHECK(peak(y, 2) <= dbToAmp(-3.0) + 1e-6);
    CHECK(peak(y, 2, kFrames1s / 2) > dbToAmp(-4.0)); // driven into the limiter
    // Brickwall off: no lookahead, no latency.
    params.push_back({"brickwall", ParamValue(0.0)});
    CHECK(effectLatency(effectOf(kMultiband, params), 2) == 0);
}

// ---- Hard Limiter -----------------------------------------------------------------------------------------------

TEST_CASE("Hard Limiter keeps a +6 dB sine under Maximum Amplitude", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kHardLimiter);
    CHECK(effectLatency(e, 2) == 336); // the default 7 ms look-ahead
    const std::vector<float> y = runEffect(e, sine(1000.0, 2.0, 1.0), 2);
    const double ceiling = dbToAmp(-0.1);
    CHECK(peak(y, 2) <= ceiling + 1e-6);
    CHECK(toneAmplitude(y, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(ceiling).epsilon(0.01));
    // Lower ceiling, louder input: still a brick wall.
    const EffectInstance hard = effectOf(kHardLimiter, {{"maximumAmplitude", ParamValue(-12.0)}, {"inputBoost", ParamValue(12.0)}});
    const std::vector<float> z = runEffect(hard, noise(1.0, 1.0), 2);
    CHECK(peak(z, 2) <= dbToAmp(-12.0) + 1e-6);
}

TEST_CASE("Hard Limiter Input Boost raises signals under the ceiling", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kHardLimiter, {{"inputBoost", ParamValue(10.0)}});
    const std::vector<float> y = runEffect(e, sine(1000.0, dbToAmp(-20.0), 1.0), 2);
    CHECK(toneDb(y, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(-10.0).margin(0.01));
}

TEST_CASE("Hard Limiter Link Channels shares one gain", "[audio][dsp][premiere][amplitude]") {
    const std::vector<float> x = stereo(sine(1000.0, 2.0, 1.0, 1), sine(1000.0, 0.1, 1.0, 1));
    const std::vector<float> linked = runEffect(effectOf(kHardLimiter), x, 2);
    const double gain = dbToAmp(-0.1) / 2.0;
    CHECK(toneAmplitude(linked, 2, 1000.0, kFrames1s / 2, kFrames1s, 1) == Approx(0.1 * gain).epsilon(0.02));
    const std::vector<float> apart = runEffect(effectOf(kHardLimiter, {{"linkChannels", ParamValue(0.0)}}), x, 2);
    CHECK(toneAmplitude(apart, 2, 1000.0, kFrames1s / 2, kFrames1s, 1) == Approx(0.1).epsilon(0.001));
    CHECK(toneAmplitude(apart, 2, 1000.0, kFrames1s / 2, kFrames1s, 0) == Approx(dbToAmp(-0.1)).epsilon(0.01));
}

// ---- Single-band and Tube-modeled Compressor ---------------------------------------------------------------------

TEST_CASE("Single-band Compressor settles on its static curve", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kSingleBand); // -20 dB, 4:1
    const std::vector<float> loud = runEffect(e, sine(1000.0, 1.0, 1.0), 2);
    CHECK(toneDb(loud, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(-15.0).margin(0.3));
    const std::vector<float> quiet = runEffect(e, sine(1000.0, dbToAmp(-30.0), 1.0), 2);
    CHECK(toneDb(quiet, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(-30.0).margin(0.01));
    const EffectInstance gentle = effectOf(kSingleBand, {{"threshold", ParamValue(-30.0)}, {"ratio", ParamValue(2.0)}});
    const std::vector<float> half = runEffect(gentle, sine(1000.0, dbToAmp(-10.0), 1.0), 2);
    CHECK(toneDb(half, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(-20.0).margin(0.3));
    const EffectInstance boosted = effectOf(kSingleBand, {{"outputGain", ParamValue(6.0)}});
    const std::vector<float> up = runEffect(boosted, sine(1000.0, dbToAmp(-30.0), 1.0), 2);
    CHECK(toneDb(up, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(-24.0).margin(0.01));
}

TEST_CASE("Single-band Compressor releases over its Release time", "[audio][dsp][premiere][amplitude]") {
    // 0.5 s at 0 dBFS, then 1 s at -30 dBFS: 50 ms after the drop most of the 15 dB reduction remains; 1 s after, none.
    std::vector<float> x = sine(1000.0, 1.0, 1.5, 1);
    for (std::size_t i = kFrames1s / 2; i < x.size(); ++i) {
        x[i] *= static_cast<float>(dbToAmp(-30.0));
    }
    const std::vector<float> y = runEffect(effectOf(kSingleBand), x, 1);
    const std::size_t drop = kFrames1s / 2;
    CHECK(toneDb(y, 1, 1000.0, drop + 2400, drop + 2880) < -33.0);
    CHECK(toneDb(y, 1, 1000.0, drop + kFrames1s - 960, drop + kFrames1s) == Approx(-30.0).margin(0.05));
}

TEST_CASE("Tube-modeled Compressor colours only while it compresses", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kTube); // -20 dB, 4:1
    const std::vector<float> loud = runEffect(e, sine(1000.0, 1.0, 1.0), 2);
    const double fundamental = toneDb(loud, 2, 1000.0, kFrames1s / 2, kFrames1s);
    CHECK(fundamental == Approx(-15.0).margin(1.0));
    CHECK(toneDb(loud, 2, 2000.0, kFrames1s / 2, kFrames1s) - fundamental > -45.0);
    CHECK(toneDb(loud, 2, 3000.0, kFrames1s / 2, kFrames1s) - fundamental > -70.0);
    const std::vector<float> x = sine(1000.0, dbToAmp(-40.0), 1.0);
    const std::vector<float> quiet = runEffect(e, x, 2);
    CHECK(toneDb(quiet, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(-40.0).margin(0.01));
    CHECK(toneDb(quiet, 2, 2000.0, kFrames1s / 2, kFrames1s) < -150.0);
    CHECK(toneDb(quiet, 2, 3000.0, kFrames1s / 2, kFrames1s) < -150.0);
}

// ---- Dynamics ----------------------------------------------------------------------------------------------------

TEST_CASE("Dynamics with every section off is the identity", "[audio][dsp][premiere][amplitude]") {
    const std::vector<float> x = noise(0.8, 0.5);
    CHECK(delayError(x, runEffect(effectOf(kDynamics), x, 2), 2, 0) == 0.0);
}

TEST_CASE("Dynamics AutoGate mutes below its threshold and opens above it", "[audio][dsp][premiere][amplitude]") {
    std::vector<float> x = sine(1000.0, dbToAmp(-50.0), 1.0, 1);
    for (std::size_t i = kFrames1s / 2; i < x.size(); ++i) {
        x[i] *= static_cast<float>(dbToAmp(40.0)); // -10 dBFS from 0.5 s
    }
    const EffectInstance e = effectOf(kDynamics, {{"gateEnable", ParamValue(1.0)}, {"gateThreshold", ParamValue(-30.0)}});
    const std::vector<float> y = runEffect(e, x, 1);
    CHECK(peak(y, 1, 0, kFrames1s / 2) == 0.0);
    CHECK(toneDb(y, 1, 1000.0, 3 * kFrames1s / 5, kFrames1s) == Approx(-10.0).margin(0.01));
    // After the tone stops, Hold (50 ms) then Release (150 ms) close the gate.
    std::vector<float> z = sine(1000.0, dbToAmp(-10.0), 1.0, 1);
    for (std::size_t i = kFrames1s / 2; i < z.size(); ++i) {
        z[i] = static_cast<float>(dbToAmp(-50.0) * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate));
    }
    const std::vector<float> w = runEffect(e, z, 1);
    CHECK(toneDb(w, 1, 1000.0, kFrames1s / 2 + 960, kFrames1s / 2 + 1920) == Approx(-50.0).margin(0.1)); // holding
    CHECK(peak(w, 1, kFrames1s / 2 + 12000) == 0.0);                                                     // closed
}

TEST_CASE("Dynamics Compressor follows its curve plus MakeUp", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kDynamics, {{"compressorEnable", ParamValue(1.0)},
                                                  {"compressorThreshold", ParamValue(-20.0)},
                                                  {"compressorRatio", ParamValue(4.0)},
                                                  {"makeUp", ParamValue(3.0)}});
    const std::vector<float> y = runEffect(e, sine(1000.0, 1.0, 1.0), 2);
    CHECK(toneDb(y, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(-12.0).margin(0.3));
    // Auto release: the same steady curve.
    const EffectInstance automatic = effectOf(kDynamics, {{"compressorEnable", ParamValue(1.0)},
                                                          {"compressorThreshold", ParamValue(-20.0)},
                                                          {"compressorRatio", ParamValue(4.0)},
                                                          {"compressorAuto", ParamValue(1.0)}});
    const std::vector<float> z = runEffect(automatic, sine(1000.0, 1.0, 2.0), 2);
    CHECK(toneDb(z, 2, 1000.0, 3 * kFrames1s / 2, 2 * kFrames1s) == Approx(-15.0).margin(0.3));
}

TEST_CASE("Dynamics Expander pushes quiet signals down by its ratio", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kDynamics, {{"expanderEnable", ParamValue(1.0)},
                                                  {"expanderThreshold", ParamValue(-40.0)},
                                                  {"expanderRatio", ParamValue(2.0)}});
    const std::vector<float> quiet = runEffect(e, sine(1000.0, dbToAmp(-50.0), 1.0), 2);
    CHECK(toneDb(quiet, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(-60.0).margin(1.0));
    const std::vector<float> loud = runEffect(e, sine(1000.0, dbToAmp(-30.0), 1.0), 2);
    CHECK(toneDb(loud, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(-30.0).margin(0.01));
}

TEST_CASE("Dynamics Limiter holds its threshold with or without Soft Clip", "[audio][dsp][premiere][amplitude]") {
    const std::vector<float> x = sine(1000.0, 2.0, 1.0);
    const EffectInstance hard = effectOf(kDynamics, {{"limiterEnable", ParamValue(1.0)}, {"limiterThreshold", ParamValue(-6.0)}});
    const std::vector<float> y = runEffect(hard, x, 2);
    CHECK(peak(y, 2) <= dbToAmp(-6.0) + 1e-6);
    CHECK(toneDb(y, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(-6.0).margin(0.2));
    const EffectInstance soft = effectOf(kDynamics, {{"limiterEnable", ParamValue(1.0)},
                                                     {"limiterThreshold", ParamValue(-6.0)},
                                                     {"softClip", ParamValue(1.0)}});
    const std::vector<float> z = runEffect(soft, x, 2);
    CHECK(peak(z, 2) <= dbToAmp(-6.0) + 1e-6);
    CHECK(toneDb(z, 2, 3000.0, kFrames1s / 2, kFrames1s) - toneDb(z, 2, 1000.0, kFrames1s / 2, kFrames1s) > -30.0);
    // Below the threshold, Soft Clip leaves half the threshold and less untouched.
    const std::vector<float> small = sine(1000.0, dbToAmp(-14.0), 0.5);
    CHECK(delayError(small, runEffect(soft, small, 2), 2, 0) < 1e-7);
}

// ---- Distortion ---------------------------------------------------------------------------------------------------

TEST_CASE("Distortion with symmetric curves adds odd harmonics only", "[audio][dsp][premiere][amplitude]") {
    const std::vector<float> y = runEffect(effectOf(kDistortion), sine(1000.0, 0.5, 1.0), 2);
    const double fundamental = toneDb(y, 2, 1000.0, kFrames1s / 2, kFrames1s);
    CHECK(toneDb(y, 2, 3000.0, kFrames1s / 2, kFrames1s) - fundamental > -40.0);
    CHECK(toneDb(y, 2, 2000.0, kFrames1s / 2, kFrames1s) - fundamental < -80.0);
    CHECK(peak(y, 2) < 1.5);
    // Curve Smoothing rounds the knee: fewer high harmonics than the straight segments.
    const std::vector<float> s = runEffect(effectOf(kDistortion, {{"curveSmoothing", ParamValue(1.0)}}), sine(1000.0, 0.5, 1.0), 2);
    CHECK(toneDb(s, 2, 9000.0, kFrames1s / 2, kFrames1s) < toneDb(y, 2, 9000.0, kFrames1s / 2, kFrames1s));
}

TEST_CASE("Distortion asymmetric curves add even harmonics and DC the post filter removes", "[audio][dsp][premiere][amplitude]") {
    std::vector<EffectParam> params{{"symmetric", ParamValue(0.0)}, {"positive", ParamValue(100.0)}, {"negative", ParamValue(0.0)}};
    const std::vector<float> y = runEffect(effectOf(kDistortion, params), sine(1000.0, 0.5, 1.0), 2);
    const double fundamental = toneDb(y, 2, 1000.0, kFrames1s / 2, kFrames1s);
    CHECK(toneDb(y, 2, 2000.0, kFrames1s / 2, kFrames1s) - fundamental > -35.0);
    CHECK(std::fabs(mean(y, 2, kFrames1s / 2, kFrames1s)) < 1e-3);
    params.push_back({"postFilterDcOffset", ParamValue(0.0)});
    const std::vector<float> raw = runEffect(effectOf(kDistortion, params), sine(1000.0, 0.5, 1.0), 2);
    CHECK(std::fabs(mean(raw, 2, kFrames1s / 2, kFrames1s)) > 1e-2);
}

TEST_CASE("Distortion with straight curves passes the signal", "[audio][dsp][premiere][amplitude]") {
    const std::vector<float> x = noise(0.9, 0.5);
    const EffectInstance flat = effectOf(kDistortion, {{"positive", ParamValue(0.0)}, {"postFilterDcOffset", ParamValue(0.0)}});
    CHECK(delayError(x, runEffect(flat, x, 2), 2, 0) == 0.0);
    // The post filter alone is a 10 Hz high-pass: a 1 kHz tone keeps its level.
    const std::vector<float> y = runEffect(effectOf(kDistortion, {{"positive", ParamValue(0.0)}}), sine(1000.0, 0.5, 1.0), 2);
    CHECK(toneDb(y, 2, 1000.0, kFrames1s / 2, kFrames1s) == Approx(ampToDb(0.5)).margin(0.01));
    // Linear Scale and Time Smoothing still drive harder with a bigger curve.
    const std::vector<float> lin = runEffect(
        effectOf(kDistortion, {{"linearScale", ParamValue(1.0)}, {"timeSmoothing", ParamValue(50.0)}, {"positive", ParamValue(80.0)}}),
        sine(1000.0, 0.1, 1.0), 2);
    CHECK(toneDb(lin, 2, 1000.0, kFrames1s / 2, kFrames1s) > ampToDb(0.1) + 6.0);
}

// ---- Channel Mixer and Amplify -----------------------------------------------------------------------------------

TEST_CASE("Channel Mixer builds each output from percentages of the inputs", "[audio][dsp][premiere][amplitude]") {
    const std::vector<float> x = stereo(sine(500.0, 0.5, 1.0, 1), sine(1500.0, 0.25, 1.0, 1));
    const EffectInstance e = effectOf(kChannelMixer, {{"leftFromLeft", ParamValue(50.0)},
                                                      {"leftFromRight", ParamValue(50.0)},
                                                      {"rightFromLeft", ParamValue(0.0)},
                                                      {"rightFromRight", ParamValue(100.0)},
                                                      {"invertRight", ParamValue(1.0)}});
    const std::vector<float> y = runEffect(e, x, 2);
    CHECK(toneAmplitude(y, 2, 500.0, 0, kFrames1s, 0) == Approx(0.25).epsilon(1e-4));
    CHECK(toneAmplitude(y, 2, 1500.0, 0, kFrames1s, 0) == Approx(0.125).epsilon(1e-4));
    double worst = 0.0;
    for (std::size_t i = 0; i < kFrames1s; ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(y[2 * i + 1] + x[2 * i + 1])));
    }
    CHECK(worst < 1e-7); // the right output is the inverted right input
    // The defaults (L = 100 % L, R = 100 % R) and mono are the identity.
    CHECK(delayError(x, runEffect(effectOf(kChannelMixer), x, 2), 2, 0) == 0.0);
    const std::vector<float> m = noise(0.5, 0.5, 1);
    CHECK(delayError(m, runEffect(e, m, 1), 1, 0) == 0.0);
}

TEST_CASE("Amplify sets each channel's gain and Link ties them", "[audio][dsp][premiere][amplitude]") {
    const std::vector<float> x = sine(1000.0, 0.25, 1.0);
    const std::vector<float> y =
        runEffect(effectOf(kAmplify, {{"leftGain", ParamValue(6.0)}, {"rightGain", ParamValue(-12.0)}}), x, 2);
    CHECK(toneDb(y, 2, 1000.0, 0, kFrames1s, 0) == Approx(ampToDb(0.25) + 6.0).margin(0.001));
    CHECK(toneDb(y, 2, 1000.0, 0, kFrames1s, 1) == Approx(ampToDb(0.25) - 12.0).margin(0.001));
    const std::vector<float> z = runEffect(
        effectOf(kAmplify, {{"leftGain", ParamValue(6.0)}, {"rightGain", ParamValue(-12.0)}, {"link", ParamValue(1.0)}}), x, 2);
    CHECK(toneDb(z, 2, 1000.0, 0, kFrames1s, 1) == Approx(ampToDb(0.25) + 6.0).margin(0.001));
    CHECK(delayError(x, runEffect(effectOf(kAmplify), x, 2), 2, 0) == 0.0);
}

// ---- Vocal Enhancer, Mastering, Guitar Suite -----------------------------------------------------------------------

TEST_CASE("Vocal Enhancer modes shape the voice band", "[audio][dsp][premiere][amplitude]") {
    // Quiet tones (under the gentle compressor) read the voicing EQ.
    const auto gainAt = [](double mode, double f) {
        const std::vector<float> y = runEffect(effectOf(kVocalEnhancer, {{"mode", ParamValue(mode)}}), sine(f, dbToAmp(-40.0), 1.0), 2);
        return toneDb(y, 2, f, kFrames1s / 2, kFrames1s) + 40.0;
    };
    CHECK(gainAt(0.0, 3000.0) == Approx(3.0).margin(1.0));   // Male: presence
    CHECK(gainAt(0.0, 300.0) == Approx(-3.0).margin(1.0));   // Male: mud
    CHECK(gainAt(0.0, 40.0) < -9.0);                          // Male: rumble
    CHECK(gainAt(1.0, 4500.0) == Approx(3.0).margin(1.0));   // Female: presence
    CHECK(gainAt(1.0, 400.0) == Approx(-3.0).margin(1.0));   // Female: mud
    CHECK(gainAt(2.0, 2500.0) == Approx(-4.0).margin(1.0));  // Music: the voice band carved out
    CHECK(gainAt(2.0, 200.0) == Approx(0.0).margin(1.0));
}

TEST_CASE("Mastering at its defaults is a pure delay", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance e = effectOf(kMastering);
    const int latency = effectLatency(e, 2);
    CHECK(latency == 144); // the maximizer's 3 ms look-ahead
    const std::vector<float> x = noise(0.5, 1.0);
    CHECK(delayError(x, runEffect(e, x, 2), 2, latency) == 0.0);
}

TEST_CASE("Mastering EQ Widener and Loudness Maximizer", "[audio][dsp][premiere][amplitude]") {
    const EffectInstance eq = effectOf(kMastering, {{"peakEnable", ParamValue(1.0)},
                                                    {"peakGain", ParamValue(6.0)},
                                                    {"lowShelfEnable", ParamValue(1.0)},
                                                    {"lowShelfGain", ParamValue(6.0)},
                                                    {"highShelfEnable", ParamValue(1.0)},
                                                    {"highShelfGain", ParamValue(-6.0)}});
    const auto level = [&eq](double f) {
        return toneDb(runEffect(eq, sine(f, dbToAmp(-30.0), 1.0), 2), 2, f, kFrames1s / 2, kFrames1s) + 30.0;
    };
    CHECK(level(1000.0) == Approx(6.0).margin(0.1));
    CHECK(level(30.0) > 5.0);
    CHECK(level(16000.0) < -5.0);
    // Widener -100 %: mono.
    const std::vector<float> x = noise(0.5, 0.5);
    const std::vector<float> mono = runEffect(effectOf(kMastering, {{"widener", ParamValue(-100.0)}}), x, 2);
    double worst = 0.0;
    for (std::size_t i = 0; i < mono.size() / 2; ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(mono[2 * i] - mono[2 * i + 1])));
    }
    CHECK(worst < 1e-6);
    // Loudness Maximizer 100 %: +12 dB into the limiter, peaks at most -0.3 dBFS.
    const std::vector<float> quiet = noise(0.3, 1.0);
    const std::vector<float> loud = runEffect(effectOf(kMastering, {{"loudnessMaximizer", ParamValue(100.0)}}), quiet, 2);
    CHECK(peak(loud, 2) <= dbToAmp(-0.3) + 1e-6);
    CHECK(rms(loud, 2, kFrames1s / 2, kFrames1s) > 2.5 * rms(quiet, 2, kFrames1s / 2, kFrames1s));
}

TEST_CASE("Mastering Exciter and Reverb add what they say", "[audio][dsp][premiere][amplitude]") {
    // Tube exciter: the second harmonic of a 3 kHz tone.
    const std::vector<float> tone = sine(3000.0, dbToAmp(-12.0), 1.0);
    const std::vector<float> plain = runEffect(effectOf(kMastering), tone, 2);
    const std::vector<float> excited =
        runEffect(effectOf(kMastering, {{"exciterMode", ParamValue(2.0)}, {"exciterAmount", ParamValue(100.0)}}), tone, 2);
    CHECK(toneDb(plain, 2, 6000.0, kFrames1s / 2, kFrames1s) < -120.0);
    CHECK(toneDb(excited, 2, 6000.0, kFrames1s / 2, kFrames1s) > -40.0);
    // Reverb: a tail after the input stops (1 s of noise, then 1 s of silence), the dry level kept.
    std::vector<float> padded = noise(0.5, 1.0);
    padded.resize(padded.size() * 2, 0.0f);
    const std::vector<float> wet = runEffect(effectOf(kMastering, {{"reverbAmount", ParamValue(100.0)}}), padded, 2);
    const double input = rms(padded, 2, kFrames1s / 2, kFrames1s);
    CHECK(rms(wet, 2, kFrames1s + 4800, kFrames1s + 9600) > dbToAmp(-40.0) * input); // 100 .. 200 ms into the tail
    CHECK(rms(wet, 2, kFrames1s / 2, kFrames1s) > 0.95 * input);
    const std::vector<float> dry = runEffect(effectOf(kMastering), padded, 2);
    CHECK(peak(dry, 2, kFrames1s + 4800) == 0.0);
}

TEST_CASE("Guitar Suite distorts within bounds and Mix 0 is the dry signal", "[audio][dsp][premiere][amplitude]") {
    const std::vector<float> x = sine(220.0, 1.0, 1.0);
    const std::vector<float> y = runEffect(effectOf(kGuitarSuite), x, 2);
    const double fundamental = toneDb(y, 2, 220.0, kFrames1s / 2, kFrames1s);
    CHECK(toneDb(y, 2, 660.0, kFrames1s / 2, kFrames1s) - fundamental > -30.0);
    CHECK(peak(y, 2) < 4.0);
    CHECK(delayError(x, runEffect(effectOf(kGuitarSuite, {{"mix", ParamValue(0.0)}}), x, 2), 2, 0) == 0.0);
}

TEST_CASE("Guitar Suite filter shapes the tone", "[audio][dsp][premiere][amplitude]") {
    const auto level = [](double type, double f) {
        const EffectInstance e = effectOf(kGuitarSuite, {{"compressor", ParamValue(0.0)},
                                                         {"distortion", ParamValue(0.0)},
                                                         {"amplifier", ParamValue(0.0)},
                                                         {"filterType", ParamValue(type)},
                                                         {"filterFrequency", ParamValue(500.0)}});
        return toneDb(runEffect(e, sine(f, dbToAmp(-20.0), 1.0), 2), 2, f, kFrames1s / 2, kFrames1s) + 20.0;
    };
    CHECK(level(1.0, 5000.0) < -35.0); // lowpass
    CHECK(level(1.0, 100.0) == Approx(0.0).margin(0.5));
    CHECK(level(2.0, 50.0) < -35.0);   // highpass
    CHECK(level(3.0, 500.0) == Approx(0.0).margin(0.1)); // bandpass: 0 dB at its centre
    CHECK(level(0.0, 5000.0) == Approx(0.0).margin(0.01)); // off
}
