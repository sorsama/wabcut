// Speed audio renderers (A.10, audio/dsp/SpeedRender.h): varispeed and the pitch-keeping stretch driven by source
// positions - pitch, sample-exact 100 % reverse, block and seek independence (preview == export), speed limits,
// click positions, and the cost per voice.

#include <wabcut/audio/dsp/SpeedRender.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numbers>
#include <vector>

using namespace wabcut::audio::dsp;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

/// A stereo source from a function of the frame (right = left * 0.5); silence before frame 0 and from `length` on.
class FunctionSource final : public SpeedSource {
public:
    FunctionSource(std::function<double(std::int64_t)> f, std::int64_t length) : m_f(std::move(f)), m_length(length) {}
    void read(std::int64_t first, std::int64_t count, float* dst) override {
        ++reads;
        for (std::int64_t i = 0; i < count; ++i) {
            const std::int64_t k = first + i;
            const float v = k >= 0 && k < m_length ? static_cast<float>(m_f(k)) : 0.0f;
            dst[2 * i] = v;
            dst[2 * i + 1] = v * 0.5f;
        }
    }
    float at(std::int64_t k) const { return k >= 0 && k < m_length ? static_cast<float>(m_f(k)) : 0.0f; }
    int reads = 0;

private:
    std::function<double(std::int64_t)> m_f;
    std::int64_t m_length;
};

/// position(f) = from + speed * (f - origin).
class LinearPositions final : public SpeedPositions {
public:
    LinearPositions(double from, double speed, std::int64_t origin = 0) : m_from(from), m_speed(speed), m_origin(origin) {}
    void fill(std::int64_t first, int count, double* out) const override {
        for (int i = 0; i < count; ++i) {
            out[i] = m_from + m_speed * static_cast<double>(first + i - m_origin);
        }
    }

private:
    double m_from;
    double m_speed;
    std::int64_t m_origin;
};

class FunctionPositions final : public SpeedPositions {
public:
    explicit FunctionPositions(std::function<double(std::int64_t)> f) : m_f(std::move(f)) {}
    void fill(std::int64_t first, int count, double* out) const override {
        for (int i = 0; i < count; ++i) {
            out[i] = m_f(first + i);
        }
    }

private:
    std::function<double(std::int64_t)> m_f;
};

std::function<double(std::int64_t)> sine(double hz, double amplitude = 0.5) {
    return [=](std::int64_t k) { return amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(k) / kRate); };
}

/// Renders [first, first + count) in blocks of the given sizes (cycled), left channel only.
std::vector<float> renderLeft(SpeedRenderer& r, std::int64_t first, std::int64_t count, const SpeedPositions& p, SpeedSource& s,
                              std::vector<int> blocks = {512}) {
    std::vector<float> out(static_cast<std::size_t>(count));
    std::vector<float> block;
    std::size_t b = 0;
    for (std::int64_t done = 0; done < count;) {
        const auto n = static_cast<int>(std::min<std::int64_t>(blocks[b++ % blocks.size()], count - done));
        block.assign(static_cast<std::size_t>(n) * 2, 0.0f);
        r.render(first + done, n, p, s, block.data());
        for (int i = 0; i < n; ++i) {
            out[static_cast<std::size_t>(done + i)] = block[static_cast<std::size_t>(2 * i)];
        }
        done += n;
    }
    return out;
}

/// Frequency from rising zero crossings (interpolated) over the signal.
double frequencyOf(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double first = -1.0;
    double last = -1.0;
    int crossings = 0;
    for (std::size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double at = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0.0) {
                first = at;
            } else {
                ++crossings;
            }
            last = at;
        }
    }
    return crossings > 0 ? crossings * kRate / (last - first) : 0.0;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * x[i];
    }
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(to - from, 1)));
}

} // namespace

