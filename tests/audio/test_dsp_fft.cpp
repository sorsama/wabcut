// p02 audio: the FFT and the streaming STFT the spectral effects are built on.

#include "DspTestUtil.h"

#include <wabcut/audio/dsp/Fft.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

using namespace wabcut::audio;
using namespace wabcut::audio::test;

TEST_CASE("Fft: a sine lands in its bin and the inverse gives the signal back", "[audio][dsp][fft]") {
    dsp::Fft fft(256);
    REQUIRE(fft.size() == 256);
    REQUIRE(fft.bins() == 129);
    std::vector<double> x(256);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = 0.5 * std::cos(2.0 * std::numbers::pi * 10.0 * static_cast<double>(i) / 256.0) + 0.25;
    }
    std::vector<std::complex<double>> bins(129);
    fft.forwardReal(x.data(), bins.data());
    CHECK(bins[0].real() == Catch::Approx(0.25 * 256).margin(1e-9));
    CHECK(std::abs(bins[10]) == Catch::Approx(0.5 * 128).margin(1e-9));
    for (std::size_t k = 1; k < bins.size(); ++k) {
        if (k != 10) {
            CHECK(std::abs(bins[k]) < 1e-9);
        }
    }
    std::vector<double> back(256);
    fft.inverseReal(bins.data(), back.data());
    for (std::size_t i = 0; i < x.size(); ++i) {
        CHECK(back[i] == Catch::Approx(x[i]).margin(1e-12));
    }
}

TEST_CASE("Fft: sizes round up to a power of two", "[audio][dsp][fft]") {
    CHECK(dsp::Fft(1000).size() == 1024);
    CHECK(dsp::Fft(1).size() == 2);
    CHECK(dsp::Stft(2, 100).size() == 128);
    CHECK(dsp::Stft(2, 1024).hop() == 256);
}

TEST_CASE("Stft: an unchanged spectrum gives the input back delayed by the latency", "[audio][dsp][fft]") {
    dsp::Stft stft(2, 1024);
    const int latency = stft.latencyFrames();
    REQUIRE(latency == 1024);
    std::vector<float> x = noise(0.5, 0.5, 2);
    const std::vector<float> in = x;
    // Uneven blocks: the result does not depend on how the stream is cut.
    std::size_t done = 0;
    const std::size_t frames = x.size() / 2;
    int calls = 0;
    while (done < frames) {
        const std::size_t n = std::min<std::size_t>(frames - done, 37 + done % 500);
        stft.process(x.data() + done * 2, static_cast<int>(n), [&](int, std::complex<double>*) { ++calls; });
        done += n;
    }
    CHECK(calls == 2 * static_cast<int>(frames / 256));
    double worst = 0.0;
    for (std::size_t i = static_cast<std::size_t>(latency); i < frames; ++i) {
        for (std::size_t c = 0; c < 2; ++c) {
            worst = std::max(worst, std::fabs(static_cast<double>(x[i * 2 + c]) - in[(i - static_cast<std::size_t>(latency)) * 2 + c]));
        }
    }
    CHECK(worst < 1e-5);
}

TEST_CASE("Stft: zeroing bins above a cutoff removes a high tone and keeps a low one", "[audio][dsp][fft]") {
    dsp::Stft stft(1, 2048);
    std::vector<float> x = sine(200.0, 0.4, 1.0, 1);
    const std::vector<float> high = sine(9000.0, 0.4, 1.0, 1);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] += high[i];
    }
    const int cutoff = static_cast<int>(2000.0 / 48000.0 * 2048.0);
    stft.process(x.data(), static_cast<int>(x.size()), [&](int, std::complex<double>* bins) {
        for (int k = cutoff; k < stft.bins(); ++k) {
            bins[k] = 0.0;
        }
    });
    const std::size_t from = 4096;
    CHECK(toneAmplitude(x, 1, 200.0, from, x.size()) == Catch::Approx(0.4).margin(0.01));
    CHECK(toneAmplitude(x, 1, 9000.0, from, x.size()) < 0.001);
}
