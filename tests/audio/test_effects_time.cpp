// p02 audio: Premiere Pro's Time family of audio effects, measured offline: Delay, Analog Delay and Multitap Delay (echo
// positions and feedback ratios, automation without zipper noise, a saturating loop past 100 % feedback),
// Chorus/Flanger, Flanger and Phaser (swept delay ranges, all-pass notches and their movement), DeReverb (a synthetic
// reverberant signal), Convolution Reverb (against a direct convolution; RT60 and Room Size), Studio Reverb (RT60 and
// the dry/wet levels), Surround Reverb (5.1 routing, decorrelated tails), DeNoise and Adaptive Noise Reduction (steady noise under a sine) and the Automatic Click Remover
// (clicks on a sine). Every effect runs through an EffectChain (runEffect), the path preview and export take.

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

std::size_t framesOf(const std::vector<float>& x, int channels) { return x.size() / static_cast<std::size_t>(channels); }

/// A unit impulse every `every` frames (mono).
std::vector<float> impulseTrain(std::size_t frames, std::size_t every) {
    std::vector<float> x(frames, 0.0f);
    for (std::size_t i = 0; i < frames; i += every) {
        x[i] = 1.0f;
    }
    return x;
}

/// Largest |a[i + shift] - b[i]| over the samples both have (same channel layout).
double maxDifference(const std::vector<float>& a, const std::vector<float>& b, std::size_t shift = 0) {
    double m = 0.0;
    for (std::size_t i = 0; i + shift < a.size() && i < b.size(); ++i) {
        m = std::max(m, static_cast<double>(std::fabs(a[i + shift] - b[i])));
    }
    return m;
}

/// Energy of a mono signal over [first, last).
double energy(const std::vector<float>& x, std::size_t first, std::size_t last) {
    double e = 0.0;
    for (std::size_t i = first; i < std::min(last, x.size()); ++i) {
        e += static_cast<double>(x[i]) * x[i];
    }
    return e;
}

/// RT60 of channel 0 from frame `from`: twice the time the Schroeder energy decay curve takes from -5 to -35 dB.
double rt60(const std::vector<float>& x, int channels, std::size_t from) {
    const std::size_t frames = framesOf(x, channels);
    std::vector<double> edc(frames, 0.0);
    double acc = 0.0;
    for (std::size_t i = frames; i-- > from;) {
        const double v = x[i * static_cast<std::size_t>(channels)];
        acc += v * v;
        edc[i] = acc;
    }
    std::size_t t5 = 0;
    std::size_t t35 = 0;
    for (std::size_t i = from; i < frames && edc[from] > 0.0; ++i) {
        const double level = 10.0 * std::log10(std::max(edc[i], 1e-300) / edc[from]);
        if (t5 == 0 && level < -5.0) {
            t5 = i;
        }
        if (level < -35.0) {
            t35 = i;
            break;
        }
    }
    return t35 > t5 ? 2.0 * static_cast<double>(t35 - t5) / kRate : 0.0;
}

/// A reverberant mono test signal: three 40 ms noise bursts a second apart, convolved with a direct path plus an
/// exponentially decaying noise tail of `rt60Seconds` (starting 2 ms later, about as much energy as the direct path).
std::vector<float> reverberantBursts(double rt60Seconds, std::vector<std::size_t>& burstStarts) {
    const std::size_t frames = static_cast<std::size_t>(4 * kRate);
    std::vector<double> dry(frames, 0.0);
    std::uint32_t s = 999;
    const auto next = [&s] {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<double>(s) / 4294967295.0 * 2.0 - 1.0;
    };
    const std::size_t burst = static_cast<std::size_t>(0.04 * kRate);
    for (int b = 0; b < 3; ++b) {
        const std::size_t start = static_cast<std::size_t>(0.2 * kRate) + static_cast<std::size_t>(b) * kRate;
        burstStarts.push_back(start);
        for (std::size_t i = 0; i < burst; ++i) {
            dry[start + i] = 0.3 * next();
        }
    }
    const auto tail = static_cast<std::size_t>(rt60Seconds * kRate);
    std::vector<double> h(tail, 0.0);
    h[0] = 1.0;
    const double g = std::sqrt(3.0 * 13.8 / (rt60Seconds * kRate));
    for (std::size_t i = 96; i < tail; ++i) {
        h[i] = g * next() * std::exp(-6.9078 * static_cast<double>(i) / (rt60Seconds * kRate));
    }
    std::vector<float> out(frames, 0.0f);
    for (std::size_t n = 0; n < frames; ++n) {
        if (dry[n] == 0.0) {
            continue;
        }
        for (std::size_t k = 0; k < tail && n + k < frames; ++k) {
            out[n + k] += static_cast<float>(dry[n] * h[k]);
        }
    }
    return out;
}

struct NoiseResult {
    double sineDb = 0.0;  ///< level of the 1 kHz sine, output over input
    double noiseDb = 0.0; ///< level of what is left after removing the sine, over the input noise
};