TEST_CASE("Speed audio: the audible speed range fades at its limits", "[audio][dsp][speed]") {
    CHECK(audibleSpeedGain(1.0) == 1.0);
    CHECK(audibleSpeedGain(-1.0) == 1.0);
    CHECK(audibleSpeedGain(kMinAudibleSpeed) == 1.0);
    CHECK(audibleSpeedGain(kMaxAudibleSpeed) == 1.0);
    CHECK(audibleSpeedGain(0.09) == Approx(0.5));
    CHECK(audibleSpeedGain(0.05) == 0.0);
    CHECK(audibleSpeedGain(0.0) == 0.0);
    CHECK(audibleSpeedGain(22.0) == Approx(0.5));
    CHECK(audibleSpeedGain(30.0) == 0.0);
    CHECK(audibleSpeedGain(std::nan("")) == 0.0);
}

TEST_CASE("Speed audio: varispeed copies samples at 100 percent and reverses the source exactly", "[audio][dsp][speed]") {
    FunctionSource source(sine(997.0), 1'000'000);
    auto r = makeSpeedRenderer(SpeedMode::Varispeed, 2, kRate);
    r->setPiece(1000, 5000);
    // Forwards at whole-frame positions: the samples themselves (inside the piece, where the coverage is 1).
    const LinearPositions forward(20000.0, 1.0, 1000);
    const auto out = renderLeft(*r, 1000, 4000, forward, source);
    for (std::int64_t i = 0; i < 4000; ++i) {
        REQUIRE(out[static_cast<std::size_t>(i)] == source.at(20000 + i));
    }
    // Reverse at 100 %: the time-reversed source, sample for sample.
    auto back = makeSpeedRenderer(SpeedMode::Varispeed, 2, kRate);
    back->setPiece(1000, 5000);
    const LinearPositions reverse(30000.0, -1.0, 1000);
    const auto rev = renderLeft(*back, 1000, 4000, reverse, source, {333, 1024});
    for (std::int64_t i = 0; i < 4000; ++i) {
        REQUIRE(rev[static_cast<std::size_t>(i)] == source.at(30000 - i));
    }
}

TEST_CASE("Speed audio: varispeed moves the pitch with the speed and the stretch keeps it", "[audio][dsp][speed]") {
    FunctionSource source(sine(1000.0), 10'000'000);
    for (const double speed : {0.5, 2.0, -0.5, -2.0}) {
        INFO("speed " << speed);
        const LinearPositions positions(200000.0, speed, 0);
        auto vari = makeSpeedRenderer(SpeedMode::Varispeed, 2, kRate);
        vari->setPiece(0, 96000);
        const auto v = renderLeft(*vari, 0, 96000, positions, source);
        CHECK(frequencyOf(v, 4800, 91200) == Approx(1000.0 * std::fabs(speed)).epsilon(0.01));
        auto keep = makeSpeedRenderer(SpeedMode::MaintainPitch, 2, kRate);
        keep->setPiece(0, 96000);
        const auto k = renderLeft(*keep, 0, 96000, positions, source);
        CHECK(frequencyOf(k, 4800, 91200) == Approx(1000.0).epsilon(0.01));
        // The level holds too (a sine of amplitude 0.5: RMS 0.354).
        CHECK(rms(k, 4800, 91200) == Approx(0.3536).epsilon(0.05));
        CHECK(rms(v, 4800, 91200) == Approx(0.3536).epsilon(0.02));
    }
}

