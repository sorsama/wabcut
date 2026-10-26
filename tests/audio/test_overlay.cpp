// 0.5: the AudioEngine's overlay (setOverlay; audio scrubbing plays through it): rendered every block whatever the
// transport does (paused, playing, a shuttle speed), added on top of the program, never moving the clock, on the
// device's own time; a replaced overlay is released off the audio thread.

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/Sources.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using namespace std::chrono_literals;

namespace {

/// A constant level on every channel; remembers the times it was asked for.
struct Level final : AudioSource {
    explicit Level(float v) : value(v) {}
    void render(std::span<float> out, int, Rate, Ticks start) noexcept override {
        std::ranges::fill(out, value);
        last = start;
        ++calls;
    }
    float value;
    Ticks last;
    int calls = 0;
};

AudioDeviceConfig exactNull() {
    AudioDeviceConfig c;
    c.backend = AudioBackend::Null;
    c.declickTime = Ticks{}; // exact sample values
    return c;
}

} // namespace

TEST_CASE("The engine's overlay is heard while paused and adds to the program without moving the clock",
          "[audio][engine][overlay]") {
    AudioEngine engine;
    REQUIRE(engine.start(exactNull()));
    const auto t0 = std::chrono::steady_clock::now() - 10min;
    engine.setTimeline(Ticks::fromSeconds(3));
    std::vector<float> buffer(480 * 2);
    REQUIRE(engine.pull(buffer, t0));
    const Ticks parked = engine.clockAt(t0 + 10ms);
    REQUIRE(parked == Ticks::fromSeconds(3));

    // Paused: the program is silent, the overlay sounds.
    auto overlay = std::make_shared<Level>(0.25f);
    engine.setOverlay(overlay);
    CHECK(engine.overlay() == overlay);
    REQUIRE(engine.pull(buffer, t0 + 10ms));
    CHECK(std::ranges::all_of(buffer, [](float x) { return x == 0.25f; }));
    // Its time is the device's (frames delivered so far), not the timeline's.
    CHECK(overlay->last == samplesToTicks(480, Rate{48000, 1}));
    for (int k = 2; k < 10; ++k) {
        REQUIRE(engine.pull(buffer, t0 + k * 10ms));
    }
    CHECK(engine.isPaused());
    CHECK(engine.clockAt(t0 + 100ms) == parked);

    // Playing: added on top of the program.
    engine.setSource(std::make_shared<Level>(0.5f));
    engine.resume();
    REQUIRE(engine.pull(buffer, t0 + 100ms));
    CHECK(std::ranges::all_of(buffer, [](float x) { return x == 0.75f; }));
    // At a muted shuttle speed (8x): the program is silent, the overlay still sounds.
    engine.setSpeed({8, 1});
    REQUIRE(engine.pull(buffer, t0 + 110ms));
    CHECK(std::ranges::all_of(buffer, [](float x) { return x == 0.25f; }));
    engine.setSpeed({1, 1});

    // Replaced: the old one is released (off the audio thread, by the next control call); none: just the program.
    const int calls = overlay->calls;
    engine.setOverlay(nullptr);
    REQUIRE(engine.pull(buffer, t0 + 120ms));
    engine.setOverlay(nullptr); // a control call collects what the audio thread let go of
    CHECK(overlay.use_count() == 1);
    CHECK(overlay->calls == calls);
    CHECK(std::ranges::all_of(buffer, [](float x) { return x == 0.5f; }));
    engine.stop();
}