/// 4 s of a 1 kHz sine at -12 dBFS under uniform white noise at about -31 dBFS (mono), measured over the last 2 s.
NoiseResult noiseUnderSine(const EffectInstance& e) {
    std::vector<float> x = sine(1000.0, 0.25, 4.0, 1, kRate);
    const std::vector<float> n = noise(0.05, 4.0, 1, kRate, 4242);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] += n[i];
    }
    const auto latency = static_cast<std::size_t>(effectLatency(e, 1));
    const std::vector<float> y = runEffect(e, x, 1);
    const std::size_t first = 2 * kRate;
    const std::size_t last = x.size() - latency;
    // Fit the sine (its exact frequency) and take it out.
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        const double phase = 2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate;
        re += y[i + latency] * std::cos(phase);
        im += y[i + latency] * std::sin(phase);
    }
    re *= 2.0 / static_cast<double>(last - first);
    im *= 2.0 / static_cast<double>(last - first);
    double residual = 0.0;
    double input = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        const double phase = 2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate;
        const double v = y[i + latency] - (re * std::cos(phase) + im * std::sin(phase));
        residual += v * v;
        input += static_cast<double>(n[i]) * n[i];
    }
    return {ampToDb(std::hypot(re, im) / 0.25), 10.0 * std::log10(residual / input)};
}

} // namespace

// ---- Delay and Echo -----------------------------------------------------------------------------------------------

TEST_CASE("Delay puts the echoes of an impulse at the delay time with the feedback ratio", "[audio][dsp][premiere][time]") {
    const EffectInstance e = effectOf("wabcut.audio.delay", {{"delay", 100.0}, {"feedback", 50.0}, {"mix", 100.0}});
    CHECK(effectLatency(e, 1) == 0);
    const std::vector<float> y = runEffect(e, impulse(kRate, 1), 1);
    CHECK(y[0] == 0.0f); // Mix 100 %: only the echoes
    CHECK(y[4800] == Approx(1.0).margin(1e-6));
    CHECK(y[9600] == Approx(0.5).margin(1e-6));
    CHECK(y[14400] == Approx(0.25).margin(1e-6));
    CHECK(y[19200] == Approx(0.125).margin(1e-6));
    double elsewhere = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        if (i % 4800 != 0) {
            elsewhere = std::max(elsewhere, static_cast<double>(std::fabs(y[i])));
        }
    }
    CHECK(elsewhere < 1e-6);

    // Premiere's defaults: one echo after 1 s, half dry and half echo.
    const std::vector<float> d = runEffect(effectOf("wabcut.audio.delay"), impulse(2 * kRate, 2), 2);
    CHECK(d[0] == Approx(0.5));
    CHECK(d[1] == Approx(0.5));
    CHECK(d[2 * kRate] == Approx(0.5).margin(1e-6));
    CHECK(peak(d, 2, 1, kRate) < 1e-6);
    CHECK(peak(d, 2, kRate + 1, 2 * kRate) < 1e-6);
}

TEST_CASE("Delay at Mix 0 is the dry signal and its automation glides without zipper noise", "[audio][dsp][premiere][time]") {
    const std::vector<float> x = noise(0.5, 0.5, 2, kRate);
    const std::vector<float> dry = runEffect(effectOf("wabcut.audio.delay", {{"mix", 0.0}, {"feedback", 90.0}}), x, 2);
    CHECK(maxDifference(dry, x) == 0.0);

    // The delay time ramps from 100 to 300 ms: the echo of a 300 Hz sine glides in pitch, it never jumps (a stepped
    // delay would jump by up to 0.4 x 32 frames at every parameter cell).
    Curve ramp;
    ramp.keys.push_back(Keyframe{Ticks{kTicksPerSecond / 10}, 100.0});
    ramp.keys.push_back(Keyframe{Ticks{kTicksPerSecond * 6 / 10}, 300.0});
    const EffectInstance e = effectOf("wabcut.audio.delay", {{"delay", ParamValue(ramp)}, {"feedback", 0.0}, {"mix", 100.0}});
    const std::vector<float> y = runEffect(e, sine(300.0, 0.5, 1.0, 1, kRate), 1);
    double step = 0.0;
    for (std::size_t i = 1; i < y.size(); ++i) {
        step = std::max(step, static_cast<double>(std::fabs(y[i] - y[i - 1])));
    }
    const double sineStep = 0.5 * 2.0 * std::numbers::pi * 300.0 / kRate; // the input's own largest step
    CHECK(step <= 1.05 * sineStep);
    CHECK(peak(y, 1, kRate * 8 / 10, kRate) == Approx(0.5).margin(0.01)); // after the glide: the echo is whole
}