TEST_CASE("Speed audio: output does not depend on block sizes or where rendering starts", "[audio][dsp][speed]") {
    // A ramp from 0.3x up to 1.4x and back to 0.3x, over dense material (a sum of incommensurate sines).
    FunctionSource source([](std::int64_t k) {
        const double t = static_cast<double>(k) / kRate;
        return 0.2 * std::sin(2 * std::numbers::pi * 211.0 * t) + 0.15 * std::sin(2 * std::numbers::pi * 1733.3 * t) +
               0.1 * std::sin(2 * std::numbers::pi * 4211.7 * t + 0.3);
    }, 10'000'000);
    const FunctionPositions ramp([](std::int64_t f) {
        const double u = static_cast<double>(f) / 96000.0; // 0..1 over the piece
        const double speedIntegral = 0.3 * u + 2.2 * (u * u / 2.0) * 2.0 * (1.0 - u / 1.5); // smooth, monotonic
        return 50000.0 + 96000.0 * speedIntegral;
    });
    for (const SpeedMode mode : {SpeedMode::Varispeed, SpeedMode::MaintainPitch}) {
        INFO((mode == SpeedMode::Varispeed ? "varispeed" : "maintain pitch"));
        auto whole = makeSpeedRenderer(mode, 2, kRate);
        whole->setPiece(0, 96000);
        const std::int64_t from = whole->extentBegin();
        const std::int64_t to = whole->extentEnd();
        const auto reference = renderLeft(*whole, from, to - from, ramp, source, {4096});
        auto blocks = makeSpeedRenderer(mode, 2, kRate);
        blocks->setPiece(0, 96000);
        CHECK(renderLeft(*blocks, from, to - from, ramp, source, {1, 7, 480, 333, 1024, 64}) == reference);
        // A start in the middle (a seek): the same samples from there on, and again after jumping back.
        for (const std::int64_t start : {std::int64_t{31111}, std::int64_t{70000}, std::int64_t{5}}) {
            auto seek = makeSpeedRenderer(mode, 2, kRate);
            seek->setPiece(0, 96000);
            const auto tail = renderLeft(*seek, start, to - start, ramp, source, {512});
            CHECK(std::equal(tail.begin(), tail.end(), reference.begin() + (start - from)));
        }
    }
}

TEST_CASE("Speed audio: a piece fades in and out by its coverage, which complements the audio around it",
          "[audio][dsp][speed]") {
    for (const SpeedMode mode : {SpeedMode::Varispeed, SpeedMode::MaintainPitch}) {
        auto r = makeSpeedRenderer(mode, 2, kRate);
        r->setPiece(10000, 20000);
        CHECK(r->extentBegin() <= 10000 - 32);
        CHECK(r->extentEnd() >= 20000 + 32);
        CHECK(r->coverage(r->extentBegin() - 1) == 0.0);
        CHECK(r->coverage(r->extentEnd()) == 0.0);
        for (std::int64_t f = 10000; f < 20000; f += 37) {
            REQUIRE(r->coverage(f) == Approx(1.0).margin(1e-6));
        }
        double previous = 0.0;
        for (std::int64_t f = r->extentBegin(); f < 10000; ++f) { // rises monotonically
            REQUIRE(r->coverage(f) >= previous - 1e-6); // float windows: within rounding
            previous = r->coverage(f);
        }
        // At 100 % on whole frames a piece reproduces the source (the stretch within float rounding), so a 100 % read
        // faded by 1 - coverage plus the piece is the source again across the join.
        FunctionSource source(sine(440.0), 1'000'000);
        const LinearPositions unity(0.0, 1.0, 0);
        const auto out = renderLeft(*r, r->extentBegin(), r->extentEnd() - r->extentBegin(), unity, source);
        double worst = 0.0;
        for (std::int64_t f = r->extentBegin(); f < r->extentEnd(); ++f) {
            const double joined = source.at(f) * (1.0 - r->coverage(f)) + out[static_cast<std::size_t>(f - r->extentBegin())];
            worst = std::max(worst, std::fabs(joined - source.at(f)));
        }
        CHECK(worst < 1e-5);
    }
}

TEST_CASE("Speed audio: speeds outside the audible range are silent", "[audio][dsp][speed]") {
    FunctionSource source(sine(1000.0), 100'000'000);
    for (const SpeedMode mode : {SpeedMode::Varispeed, SpeedMode::MaintainPitch}) {
        for (const double speed : {0.05, -0.05, 30.0}) {
            auto r = makeSpeedRenderer(mode, 2, kRate);
            r->setPiece(0, 48000);
            const LinearPositions p(1'000'000.0, speed, 0);
            const auto out = renderLeft(*r, 0, 48000, p, source);
            CHECK(rms(out, 0, 48000) == 0.0);
        }
    }
}

