// Reduce Noise (RNNoise): noise is removed, a voice-like signal survives, the latency is what the effect reports.

#include "DspTestUtil.h"

#include <wabcut/audio/dsp/Denoiser.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using namespace wabcut::audio;
using namespace wabcut::audio::test;

namespace {

/// A voiced, speech-like signal: a glottal pulse train gliding around 140 Hz (so it never repeats exactly) shaped by
/// two formants, gated into 300 ms syllables.
std::vector<float> voice(double seconds, int channels = 1) {
    const auto frames = static_cast<std::size_t>(seconds * 48000.0);
    std::vector<float> out(frames * static_cast<std::size_t>(channels));
    double f1 = 0.0;
    double f2 = 0.0;
    double v1 = 0.0;
    double v2 = 0.0;
    double glottis = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        glottis += (140.0 + 40.0 * std::sin(2.0 * std::numbers::pi * 0.7 * t)) / 48000.0;
        const double pulse = glottis >= 1.0 ? 1.0 : 0.0;
        glottis -= std::floor(glottis);
        // Two resonators (700 Hz and 1200 Hz) driven by the pulses.
        const double r = 0.995;
        const double c1 = 2.0 * r * std::cos(2.0 * std::numbers::pi * 700.0 / 48000.0);
        const double c2 = 2.0 * r * std::cos(2.0 * std::numbers::pi * 1200.0 / 48000.0);
        const double y1 = pulse + c1 * f1 - r * r * v1;
        v1 = f1;
        f1 = y1;
        const double y2 = pulse + c2 * f2 - r * r * v2;
        v2 = f2;
        f2 = y2;
        const double syllable = std::fmod(t, 0.45) < 0.3 ? std::sin(std::numbers::pi * std::fmod(t, 0.45) / 0.3) : 0.0;
        const auto v = static_cast<float>(0.02 * (y1 + 0.6 * y2) * syllable);
        for (int c = 0; c < channels; ++c) {
            out[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = v;
        }
    }
    return out;
}

/// Lag (0..maxLag) with the largest correlation of y against x.
int bestLag(const std::vector<float>& x, const std::vector<float>& y, int maxLag, std::size_t from, std::size_t to) {
    int best = 0;
    double bestValue = -1e300;
    for (int lag = 0; lag <= maxLag; ++lag) {
        double sum = 0.0;
        for (std::size_t i = from; i < to; ++i) {
            sum += static_cast<double>(x[i]) * y[i + static_cast<std::size_t>(lag)];
        }
        if (sum > bestValue) {
            bestValue = sum;
            best = lag;
        }
    }
    return best;
}

} // namespace

TEST_CASE("Reduce Noise lowers steady noise", "[audio][dsp][denoise]") {
    dsp::Denoiser denoiser(2, 48000.0);
    REQUIRE(denoiser.active());
    // Hiss alone (about -45 dBFS RMS). RNNoise 0.2 attenuates noise-only synthetic white noise by about 4 dB
    // (measured identically with upstream's own rnnoise_demo); it does most of its work around speech.
    auto x = noise(0.0097, 4.0);
    const auto in = x;
    denoiser.process(x.data(), static_cast<int>(x.size() / 2));
    const double before = rms(in, 2, 96000, 192000);
    const double after = rms(x, 2, 96000, 192000);
    INFO("noise " << ampToDb(before) << " dB -> " << ampToDb(after) << " dB");
    CHECK(ampToDb(after) < ampToDb(before) - 3.0);
}

TEST_CASE("Reduce Noise keeps a voice and reports its latency exactly", "[audio][dsp][denoise]") {
    const auto clean = voice(3.0);
    auto noisy = clean;
    const auto hiss = noise(0.005, 3.0, 1);
    for (std::size_t i = 0; i < noisy.size(); ++i) {
        noisy[i] += hiss[i];
    }
    dsp::Denoiser denoiser(1, 48000.0);
    auto y = noisy;
    denoiser.process(y.data(), static_cast<int>(y.size()));
    REQUIRE(denoiser.latencyFrames() == dsp::Denoiser::kLatencyFrames);
    // The cleaned signal lines up with the input exactly `latency` frames later...
    CHECK(bestLag(clean, y, 3 * dsp::Denoiser::kLatencyFrames / 2, 24000, 96000) == denoiser.latencyFrames());
    // ...and keeps most of the voice: within 6 dB of its level once the network settled.
    const auto latency = static_cast<std::size_t>(denoiser.latencyFrames());
    double voiceEnergy = 0.0;
    double outEnergy = 0.0;
    for (std::size_t i = 48000; i < 120000; ++i) {
        voiceEnergy += static_cast<double>(clean[i]) * clean[i];
        outEnergy += static_cast<double>(y[i + latency]) * y[i + latency];
    }
    CHECK(10.0 * std::log10(outEnergy / voiceEnergy) > -6.0);
    // Between syllables only the hiss is left, and it is turned down.
    double gapIn = 0.0;
    double gapOut = 0.0;
    for (std::size_t i = 48000; i < 132000; ++i) {
        const double phase = std::fmod(static_cast<double>(i) / 48000.0, 0.45);
        if (phase > 0.33 && phase < 0.43) {
            gapIn += static_cast<double>(noisy[i]) * noisy[i];
            gapOut += static_cast<double>(y[i + latency]) * y[i + latency];
        }
    }
    const double reduction = 10.0 * std::log10(gapIn / gapOut);
    INFO("noise between syllables reduced by " << reduction << " dB");
    CHECK(reduction > 3.0);
}

TEST_CASE("Reduce Noise amount 0 is the delayed original; other rates pass through", "[audio][dsp][denoise]") {
    dsp::Denoiser denoiser(2, 48000.0);
    denoiser.setAmount(0.0);
    const auto x = noise(0.3, 0.5);
    auto y = x;
    denoiser.process(y.data(), static_cast<int>(y.size() / 2));
    const auto d = static_cast<std::size_t>(denoiser.latencyFrames()) * 2;
    bool exact = true;
    for (std::size_t i = d; i < y.size(); ++i) {
        exact = exact && y[i] == x[i - d];
    }
    CHECK(exact);

    dsp::Denoiser other(2, 44100.0);
    CHECK_FALSE(other.active());
    CHECK(other.latencyFrames() == 0);
    auto z = x;
    other.process(z.data(), static_cast<int>(z.size() / 2));
    CHECK(z == x);
}

TEST_CASE("Reduce Noise reset restarts the stream deterministically", "[audio][dsp][denoise]") {
    dsp::Denoiser denoiser(1, 48000.0);
    const auto x = voice(1.0);
    auto a = x;
    denoiser.process(a.data(), static_cast<int>(a.size()));
    denoiser.reset();
    auto b = x;
    // Different block sizes: same output.
    for (std::size_t i = 0; i < b.size(); i += 333) {
        denoiser.process(b.data() + i, static_cast<int>(std::min<std::size_t>(333, b.size() - i)));
    }
    CHECK(a == b);
}