TEST_CASE("Multitap Delay places each tap at its delay and level and feeds each back on itself", "[audio][dsp][premiere][time]") {
    const EffectInstance e = effectOf("wabcut.audio.multitapDelay",
                                      {{"delay1", 100.0}, {"delay2", 200.0}, {"delay3", 300.0}, {"delay4", 400.0},
                                       {"level1", 0.0}, {"level2", -6.0}, {"level3", -12.0}, {"level4", -18.0},
                                       {"feedback1", 0.0}, {"mix", 100.0}});
    const std::vector<float> y = runEffect(e, impulse(kRate, 1), 1);
    CHECK(y[0] == 0.0f);
    CHECK(y[4800] == Approx(1.0).margin(1e-6));
    CHECK(y[9600] == Approx(dbToAmp(-6.0)).margin(1e-6));
    CHECK(y[14400] == Approx(dbToAmp(-12.0)).margin(1e-6));
    CHECK(y[19200] == Approx(dbToAmp(-18.0)).margin(1e-6));
    CHECK(peak(y, 1, 19201, kRate) < 1e-6);

    // Tap 1 at 50 % feedback repeats every 100 ms; the other taps do not.
    const EffectInstance fed = effectOf("wabcut.audio.multitapDelay",
                                        {{"delay1", 100.0}, {"delay2", 1500.0}, {"delay3", 1500.0}, {"delay4", 1500.0},
                                         {"feedback1", 50.0}, {"level2", -96.0}, {"level3", -96.0}, {"level4", -96.0},
                                         {"mix", 100.0}});
    const std::vector<float> r = runEffect(fed, impulse(kRate, 1), 1);
    CHECK(r[4800] == Approx(1.0).margin(1e-6));
    CHECK(r[9600] == Approx(0.5).margin(1e-6));
    CHECK(r[14400] == Approx(0.25).margin(1e-6));

    // Mix 50 % (the default) halves the dry signal.
    const std::vector<float> d = runEffect(effectOf("wabcut.audio.multitapDelay"), impulse(1000, 1), 1);
    CHECK(d[0] == Approx(0.5));
}

TEST_CASE("Analog Delay repeats at the delay with the feedback ratio in every mode and its loop saturates", "[audio][dsp][premiere][time]") {
    // A 20 ms 200 Hz burst (inside every mode's loop band); Dry 0, Wet 100, Delay 100 ms, Feedback 50 %.
    std::vector<float> burst(kRate, 0.0f);
    for (std::size_t i = 0; i < 960; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / 960.0);
        burst[i] = static_cast<float>(0.25 * w * std::sin(2.0 * std::numbers::pi * 200.0 * static_cast<double>(i) / kRate));
    }
    const double input = rms(burst, 1, 0, 1400);
    for (const double mode : {0.0, 1.0, 2.0}) {
        INFO("mode " << mode);
        const EffectInstance e = effectOf("wabcut.audio.analogDelay", {{"mode", mode}, {"dryOutput", 0.0}, {"wetOutput", 100.0},
                                                                       {"delay", 100.0}, {"feedback", 50.0}});
        const std::vector<float> y = runEffect(e, burst, 1);
        CHECK(peak(y, 1, 0, 4800) == 0.0); // no dry, nothing before the delay
        const double first = rms(y, 1, 4800, 6200);
        const double second = rms(y, 1, 9600, 11000);
        const double third = rms(y, 1, 14400, 15800);
        CHECK(first == Approx(input).epsilon(0.01));
        CHECK(second / first == Approx(0.5).epsilon(0.05));
        CHECK(third / second == Approx(0.5).epsilon(0.05));
    }

    // Dry 100 %, Wet 0 %: the original.
    const std::vector<float> x = noise(0.5, 0.5, 2, kRate);
    CHECK(maxDifference(runEffect(effectOf("wabcut.audio.analogDelay", {{"wetOutput", 0.0}}), x, 2), x) == 0.0);

    // Feedback 200 %: the repeats grow until the loop's saturation holds them; the output stays bounded.
    std::vector<float> kick = noise(0.5, 0.05, 2, kRate);
    kick.resize(static_cast<std::size_t>(3 * kRate) * 2, 0.0f);
    const std::vector<float> loud = runEffect(effectOf("wabcut.audio.analogDelay", {{"feedback", 200.0}, {"delay", 50.0}}), kick, 2);
    bool finite = true;
    for (const float v : loud) {
        finite = finite && std::isfinite(v);
    }
    CHECK(finite);
    CHECK(peak(loud, 2) < 3.0);
    CHECK(peak(loud, 2, 2 * kRate, 3 * kRate) > 0.1); // still repeating: the hardware's runaway, held
}

// ---- Modulation -------------------------------------------------------------------------------------------------

