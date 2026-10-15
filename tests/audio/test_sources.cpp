#include <wabcut/audio/OfflineRenderer.h>
#include <wabcut/audio/Sources.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using Catch::Approx;

namespace {

constexpr Rate k48k{48000, 1};

/// Goertzel power of `x` (one channel taken every `stride` samples) at `frequency`, normalised so a full-scale
/// sine of amplitude A at that frequency reads A.
double toneAmplitude(const std::vector<float>& x, std::size_t stride, double frequency, double sampleRate) {
    const std::size_t n = x.size() / stride;
    const double w = 2.0 * std::numbers::pi * frequency / sampleRate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double s0 = x[i * stride] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt(std::max(power, 0.0)) / static_cast<double>(n);
}

} // namespace

TEST_CASE("Offline render of SineSource has the expected frequency and level", "[audio][sources][offline]") {
    auto sine = std::make_shared<SineSource>(1000.0, 0.5f);
    auto renderer = OfflineRenderer::create(sine, 2, k48k, 512);
    REQUIRE(renderer);
    auto rendered = renderer->render(48000); // exactly one second
    REQUIRE(rendered);
    const std::vector<float>& x = *rendered;
    REQUIRE(x.size() == 96000);
    CHECK(renderer->position() == Ticks::fromSeconds(1));

    // Channels are identical.
    bool identical = true;
    for (std::size_t i = 0; i < 48000; ++i) {
        identical = identical && x[2 * i] == x[2 * i + 1];
    }
    CHECK(identical);

    // Level: peak and RMS of a 0.5-amplitude sine.
    double peak = 0.0;
    double sumSquares = 0.0;
    for (std::size_t i = 0; i < 48000; ++i) {
        peak = std::max(peak, std::fabs(static_cast<double>(x[2 * i])));
        sumSquares += static_cast<double>(x[2 * i]) * x[2 * i];
    }
    CHECK(peak == Approx(0.5).epsilon(1e-6));
    CHECK(std::sqrt(sumSquares / 48000.0) == Approx(0.5 / std::sqrt(2.0)).epsilon(1e-6));

    // Frequency: count rising zero crossings and refine with linear interpolation of the first and last one.
    int rising = 0;
    double firstCrossing = -1.0;
    double lastCrossing = -1.0;
    for (std::size_t i = 1; i < 48000; ++i) {
        const double a = x[2 * (i - 1)];
        const double b = x[2 * i];
        if (a < 0.0 && b >= 0.0) {
            const double at = static_cast<double>(i - 1) + a / (a - b);
            if (firstCrossing < 0.0) {
                firstCrossing = at;
            }
            lastCrossing = at;
            ++rising;
        }
    }
    CHECK((rising == 999 || rising == 1000));
    const double measuredHz = (rising - 1) / ((lastCrossing - firstCrossing) / 48000.0);
    CHECK(measuredHz == Approx(1000.0).margin(0.01));

    // Spectrally pure: all of the energy is at 1 kHz.
    CHECK(toneAmplitude(x, 2, 1000.0, 48000.0) == Approx(0.5).epsilon(1e-4));
    CHECK(toneAmplitude(x, 2, 900.0, 48000.0) < 1e-4);
    CHECK(toneAmplitude(x, 2, 2000.0, 48000.0) < 1e-4);
}

TEST_CASE("SineSource is a pure function of source time", "[audio][sources][offline]") {
    auto sine = std::make_shared<SineSource>(440.0, 0.25f);

    // Same blocks -> bit-identical.
    auto a = OfflineRenderer::create(sine, 1, k48k, 480)->render(9600);
    auto b = OfflineRenderer::create(sine, 1, k48k, 480)->render(9600);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a == *b);

    // Seek into the middle: matches the continuous render.
    auto seeked = OfflineRenderer::create(sine, 1, k48k, 480);
    REQUIRE(seeked);
    seeked->seek(samplesToTicks(4800, k48k));
    auto tail = seeked->render(4800);
    REQUIRE(tail);
    double maxDiff = 0.0;
    for (std::size_t i = 0; i < 4800; ++i) {
        maxDiff = std::max(maxDiff, std::fabs(static_cast<double>((*tail)[i]) - (*a)[4800 + i]));
    }
    CHECK(maxDiff < 1e-6);

    // Far into a long timeline the phase is still exact: at t = 10 h + 1/(4·440) s the 440 Hz sine is at its peak.
    std::vector<float> one(1);
    const Ticks tenHours = Ticks::fromSeconds(36000);
    sine->render(one, 1, k48k, tenHours + Ticks{kTicksPerSecond / (4 * 440)});
    CHECK(one[0] == Approx(0.25f).epsilon(1e-6));
    sine->render(one, 1, k48k, tenHours);
    CHECK(one[0] == Approx(0.0f).margin(1e-6));
}

TEST_CASE("SilenceSource and RingBufferSource", "[audio][sources]") {
    std::vector<float> buf(64, 3.0f);
    SilenceSource{}.render(buf, 2, k48k, Ticks{});
    CHECK(std::ranges::all_of(buf, [](float v) { return v == 0.0f; }));

    auto ring = std::make_shared<SpscFrameRing>(64, 2);
    RingBufferSource source(ring);
    const std::vector<float> data{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    REQUIRE(ring->write(data) == 3);
    std::vector<float> out(10, 9.0f); // 5 frames requested, 3 available
    source.render(out, 2, k48k, Ticks{});
    CHECK(out[0] == 0.1f);
    CHECK(out[5] == 0.6f);
    CHECK(out[6] == 0.0f);
    CHECK(out[9] == 0.0f);
    CHECK(source.underrunFrames() == 2);

    // Wrong channel count: silence, all frames counted as underrun.
    REQUIRE(ring->write(data) == 3);
    std::vector<float> mono(4, 9.0f);
    source.render(mono, 1, k48k, Ticks{});
    CHECK(std::ranges::all_of(mono, [](float v) { return v == 0.0f; }));
    CHECK(source.underrunFrames() == 6);
}

TEST_CASE("OfflineRenderer validates its arguments", "[audio][offline]") {
    auto sine = std::make_shared<SineSource>(100.0);
    CHECK_FALSE(OfflineRenderer::create(nullptr, 2, k48k).has_value());
    CHECK(OfflineRenderer::create(sine, 0, k48k).error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(OfflineRenderer::create(sine, 2, Rate{44099, 1}).has_value()); // not tick-exact
    CHECK_FALSE(OfflineRenderer::create(sine, 2, k48k, 0).has_value());
    auto renderer = OfflineRenderer::create(sine, 2, k48k);
    REQUIRE(renderer);
    std::vector<float> odd(5);
    CHECK_FALSE(renderer->render(std::span<float>(odd)).has_value());
    CHECK(renderer->position() == Ticks{});
    CHECK_FALSE(renderer->render(std::int64_t{-1}).has_value());
}
