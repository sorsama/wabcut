// P.2 (E6): Premiere's Loop. The free-run transport wraps inside the loop (the audio engine's TransportMap does the
// same for sound), the controller keeps the loop across transports, and playback over real media keeps presenting
// frames of the range, from its start again after each wrap, without stopping.

#include "PlaybackTestSupport.h"

#include <wabcut/model/EditSession.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/playback/video/TransportClock.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::playback::test;
using namespace std::chrono_literals;

namespace {

constexpr const char* kClip = "h264_1080p60_aac.mp4"; // 600 frames, barcode = frame index
constexpr std::int64_t kSourceIn = 100;

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }
std::int64_t ns(double s) { return static_cast<std::int64_t>(s * 1e9); }

/// |a - b| within 5 ms (the transport anchors at its own "now").
bool near(Ticks a, Ticks b) { return std::llabs(a.value - b.value) <= seconds(0.005).value; }

} // namespace

TEST_CASE("The free-run transport wraps inside its loop and only from inside or before it", "[playback][loop]") {
    const auto t = makeFreeRunTransport();
    t->setLoop(TimeRange::fromStartEnd(seconds(1), seconds(2)));
    t->seek(seconds(1.5));
    t->play();
    const std::int64_t now = steadyNowNs();
    CHECK(near(t->transportTime(now + ns(0.25)), seconds(1.75)));
    CHECK(near(t->transportTime(now + ns(0.75)), seconds(1.25))); // wrapped once
    CHECK(near(t->transportTime(now + ns(2.75)), seconds(1.25))); // three times
    SECTION("from before the loop, playback runs into it and wraps at its end") {
        t->seek(seconds(0.5));
        const std::int64_t at = steadyNowNs();
        CHECK(near(t->transportTime(at + ns(0.75)), seconds(1.25)));
        CHECK(near(t->transportTime(at + ns(1.75)), seconds(1.25)));
    }
    SECTION("from beyond its end, playback is not wrapped back (TransportMap's rule)") {
        t->seek(seconds(3));
        const std::int64_t at = steadyNowNs();
        CHECK(near(t->transportTime(at + ns(1)), seconds(4)));
    }
    SECTION("reverse playback wraps below the start to the end") {
        t->setSpeed({-1, 1});
        t->seek(seconds(1.25));
        const std::int64_t at = steadyNowNs();
        CHECK(near(t->transportTime(at + ns(0.5)), seconds(1.75)));
    }
    SECTION("no loop: straight on") {
        t->setLoop(std::nullopt);
        t->seek(seconds(1.5));
        const std::int64_t at = steadyNowNs();
        CHECK(near(t->transportTime(at + ns(1)), seconds(2.5)));
    }
    SECTION("pausing keeps the wrapped position") {
        // 0.75 s later the transport is at 1.25 s (wrapped); pause re-anchors there.
        while (steadyNowNs() < now + ns(0.75)) {
        }
        t->pause();
        const Ticks parked = t->transportTime(steadyNowNs());
        CHECK(parked >= seconds(1));
        CHECK(parked < seconds(2));
    }
}

TEST_CASE("The controller keeps its loop on a new transport", "[playback][loop]") {
    PlaybackController controller;
    CHECK_FALSE(controller.loop());
    const TimeRange range = TimeRange::fromStartEnd(seconds(2), seconds(3));
    controller.setLoop(range);
    CHECK(controller.loop() == range);
    controller.setLoop(TimeRange{seconds(5), Ticks{}}); // empty: off
    CHECK_FALSE(controller.loop());
    controller.setLoop(range);
    // A new clock (the audio device started) loops the same range.
    auto clock = makeFreeRunTransport();
    controller.setTransport(clock);
    clock->seek(seconds(2.5));
    clock->play();
    CHECK(clock->transportTime(steadyNowNs() + ns(1)) < seconds(3));
}

TEST_CASE("Loop playback presents the range again from its start without stopping", "[playback][loop][scheduler]") {
    if (!haveMedia(kClip)) {
        SKIP("test media missing");
    }
    const TestProject project = makeProject({{kClip, 0, 0, 120, kSourceIn}});
    EditSession session(project.state);
    PlaybackController controller;
    controller.setSession(&session);
    controller.setSequence(project.sequence);
    const Ticks in = framesToTicks(20, kRate60);
    const Ticks out = framesToTicks(32, kRate60); // 12 frames: 0.2 s
    controller.setLoop(TimeRange::fromStartEnd(in, out));
    controller.seek(in);
    (void)consume(*controller.mailbox(), 300ms);
    controller.play();
    const std::vector<Presented> seen = consume(*controller.mailbox(), 900ms);
    CHECK(controller.isPlaying()); // a loop never runs into the end
    int wraps = 0;
    Ticks previous = in;
    for (const Presented& p : seen) {
        if (p.layers == 0) {
            continue;
        }
        INFO("target frame " << ticksToFrames(p.target, kRate60));
        CHECK(p.target >= in);
        CHECK(p.target < out); // nothing past the Out is shown
        CHECK(p.barcode == kSourceIn + ticksToFrames(p.target, kRate60)); // frame accurate
        if (p.target < previous) {
            ++wraps;
        }
        previous = p.target;
    }
    CHECK(wraps >= 2); // 0.9 s over a 0.2 s loop
    controller.pause();
}