TEST_CASE("Flanger sweeps its delay between the initial and final delay times", "[audio][dsp][premiere][time]") {
    // Mix 100 % and no feedback: the output is the input at the swept delay. Impulses every 10 ms probe it.
    const EffectInstance e = effectOf("wabcut.audio.flanger", {{"mix", 100.0}, {"feedback", 0.0}, {"initialDelay", 1.0},
                                                               {"finalDelay", 5.0}, {"modulationRate", 1.0}});
    const std::vector<float> y = runEffect(e, impulseTrain(2 * kRate, 480), 1);
    int lo = 1 << 30;
    int hi = 0;
    for (std::size_t t = 0; t + 480 <= y.size(); t += 480) {
        int best = 0;
        double value = 0.0;
        for (int k = 0; k < 400; ++k) {
            const double v = std::fabs(y[t + static_cast<std::size_t>(k)]);
            if (v > value) {
                value = v;
                best = k;
            }
        }
        lo = std::min(lo, best);
        hi = std::max(hi, best);
    }
    CHECK(lo >= 47);
    CHECK(lo <= 49); // 1 ms
    CHECK(hi >= 239);
    CHECK(hi <= 241); // 5 ms

    // Mix 0 % is the original.
    const std::vector<float> x = noise(0.5, 0.5, 2, kRate);
    CHECK(maxDifference(runEffect(effectOf("wabcut.audio.flanger", {{"mix", 0.0}}), x, 2), x) == 0.0);
}

TEST_CASE("Chorus/Flanger keeps its voices inside the swept delay range and is dry at Intensity 0", "[audio][dsp][premiere][time]") {
    // Chorus at Width 50 %: voices between 12 and 18 ms (576 .. 864 frames), sweeping that whole range.
    const std::vector<float> train = impulseTrain(2 * kRate, 2400);
    const std::vector<float> y = runEffect(effectOf("wabcut.audio.chorusFlanger", {{"intensity", 100.0}, {"width", 50.0}, {"speed", 1.0}}), train, 1);
    int lo = 1 << 30;
    int hi = 0;
    for (std::size_t t = 0; t + 2400 <= y.size(); t += 2400) {
        CHECK(y[t] == Approx(0.5)); // Intensity 100 %: half the original
        for (int k = 1; k < 2000; ++k) {
            if (std::fabs(y[t + static_cast<std::size_t>(k)]) > 0.02) {
                lo = std::min(lo, k);
                hi = std::max(hi, k);
            }
        }
    }
    CHECK(lo >= 574);
    CHECK(lo <= 586);
    CHECK(hi >= 854);
    CHECK(hi <= 866);

    // Flanger mode at Width 50 %: one voice between 0.3 and 3.3 ms (14 .. 158 frames).
    const std::vector<float> f = runEffect(effectOf("wabcut.audio.chorusFlanger", {{"mode", 1.0}, {"intensity", 100.0}, {"width", 50.0}, {"speed", 1.0}}), train, 1);
    lo = 1 << 30;
    hi = 0;
    for (std::size_t t = 0; t + 2400 <= f.size(); t += 2400) {
        int best = 0;
        double value = 0.0;
        for (int k = 2; k < 400; ++k) {
            const double v = std::fabs(f[t + static_cast<std::size_t>(k)]);
            if (v > value) {
                value = v;
                best = k;
            }
        }
        lo = std::min(lo, best);
        hi = std::max(hi, best);
    }
    CHECK(lo >= 13);
    CHECK(lo <= 16);
    CHECK(hi >= 156);
    CHECK(hi <= 159);

    // Intensity 0 %: the original, whatever the other settings.
    const std::vector<float> x = noise(0.5, 0.5, 2, kRate);
    CHECK(maxDifference(runEffect(effectOf("wabcut.audio.chorusFlanger", {{"intensity", 0.0}, {"transience", 100.0}}), x, 2), x) == 0.0);
}