TEST_CASE("Speed audio: clicks land where the position puts them", "[audio][dsp][speed]") {
    // A click every 4801 frames (an impulse of 0.8).
    const auto clickAt = [](std::int64_t k) { return k % 4801 == 100 ? 0.8 : 0.0; };
    FunctionSource source(clickAt, 10'000'000);
    for (const double speed : {0.5, 0.37, 2.0, -0.5}) {
        INFO("speed " << speed);
        const double from = speed > 0 ? 0.0 : 400000.0;
        const LinearPositions p(from, speed, 0);
        auto r = makeSpeedRenderer(SpeedMode::Varispeed, 2, kRate);
        r->setPiece(0, 200000);
        const auto out = renderLeft(*r, 0, 200000, p, source);
        // Each click's peak sits at the frame whose position is the click (within a frame; the spec allows 1 ms).
        int checked = 0;
        for (std::int64_t c = 100; c < 400000; c += 4801) {
            const double frame = (static_cast<double>(c) - from) / speed;
            if (frame < 1000 || frame > 199000) {
                continue;
            }
            const auto centre = static_cast<std::int64_t>(std::llround(frame));
            std::int64_t peak = centre;
            for (std::int64_t f = centre - 96; f <= centre + 96; ++f) {
                if (std::fabs(out[static_cast<std::size_t>(f)]) > std::fabs(out[static_cast<std::size_t>(peak)])) {
                    peak = f;
                }
            }
            CHECK(std::fabs(static_cast<double>(peak) - frame) <= 1.0);
            ++checked;
        }
        CHECK(checked > 10);
    }
    // The stretch keeps them in sync within its search tolerance: the energy centre of each click's copies.
    for (const double speed : {0.5, 2.0}) {
        INFO("stretch, speed " << speed);
        const LinearPositions p(0.0, speed, 0);
        auto r = makeSpeedRenderer(SpeedMode::MaintainPitch, 2, kRate);
        r->setPiece(0, 200000);
        const auto out = renderLeft(*r, 0, 200000, p, source);
        double worst = 0.0;
        for (std::int64_t c = 100; c < 400000; c += 4801) {
            const double frame = static_cast<double>(c) / speed;
            if (frame < 2000 || frame > 198000) {
                continue;
            }
            double energy = 0.0;
            double moment = 0.0;
            const auto centre = static_cast<std::int64_t>(frame);
            for (std::int64_t f = centre - 1200; f <= centre + 1200; ++f) {
                const double e = static_cast<double>(out[static_cast<std::size_t>(f)]) * out[static_cast<std::size_t>(f)];
                energy += e;
                moment += e * static_cast<double>(f);
            }
            REQUIRE(energy > 0.0);
            worst = std::max(worst, std::fabs(moment / energy - frame));
        }
        std::printf("speed audio: stretch click centre within %.2f ms at %.2fx\n", worst / 48.0, speed);
        CHECK(worst / 48.0 < 8.0);
    }
}

TEST_CASE("Speed audio: what one stretched voice costs at 48 kHz", "[audio][dsp][speed][perf]") {
    FunctionSource source([](std::int64_t k) {
        const double t = static_cast<double>(k) / kRate;
        return 0.3 * std::sin(2 * std::numbers::pi * 220.0 * t) + 0.2 * std::sin(2 * std::numbers::pi * 1500.0 * t);
    }, 100'000'000);
    for (const SpeedMode mode : {SpeedMode::Varispeed, SpeedMode::MaintainPitch}) {
        for (const double speed : {0.5, 2.0}) {
            auto r = makeSpeedRenderer(mode, 2, kRate);
            constexpr std::int64_t kFrames = 480000; // 10 s of output
            r->setPiece(0, kFrames);
            const LinearPositions p(1000.0, speed, 0);
            const auto t0 = std::chrono::steady_clock::now();
            const auto out = renderLeft(*r, 0, kFrames, p, source, {480});
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("speed audio cost: %s %.1fx: %.2f ms per second of stereo output (%.2f %% of a core)\n",
                        mode == SpeedMode::Varispeed ? "varispeed" : "maintain pitch", speed, ms / 10.0, ms / 100.0);
            CHECK(rms(out, 48000, kFrames) > 0.1);
            CHECK(ms / 10.0 < 100.0); // under 10 % of a core per voice even unoptimised
        }
    }
}
