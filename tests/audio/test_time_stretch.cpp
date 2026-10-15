// Variable-speed audition (WSOLA): duration, pitch preserved, no clicks at hop or block boundaries.

#include "DspTestUtil.h"

#include <wabcut/audio/dsp/TimeStretch.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using namespace wabcut::audio;
using namespace wabcut::audio::test;
using Catch::Approx;

namespace {

/// Stretches `input` at `speed` the way the player does: push speed x block input frames, pull block output frames.
std::vector<float> stretch(const std::vector<float>& input, int channels, double speed, int block) {
    dsp::TimeStretch ts(channels, 48000.0, 4.0, 8192);
    ts.setSpeed(speed);
    const std::size_t frames = input.size() / static_cast<std::size_t>(channels);
    std::vector<float> out;
    std::vector<float> buffer(static_cast<std::size_t>(block) * static_cast<std::size_t>(channels));
    double pushed = 0.0;
    std::size_t at = 0;
    while (at < frames) {
        pushed += speed * block;
        const auto want = static_cast<std::size_t>(pushed) - at;
        const std::size_t n = std::min(want, frames - at);
        ts.push(input.data() + at * static_cast<std::size_t>(channels), static_cast<int>(n));
        at += n;
        const int got = ts.pull(buffer.data(), block);
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got) * channels);
    }
    return out;
}

/// Frequency of channel 0 from its rising zero crossings over [from, to).
double frequency(const std::vector<float>& x, int channels, std::size_t from, std::size_t to) {
    std::size_t first = 0;
    std::size_t last = 0;
    int crossings = 0;
    for (std::size_t i = from + 1; i < to; ++i) {
        const float a = x[(i - 1) * static_cast<std::size_t>(channels)];
        const float b = x[i * static_cast<std::size_t>(channels)];
        if (a < 0.0f && b >= 0.0f) {
            if (crossings == 0) {
                first = i;
            }
            last = i;
            ++crossings;
        }
    }
    return crossings > 1 ? 48000.0 * (crossings - 1) / static_cast<double>(last - first) : 0.0;
}

} // namespace

TEST_CASE("Time stretch keeps the duration ratio", "[audio][dsp][stretch]") {
    const auto x = sine(220.0, 0.5, 10.0);
    for (const double speed : {0.5, 0.75, 1.5, 2.0, 4.0}) {
        INFO("speed " << speed);
        dsp::TimeStretch ts(2, 48000.0);
        ts.setSpeed(speed);
        long long produced = 0;
        std::vector<float> out(2 * 4096);
        for (std::size_t at = 0; at < x.size() / 2; at += 4096) {
            ts.push(x.data() + at * 2, static_cast<int>(std::min<std::size_t>(4096, x.size() / 2 - at)));
            int got = 0;
            while ((got = ts.pull(out.data(), 4096)) > 0) {
                produced += got;
            }
        }
        CHECK(produced == ts.outputFramesProduced());
        // Output = input / speed, short only by what is still buffered: one window plus the search and one advance.
        const double expected = static_cast<double>(ts.inputFramesPushed()) / speed;
        const double slack = (ts.latencyInputFrames() + speed * 480.0) / speed + 480.0;
        CHECK(static_cast<double>(produced) <= expected + 480.0);
        CHECK(static_cast<double>(produced) >= expected - slack);
    }
}

TEST_CASE("Time stretch preserves pitch", "[audio][dsp][stretch]") {
    const auto x = sine(440.0, 0.5, 4.0);
    for (const double speed : {0.5, 2.0, 4.0}) {
        INFO("speed " << speed);
        const auto y = stretch(x, 2, speed, 512);
        const std::size_t frames = y.size() / 2;
        REQUIRE(frames > 9600);
        CHECK(frequency(y, 2, 4800, frames - 4800) == Approx(440.0).epsilon(0.005));
    }
}

TEST_CASE("Time stretch has no clicks at hop or block boundaries", "[audio][dsp][stretch]") {
    const double amplitude = 0.5;
    const double f = 330.0;
    const auto x = sine(f, amplitude, 4.0);
    const double maxSlope = 2.0 * std::numbers::pi * f / 48000.0 * amplitude; // a clean sine's largest step
    for (const double speed : {0.5, 1.25, 2.0, 4.0}) {
        for (const int block : {64, 441, 1024}) {
            INFO("speed " << speed << ", block " << block);
            const auto y = stretch(x, 2, speed, block);
            const std::size_t frames = y.size() / 2;
            double step = 0.0;
            double low = 1e9;
            double high = 0.0;
            // Skip the fade-in of the first hop; look at the steady part.
            for (std::size_t i = 960; i < frames; ++i) {
                step = std::max(step, static_cast<double>(std::fabs(y[i * 2] - y[(i - 1) * 2])));
            }
            for (std::size_t w = 960; w + 480 < frames; w += 480) {
                const double p = peak(y, 2, w, w + 480);
                low = std::min(low, p);
                high = std::max(high, p);
            }
            CHECK(step < 1.15 * maxSlope);
            CHECK(ampToDb(low / amplitude) > -1.0); // no dips where segments join
            CHECK(ampToDb(high / amplitude) < 0.5);
        }
    }
}

TEST_CASE("Time stretch output does not depend on the pull size; speed changes are smooth", "[audio][dsp][stretch]") {
    const auto x = sine(250.0, 0.4, 3.0);
    const auto a = stretch(x, 2, 2.0, 256);
    const auto b = stretch(x, 2, 2.0, 1000);
    const std::size_t common = std::min(a.size(), b.size());
    REQUIRE(common > 48000);
    CHECK(std::equal(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(common), b.begin()));

    dsp::TimeStretch ts(2, 48000.0);
    std::vector<float> out;
    std::vector<float> buffer(2 * 512);
    const double maxSlope = 2.0 * std::numbers::pi * 250.0 / 48000.0 * 0.4;
    std::size_t at = 0;
    for (int block = 0; at + 4096 < x.size() / 2; ++block) {
        const double speed = block < 40 ? 1.0 : (block < 80 ? 2.0 : 0.5);
        ts.setSpeed(speed);
        const auto n = static_cast<std::size_t>(speed * 512);
        ts.push(x.data() + at * 2, static_cast<int>(n));
        at += n;
        const int got = ts.pull(buffer.data(), 512);
        out.insert(out.end(), buffer.begin(), buffer.begin() + 2 * got);
    }
    double step = 0.0;
    for (std::size_t i = 961; i < out.size() / 2; ++i) {
        step = std::max(step, static_cast<double>(std::fabs(out[i * 2] - out[(i - 1) * 2])));
    }
    CHECK(step < 1.15 * maxSlope);
    ts.reset();
    CHECK(ts.available() == 0);
    CHECK(ts.inputFramesPushed() == 0);
}