TEST_CASE("Phaser notches sit where the all-pass phase reaches 180 degrees and move with Upper Frequency", "[audio][dsp][premiere][time]") {
    // Depth 0: four all-pass sections fixed at Upper Frequency fc, mixed half and half with the original. Each section
    // shifts phase by 2 atan(tan(pi f / fs) / tan(pi fc / fs)); the notches are where the four make 180 and 540 degrees.
    const auto notches = [](double fc) {
        const double t = std::tan(std::numbers::pi * fc / kRate);
        return std::pair{kRate / std::numbers::pi * std::atan(t * std::tan(std::numbers::pi / 8.0)),
                         kRate / std::numbers::pi * std::atan(t * std::tan(3.0 * std::numbers::pi / 8.0))};
    };
    const auto gainDb = [](double upper, double frequency) {
        const EffectInstance e = effectOf("wabcut.audio.phaser", {{"depth", 0.0}, {"upperFrequency", upper}, {"stages", 4.0},
                                                                  {"intensity", 100.0}, {"mix", 50.0}, {"feedback", 0.0}});
        const std::vector<float> y = runEffect(e, sine(frequency, 0.5, 0.5, 1, kRate), 1);
        return ampToDb(toneAmplitude(y, 1, frequency, 12000, 24000) / 0.5);
    };
    const auto [low1000, high1000] = notches(1000.0);
    const auto [low2000, high2000] = notches(2000.0);
    CHECK(gainDb(1000.0, low1000) < -40.0);
    CHECK(gainDb(1000.0, high1000) < -40.0);
    CHECK(gainDb(1000.0, 1000.0) == Approx(0.0).margin(0.1)); // 360 degrees: in phase
    CHECK(gainDb(2000.0, low2000) < -40.0);                     // the notch moved up an octave
    CHECK(gainDb(2000.0, low1000) > -6.0);                      // and left the old place
    CHECK(gainDb(2000.0, high2000) < -40.0);

    // Depth 100 % at 1 Hz sweeps the notches across a 1 kHz tone: its level swings by more than 20 dB.
    const EffectInstance swept = effectOf("wabcut.audio.phaser", {{"depth", 100.0}, {"upperFrequency", 4000.0}, {"modulationRate", 1.0}});
    const std::vector<float> y = runEffect(swept, sine(1000.0, 0.5, 2.0, 1, kRate), 1);
    double lo = 1e9;
    double hi = -1e9;
    for (std::size_t w = 4800; w + 480 <= y.size(); w += 480) {
        const double level = ampToDb(rms(y, 1, w, w + 480));
        lo = std::min(lo, level);
        hi = std::max(hi, level);
    }
    CHECK(hi - lo > 20.0);
    CHECK(hi < -6.0 + 0.5); // never above the input (0.5 peak = -9 dB RMS)

    // Mix 0 % is the original.
    const std::vector<float> x = noise(0.5, 0.5, 2, kRate);
    CHECK(maxDifference(runEffect(effectOf("wabcut.audio.phaser", {{"mix", 0.0}}), x, 2), x) == 0.0);
}

// ---- Reverb -----------------------------------------------------------------------------------------------------

TEST_CASE("Convolution Reverb equals a direct convolution with its own impulse response", "[audio][dsp][premiere][time]") {
    // Mono, Mix 100 %: the output is the input convolved with the impulse (Small Room), one partition late.
    const EffectInstance e = effectOf("wabcut.audio.convolutionReverb", {{"impulse", 0.0}, {"mix", 100.0}});
    const auto latency = static_cast<std::size_t>(effectLatency(e, 1));
    CHECK(latency == 512);
    const std::vector<float> response = runEffect(e, impulse(kRate, 1), 1);
    std::vector<double> h(response.begin() + static_cast<std::ptrdiff_t>(latency), response.end());
    while (!h.empty() && h.back() == 0.0) {
        h.pop_back();
    }
    REQUIRE(h.size() > static_cast<std::size_t>(0.3 * kRate)); // a small room's tail
    double e2 = 0.0;
    for (const double v : h) {
        e2 += v * v;
    }
    CHECK(e2 == Approx(1.0).epsilon(0.05)); // unit energy: white noise comes out as loud as it went in

    // 100 ms of noise, rendered in odd-sized blocks, against the direct convolution with that impulse response.
    std::vector<float> x = noise(0.5, 0.1, 1, kRate, 31);
    x.resize(kRate, 0.0f);
    const std::vector<float> y = runEffect(e, x, 1, kRate, 333);
    double worst = 0.0;
    double loudest = 0.0;
    for (std::size_t n = 0; n + latency < x.size(); ++n) {
        double acc = 0.0;
        for (std::size_t k = 0; k < h.size() && k <= n; ++k) {
            acc += h[k] * x[n - k];
        }
        worst = std::max(worst, std::fabs(acc - y[n + latency]));
        loudest = std::max(loudest, std::fabs(acc));
    }
    CHECK(loudest > 0.3);
    CHECK(worst < 1e-5);
}

TEST_CASE("Convolution Reverb impulses decay at their room's RT60 and Room Size and Damping shape them", "[audio][dsp][premiere][time]") {
    const auto responseOf = [](std::vector<EffectParam> params, double seconds) {
        params.push_back({"mix", 100.0});
        const EffectInstance e = effectOf("wabcut.audio.convolutionReverb", std::move(params));
        const std::vector<float> y = runEffect(e, impulse(static_cast<std::size_t>(seconds * kRate), 1), 1);
        return std::vector<float>(y.begin() + effectLatency(e, 1), y.end());
    };
    // Large Hall: a 2 s mid-band RT60 at full size, half at Room Size 50 %.
    CHECK(rt60(responseOf({{"impulse", 2.0}}, 3.0), 1, 0) == Approx(2.0).epsilon(0.15));
    CHECK(rt60(responseOf({{"impulse", 2.0}, {"roomSize", 50.0}}, 3.0), 1, 0) == Approx(1.0).epsilon(0.15));
    // Damping HF takes highs out of the tail: less energy in its first difference relative to the whole.
    const auto brightness = [](const std::vector<float>& h) {
        double all = 0.0;
        double diff = 0.0;
        for (std::size_t i = 1; i < h.size(); ++i) {
            all += static_cast<double>(h[i]) * h[i];
            const double d = static_cast<double>(h[i]) - h[i - 1];
            diff += d * d;
        }
        return diff / all;
    };
    const double open = brightness(responseOf({{"impulse", 1.0}}, 2.0));
    const double damped = brightness(responseOf({{"impulse", 1.0}, {"dampingHF", 100.0}}, 2.0));
    CHECK(damped < 0.7 * open);

    // Width 0 %: a mono reverb.
    const std::vector<float> x = noise(0.5, 0.5, 2, kRate);
    const std::vector<float> mono = runEffect(effectOf("wabcut.audio.convolutionReverb", {{"width", 0.0}, {"mix", 100.0}}), x, 2);
    double side = 0.0;
    for (std::size_t i = 0; i + 1 < mono.size(); i += 2) {
        side = std::max(side, static_cast<double>(std::fabs(mono[i] - mono[i + 1])));
    }
    CHECK(side < 1e-6);
    CHECK(peak(mono, 2) > 0.1);
}

TEST_CASE("Studio Reverb decays at the Decay time and follows its Dry and Wet levels", "[audio][dsp][premiere][time]") {
    for (const double decay : {1000.0, 2500.0}) {
        INFO("decay " << decay);
        const EffectInstance e = effectOf("wabcut.audio.studioReverb",
                                          {{"decay", decay}, {"dry", 0.0}, {"wet", 100.0}, {"earlyReflections", 0.0},
                                           {"damping", 0.0}, {"lowFrequencyCut", 20.0}, {"highFrequencyCut", 20000.0}});
        const std::vector<float> y = runEffect(e, impulse(static_cast<std::size_t>(6 * kRate), 2), 2);
        CHECK(y[0] == 0.0f); // Dry 0 %: no direct sound
        CHECK(rt60(y, 2, 0) == Approx(decay * 1e-3).epsilon(0.12));
    }
    // Damping shortens the tail (its highs die away faster).
    const auto decayWith = [](double damping) {
        const EffectInstance e = effectOf("wabcut.audio.studioReverb", {{"decay", 2500.0}, {"dry", 0.0}, {"earlyReflections", 0.0},
                                                                        {"damping", damping}, {"highFrequencyCut", 20000.0}});
        return rt60(runEffect(e, impulse(static_cast<std::size_t>(6 * kRate), 2), 2), 2, 0);
    };
    CHECK(decayWith(100.0) < 0.9 * decayWith(0.0));

    const std::vector<float> x = noise(0.5, 1.0, 2, kRate);
    // Wet 0 %: the dry signal at its level.
    const std::vector<float> dryOnly = runEffect(effectOf("wabcut.audio.studioReverb", {{"wet", 0.0}, {"dry", 70.0}}), x, 2);
    double worst = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(dryOnly[i]) - 0.7 * x[i]));
    }
    CHECK(worst < 1e-6);
    // Wet 100 % against 50 %: 6 dB.
    const std::vector<float> full = runEffect(effectOf("wabcut.audio.studioReverb", {{"wet", 100.0}, {"dry", 0.0}}), x, 2);
    const std::vector<float> half = runEffect(effectOf("wabcut.audio.studioReverb", {{"wet", 50.0}, {"dry", 0.0}}), x, 2);
    CHECK(ampToDb(rms(full, 2, 0, kRate) / rms(half, 2, 0, kRate)) == Approx(6.02).margin(0.05));
}

TEST_CASE("Surround Reverb gives 5.1 decorrelated front and surround tails and keeps the LFE dry", "[audio][dsp][premiere][time]") {
    const auto correlation = [](const std::vector<float>& x, std::size_t a, std::size_t b) {
        double ab = 0.0;
        double aa = 0.0;
        double bb = 0.0;
        for (std::size_t i = 2400; i < 2 * static_cast<std::size_t>(kRate); ++i) {
            const double u = x[i * 6 + a];
            const double v = x[i * 6 + b];
            ab += u * v;
            aa += u * u;
            bb += v * v;
        }
        return ab / std::sqrt(aa * bb);
    };
    // An impulse on the left channel of a 5.1 buffer (L R C LFE Ls Rs), wet only.
    std::vector<float> x(static_cast<std::size_t>(3 * kRate) * 6, 0.0f);
    x[0] = 1.0f;
    const std::vector<float> y = runEffect(effectOf("wabcut.audio.surroundReverb", {{"dry", 0.0}, {"wet", 100.0}}), x, 6);
    for (const int c : {0, 1, 2, 4, 5}) {
        INFO("channel " << c);
        CHECK(rmsOf(y, 6, c, 0, 3 * kRate) > 1e-5);
    }
    CHECK(rmsOf(y, 6, 3, 0, 3 * kRate) == 0.0); // no tail in the LFE
    CHECK(std::fabs(correlation(y, 0, 4)) < 0.2); // front left against surround left
    CHECK(std::fabs(correlation(y, 1, 5)) < 0.2);
    CHECK(std::fabs(correlation(y, 0, 1)) < 0.2);

    // LFE Input 0 % and Center Input 0 %: those channels pass dry and feed nothing.
    for (const std::size_t channel : {std::size_t{2}, std::size_t{3}}) {
        INFO("impulse on channel " << channel);
        std::vector<float> one(static_cast<std::size_t>(kRate) * 6, 0.0f);
        one[channel] = 1.0f;
        const std::vector<float> r = runEffect(effectOf("wabcut.audio.surroundReverb", {{"centerInput", 0.0}, {"lfeInput", 0.0}}), one, 6);
        CHECK(r[channel] == 1.0f);
        double others = 0.0;
        for (std::size_t i = 0; i < r.size(); ++i) {
            if (i != channel) {
                others = std::max(others, static_cast<double>(std::fabs(r[i])));
            }
        }
        CHECK(others == 0.0);
    }

    // On stereo it is Studio Reverb.
    const std::vector<float> stereo = noise(0.5, 0.5, 2, kRate);
    CHECK(maxDifference(runEffect(effectOf("wabcut.audio.surroundReverb"), stereo, 2),
                        runEffect(effectOf("wabcut.audio.studioReverb"), stereo, 2)) == 0.0);
}

// ---- Noise Reduction/Restoration ----------------------------------------------------------------------------------

TEST_CASE("DeReverb pulls a synthetic reverb tail down and leaves it alone at Amount 0", "[audio][dsp][premiere][time]") {
    std::vector<std::size_t> starts;
    const std::vector<float> x = reverberantBursts(0.7, starts);
    const auto measure = [&](double amount) {
        const EffectInstance e = effectOf("wabcut.audio.deReverb", {{"amount", amount}});
        const auto latency = static_cast<std::size_t>(effectLatency(e, 1));
        const std::vector<float> y = runEffect(e, x, 1);
        double tailIn = 0.0;
        double tailOut = 0.0;
        double burstIn = 0.0;
        double burstOut = 0.0;
        for (const std::size_t s : starts) {
            const std::size_t end = s + static_cast<std::size_t>(0.04 * kRate);
            // The tail from 100 to 500 ms after the burst; the burst itself.
            tailIn += energy(x, end + 4800, end + 24000);
            burstIn += energy(x, s, end);
            for (std::size_t i = end + 4800; i < end + 24000; ++i) {
                tailOut += static_cast<double>(y[i + latency]) * y[i + latency];
            }
            for (std::size_t i = s; i < end; ++i) {
                burstOut += static_cast<double>(y[i + latency]) * y[i + latency];
            }
        }
        return std::pair{10.0 * std::log10(tailOut / tailIn), 10.0 * std::log10(burstOut / burstIn)};
    };
    CHECK(effectLatency(effectOf("wabcut.audio.deReverb"), 1) == 2048);
    const auto [tail0, burst0] = measure(0.0);
    CHECK(std::fabs(tail0) < 0.1);
    CHECK(std::fabs(burst0) < 0.1);
    const auto [tailDefault, burstDefault] = measure(40.0);
    CHECK(tailDefault < -3.0);
    const auto [tail100, burst100] = measure(100.0);
    CHECK(tail100 < -8.0);
    CHECK(burst100 > -1.0); // the direct sound stays
    (void)burstDefault;
}

TEST_CASE("DeNoise reduces steady noise under a sine and keeps the sine", "[audio][dsp][premiere][time]") {
    CHECK(effectLatency(effectOf("wabcut.audio.spectralDenoise"), 1) == 1024);
    const NoiseResult standard = noiseUnderSine(effectOf("wabcut.audio.spectralDenoise"));
    CHECK(standard.noiseDb < -6.0);
    CHECK(std::fabs(standard.sineDb) < 1.0);
    const NoiseResult strong = noiseUnderSine(effectOf("wabcut.audio.spectralDenoise", {{"amount", 100.0}}));
    CHECK(strong.noiseDb < standard.noiseDb - 3.0);
    CHECK(std::fabs(strong.sineDb) < 1.0);
    const NoiseResult off = noiseUnderSine(effectOf("wabcut.audio.spectralDenoise", {{"amount", 0.0}}));
    CHECK(std::fabs(off.noiseDb) < 0.1);
    CHECK(std::fabs(off.sineDb) < 0.1);

    // Output Noise Only: what is removed - the noise, not the sine.
    std::vector<float> x = sine(1000.0, 0.25, 3.0, 1, kRate);
    const std::vector<float> n = noise(0.05, 3.0, 1, kRate, 4242);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] += n[i];
    }
    const std::vector<float> removed = runEffect(effectOf("wabcut.audio.spectralDenoise", {{"outputNoiseOnly", 1.0}}), x, 1);
    CHECK(ampToDb(toneAmplitude(removed, 1, 1000.0, 2 * kRate, 3 * kRate) / 0.25) < -30.0);
    CHECK(rms(removed, 1, 2 * kRate, 3 * kRate) > 0.5 * rms(n, 1, 2 * kRate, 3 * kRate));
}

TEST_CASE("Adaptive Noise Reduction reduces steady noise under a sine and keeps the sine", "[audio][dsp][premiere][time]") {
    CHECK(effectLatency(effectOf("wabcut.audio.adaptiveNoiseReduction"), 1) == 512);
    const NoiseResult standard = noiseUnderSine(effectOf("wabcut.audio.adaptiveNoiseReduction"));
    CHECK(standard.noiseDb < -6.0);
    CHECK(std::fabs(standard.sineDb) < 1.0);
    const NoiseResult deeper = noiseUnderSine(effectOf("wabcut.audio.adaptiveNoiseReduction", {{"reduceNoiseBy", 30.0}}));
    CHECK(deeper.noiseDb < standard.noiseDb - 2.0);
    CHECK(std::fabs(deeper.sineDb) < 1.0);
    // FFT Size sets the analysis (and the latency).
    const EffectInstance fine = effectOf("wabcut.audio.adaptiveNoiseReduction", {{"fftSize", 2048.0}});
    CHECK(effectLatency(fine, 1) == 2048);
    const NoiseResult fineResult = noiseUnderSine(fine);
    CHECK(fineResult.noiseDb < -6.0);
    CHECK(std::fabs(fineResult.sineDb) < 1.0);
}

TEST_CASE("Automatic Click Remover repairs clicks on a sine and leaves a clean sine untouched", "[audio][dsp][premiere][time]") {
    // 1.5 s of a 1 kHz sine (10 ms fade-in) with a two-sample click (+0.4, -0.25) every ~100 ms.
    const std::size_t frames = static_cast<std::size_t>(1.5 * kRate);
    std::vector<float> clean(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double fade = std::min(1.0, static_cast<double>(i) / 480.0);
        clean[i] = static_cast<float>(0.5 * fade * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate));
    }
    std::vector<float> clicked = clean;
    std::uint32_t s = 77;
    for (std::size_t c = 4000; c + 2000 < frames; c += 4800) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const std::size_t at = c + s % 1000;
        clicked[at] += 0.4f;
        clicked[at + 1] -= 0.25f;
    }
    const EffectInstance e = effectOf("wabcut.audio.automaticClickRemover");
    const auto latency = static_cast<std::size_t>(effectLatency(e, 1));
    CHECK(latency > 0);
    CHECK(latency < static_cast<std::size_t>(kRate / 20)); // under 50 ms of lookahead
    CHECK(maxDifference(clicked, clean) == Approx(0.4).margin(1e-6));
    CHECK(maxDifference(runEffect(e, clicked, 1), clean, latency) < 0.01);
    // A clean sine, and a sine starting abruptly from silence, pass unchanged (delayed by the lookahead).
    CHECK(maxDifference(runEffect(e, clean, 1), clean, latency) == 0.0);
    const std::vector<float> abrupt = sine(100.0, 0.5, 1.0, 1, kRate);
    CHECK(maxDifference(runEffect(e, abrupt, 1), abrupt, latency) == 0.0);
    // The least sensitive Threshold still finds these clicks; the most sensitive leaves the clean sine alone.
    CHECK(maxDifference(runEffect(effectOf("wabcut.audio.automaticClickRemover", {{"threshold", 100.0}}), clicked, 1), clean, latency) < 0.01);
    CHECK(maxDifference(runEffect(effectOf("wabcut.audio.automaticClickRemover", {{"threshold", 1.0}}), clean, 1), clean, latency) == 0.0);
}

// ---- All of them ----------------------------------------------------------------------------------------------------

TEST_CASE("Time effects render the same samples whatever the block size", "[audio][dsp][premiere][time]") {
    const std::vector<float> x = noise(0.5, 0.5, 2, kRate, 777);
    for (const char* id : {"wabcut.audio.delay", "wabcut.audio.analogDelay", "wabcut.audio.multitapDelay",
                           "wabcut.audio.chorusFlanger", "wabcut.audio.flanger", "wabcut.audio.phaser",
                           "wabcut.audio.convolutionReverb", "wabcut.audio.studioReverb", "wabcut.audio.surroundReverb",
                           "wabcut.audio.deReverb",
                           "wabcut.audio.spectralDenoise", "wabcut.audio.adaptiveNoiseReduction",
                           "wabcut.audio.automaticClickRemover"}) {
        INFO(id);
        const EffectInstance e = effectOf(id);
        CHECK(maxDifference(runEffect(e, x, 2, kRate, 512), runEffect(e, x, 2, kRate, 37)) == 0.0);
    }
}
