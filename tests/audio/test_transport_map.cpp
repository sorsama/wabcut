// TransportMap traces: pure, no engine, no device, no threads.

#include <wabcut/audio/TransportMap.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace wabcut;
using namespace wabcut::audio;

namespace {

constexpr Rate k48k{48000, 1};
constexpr std::int64_t kTicksPerFrame = 5'292'000; // at 48 kHz

/// Milliseconds as ticks (exact for multiples of 1 µs).
Ticks ms(double v) { return Ticks{static_cast<std::int64_t>(v * 1000.0 + (v >= 0 ? 0.5 : -0.5)) * (kTicksPerSecond / 1'000'000)}; }
/// Frames for a duration in ms at 48 kHz.
std::int64_t frames(double msValue) { return static_cast<std::int64_t>(msValue * 48.0 + 0.5); }

ClockSample clockAt(std::int64_t writtenFrames, std::int64_t hostNs, std::int64_t latency) {
    ClockSample c;
    c.devicePositionFrames = writtenFrames;
    c.hostTimeNs = hostNs;
    c.negotiatedRate = k48k;
    c.latencyFrames = latency;
    c.running = true;
    return c;
}

} // namespace

TEST_CASE("TransportSpeed normalizes and classifies rational speeds", "[audio][transport]") {
    CHECK(TransportSpeed{2, 4} == TransportSpeed{1, 2});
    CHECK(TransportSpeed{-2, -4}.normalized().num == 1);
    CHECK(TransportSpeed{-2, -4}.normalized().den == 2);
    const TransportSpeed reverseHalf = TransportSpeed{3, -6}.normalized();
    CHECK(reverseHalf.num == -1);
    CHECK(reverseHalf.den == 2);
    CHECK(reverseHalf.isReverse());
    CHECK(TransportSpeed{4, 4}.isNormal());
    CHECK_FALSE(TransportSpeed{2, 1}.isNormal());
    CHECK(TransportSpeed{0, 5}.isZero());
    CHECK_FALSE(TransportSpeed{1, 0}.isValid());
}

TEST_CASE("A new TransportMap is paused at its start position", "[audio][transport]") {
    const TransportMap map(k48k, Ticks::fromSeconds(5));
    CHECK_FALSE(map.isPlaying());
    CHECK(map.playSpeed().isNormal());
    CHECK(map.timelineAtFrame(0) == Ticks::fromSeconds(5));
    CHECK(map.timelineAtFrame(1'000'000) == Ticks::fromSeconds(5));
    CHECK(map.transportTime(123'456'789) == Ticks::fromSeconds(5)); // no clock: the commanded position
    CHECK_FALSE(map.hasClock());
    CHECK_FALSE(map.loop().has_value());
}

TEST_CASE("Play, pause and speed changes trace a continuous timeline", "[audio][transport]") {
    TransportMap map(k48k);
    map.play(0);
    CHECK(map.timelineAtFrame(480) == ms(10));
    map.pause(frames(100));
    CHECK(map.timelineAtFrame(frames(100)) == ms(100));
    CHECK(map.timelineAtFrame(frames(200)) == ms(100));
    map.play(frames(200));
    CHECK(map.timelineAtFrame(frames(300)) == ms(200));

    map.setSpeed({2, 1}, frames(300)); // 2x
    CHECK(map.timelineAtFrame(frames(310)) == ms(220));
    map.setSpeed({1, 2}, frames(310)); // 1/2x
    CHECK(map.timelineAtFrame(frames(330)) == ms(230));
    map.setSpeed({-1, 1}, frames(330)); // reverse
    CHECK(map.timelineAtFrame(frames(340)) == ms(220));
    CHECK(map.speedAtFrame(frames(335)).isReverse());
    map.setSpeed({0, 1}, frames(340)); // freeze while "playing"
    CHECK(map.isPlaying());
    CHECK(map.speedAtFrame(frames(400)).isZero());
    CHECK(map.timelineAtFrame(frames(1000)) == ms(220));

    // History stays valid for frames still in flight.
    CHECK(map.timelineAtFrame(frames(50)) == ms(50));
    CHECK(map.timelineAtFrame(frames(150)) == ms(100));
    CHECK(map.timelineAtFrame(frames(305)) == ms(210));

    // Sub-frame precision and fractional speeds are exact integer maths.
    TransportMap fine(k48k);
    fine.setSpeed({7, 3}, 0);
    fine.play(0);
    CHECK(fine.timelineAtFrame(3) == Ticks{7 * kTicksPerFrame});
    CHECK(fine.timelineAtDeviceTicks(3) == Ticks{7}); // floor(3 * 7/3)
    CHECK(fine.timelineAtDeviceTicks(1) == Ticks{2}); // floor(7/3)
}

TEST_CASE("Seek flushes history and holds for the preroll", "[audio][transport][seek]") {
    TransportMap map(k48k);
    map.play(0);
    map.seek(Ticks::fromSeconds(10), frames(100), frames(20));
    CHECK(map.timelineAtFrame(0) == Ticks::fromSeconds(10)); // flushed: in-flight frames report the target
    CHECK(map.timelineAtFrame(frames(100)) == Ticks::fromSeconds(10));
    CHECK(map.timelineAtFrame(frames(119.98)) == Ticks::fromSeconds(10));
    CHECK(map.speedAtFrame(frames(110)).isZero());
    CHECK(map.timelineAtFrame(frames(120)) == Ticks::fromSeconds(10));
    CHECK(map.speedAtFrame(frames(120)).isNormal());
    CHECK(map.timelineAtFrame(frames(130)) == Ticks::fromSeconds(10) + ms(10));

    const TransportMap::Piece preroll = map.piece(frames(100), 4096);
    CHECK(preroll.speed.isZero());
    CHECK(preroll.frames == frames(20)); // ends exactly where playback starts
    CHECK(preroll.timeline == Ticks::fromSeconds(10));

    // setSpeed during the preroll keeps it and changes the pending playback.
    TransportMap fast(k48k);
    fast.play(0);
    fast.seek(Ticks::fromSeconds(5), 100, 480);
    fast.setSpeed({2, 1}, 200);
    CHECK(fast.speedAtFrame(300).isZero());
    CHECK(fast.speedAtFrame(580) == TransportSpeed{2, 1});
    CHECK(fast.timelineAtFrame(580 + 480) == Ticks::fromSeconds(5) + ms(20));

    // Pausing during the preroll cancels the pending start.
    map.pause(frames(110));
    CHECK_FALSE(map.isPlaying());
    CHECK(map.timelineAtFrame(frames(500)) == Ticks::fromSeconds(10));
    // Seeking while paused never prerolls.
    map.seek(Ticks::fromSeconds(20), frames(600), frames(20));
    CHECK(map.segmentCount() == 1);
    CHECK(map.timelineAtFrame(frames(1000)) == Ticks::fromSeconds(20));
    map.play(frames(1000));
    CHECK(map.timelineAtFrame(frames(1010)) == Ticks::fromSeconds(20) + ms(10));
}

TEST_CASE("Loop ranges wrap forward and reverse playback", "[audio][transport][loop]") {
    const TimeRange loop{Ticks::fromSeconds(1), Ticks::fromSeconds(1)}; // [1 s, 2 s)
    TransportMap map(k48k, ms(500));
    map.setLoop(loop, 0);
    map.play(0);
    REQUIRE(map.loop().has_value());
    CHECK(map.timelineAtFrame(frames(500)) == Ticks::fromSeconds(1));
    CHECK(map.timelineAtFrame(frames(1250)) == ms(1750));
    CHECK(map.timelineAtFrame(frames(1500) - 1) == ms(2000) - Ticks{kTicksPerFrame}); // last frame of the lap
    CHECK(map.timelineAtFrame(frames(1500)) == Ticks::fromSeconds(1)); // reached the end: wrapped
    CHECK(map.timelineAtFrame(frames(2000)) == ms(1500));
    CHECK(map.timelineAtFrame(frames(3500)) == Ticks::fromSeconds(1));

    const TransportMap::Piece beforeWrap = map.piece(frames(1490), 4096);
    CHECK(beforeWrap.frames == frames(10)); // stops right at the wrap
    CHECK(beforeWrap.timeline == ms(1990));
    CHECK(map.piece(frames(1500), 4096).timeline == Ticks::fromSeconds(1));

    // Starting beyond the loop end does not wrap back.
    TransportMap beyond(k48k, Ticks::fromSeconds(3));
    beyond.setLoop(loop, 0);
    beyond.play(0);
    CHECK(beyond.timelineAtFrame(frames(1000)) == Ticks::fromSeconds(4));

    // Reverse wraps below the loop start to the end.
    TransportMap reverse(k48k, ms(1200));
    reverse.setLoop(loop, 0);
    reverse.setSpeed({-1, 1}, 0);
    reverse.play(0);
    CHECK(reverse.timelineAtFrame(frames(200)) == Ticks::fromSeconds(1));
    CHECK(reverse.timelineAtFrame(frames(300)) == ms(1900));
    CHECK(reverse.piece(0, 1 << 20).frames == frames(200) + 1); // the frame at exactly 1 s is still in the lap

    // Turning the loop off stops the wrapping from then on.
    map.setLoop(std::nullopt, frames(2000));
    CHECK_FALSE(map.loop().has_value());
    CHECK(map.timelineAtFrame(frames(3000)) == ms(2500));
}

TEST_CASE("Rebasing on a new device epoch keeps the transport continuous", "[audio][transport][epoch]") {
    TransportMap map(k48k);
    map.play(0);
    map.setClock(clockAt(9600, 1'000'000'000, 0));
    REQUIRE(map.hasClock());
    map.rebase(7, frames(200), 100); // the new device's frame 100 continues at 200 ms
    CHECK(map.deviceEpoch() == 7);
    CHECK_FALSE(map.hasClock()); // the old device's clock means nothing in the new domain
    CHECK(map.timelineAtFrame(100) == ms(200));
    CHECK(map.timelineAtFrame(100 + 480) == ms(210));
    CHECK(map.isPlaying());

    TransportMap paused(k48k, ms(50));
    paused.rebase(1, 12345, 0);
    CHECK(paused.timelineAtFrame(48000) == ms(50));
    CHECK_FALSE(paused.isPlaying());
}

TEST_CASE("transportTime follows the clock and hostTimeFor inverts it", "[audio][transport][clock]") {
    TransportMap map(k48k);
    map.play(0);
    constexpr std::int64_t kHost = 1'000'000'000;
    map.setClock(clockAt(frames(100) + 960, kHost, 960)); // frame 4800 (100 ms) heard at kHost
    CHECK(map.transportTime(kHost) == ms(100));
    CHECK(map.transportTime(kHost + 5'000'000) == ms(105));
    CHECK(map.transportTime(kHost + 1) == ms(100) + Ticks{254}); // 1 ns = 254.016 ticks, floored

    auto checkInverse = [&](const TransportMap& m, Ticks t) {
        const auto host = m.hostTimeFor(t);
        REQUIRE(host.has_value());
        CHECK(m.transportTime(*host) >= t);
        CHECK(m.transportTime(*host - 1) < t);
    };
    for (const Ticks t : {ms(100), ms(105), ms(100) + Ticks{1}, ms(2500), Ticks::fromSeconds(3600)}) {
        checkInverse(map, t);
    }
    CHECK(map.hostTimeFor(ms(105)) == kHost + 5'000'000);

    // At 2x, a timeline span is heard in half the host time.
    map.setSpeed({2, 1}, frames(100));
    CHECK(map.transportTime(kHost + 5'000'000) == ms(110));
    checkInverse(map, ms(150));
    CHECK(map.hostTimeFor(ms(150)) == kHost + 25'000'000);

    // Paused: only the frozen position is ever "heard".
    TransportMap paused(k48k, ms(40));
    paused.setClock(clockAt(0, kHost, 0));
    CHECK_FALSE(paused.hostTimeFor(ms(41)).has_value());
    CHECK(paused.hostTimeFor(ms(40)).has_value());

    // Loop: an earlier time inside the loop is heard on the next lap; a time beyond the loop end never.
    TransportMap looping(k48k, ms(1500));
    looping.setLoop(TimeRange{Ticks::fromSeconds(1), Ticks::fromSeconds(1)}, 0);
    looping.play(0);
    looping.setClock(clockAt(0, kHost, 0));
    CHECK(looping.hostTimeFor(ms(1200)) == kHost + 700'000'000); // 500 ms to the wrap + 200 ms
    checkInverse(looping, ms(1200));
    CHECK_FALSE(looping.hostTimeFor(ms(2500)).has_value());

    // Reverse playback inverts too.
    TransportMap reverse(k48k, ms(500));
    reverse.setSpeed({-1, 1}, 0);
    reverse.play(0);
    reverse.setClock(clockAt(0, kHost, 0));
    CHECK(reverse.transportTime(kHost + 100'000'000) == ms(400));
    const auto back = reverse.hostTimeFor(ms(300));
    REQUIRE(back.has_value());
    CHECK(reverse.transportTime(*back) <= ms(300));
    CHECK(reverse.transportTime(*back - 1) > ms(300));
    CHECK_FALSE(reverse.hostTimeFor(ms(600)).has_value()); // behind a reverse playhead
}

TEST_CASE("TransportMap stays exact on long timelines and bounds its history", "[audio][transport]") {
    TransportMap map(k48k);
    map.setSpeed({2, 1}, 0);
    map.play(0);
    const std::int64_t tenDays = 48000LL * 86400 * 10;
    CHECK(map.timelineAtFrame(tenDays) == Ticks::fromSeconds(2LL * 86400 * 10));

    TransportMap busy(k48k);
    for (int i = 0; i < 40; ++i) {
        if (i % 2 == 0) {
            busy.play(i * 480);
        } else {
            busy.pause(i * 480);
        }
    }
    CHECK(busy.segmentCount() <= TransportMap::kMaxSegments);
    CHECK(busy.timelineAtFrame(40 * 480) == ms(200)); // 20 played blocks of 10 ms
}

TEST_CASE("Rebase keeps playing, paused and prerolling states", "[audio][transport][epoch]") {
    SECTION("playing: continuous, same speed") {
        TransportMap map(k48k);
        map.setSpeed({2, 1}, 0);
        map.play(0);
        map.rebase(5, frames(100), 1000);
        CHECK(map.timelineAtFrame(1000) == ms(200));
        CHECK(map.timelineAtFrame(1000 + 480) == ms(220));
        CHECK(map.speedAtFrame(2000) == TransportSpeed{2, 1});
    }
    SECTION("paused: stays paused") {
        TransportMap map(k48k);
        map.play(0);
        map.pause(frames(50));
        map.rebase(5, frames(80), 0);
        CHECK_FALSE(map.isPlaying());
        CHECK(map.speedAtFrame(48000).isZero());
        CHECK(map.timelineAtFrame(48000) == ms(50));
    }
    SECTION("prerolling in the same frame domain (the review's repro)") {
        TransportMap map(k48k);
        map.play(0);
        map.seek(Ticks::fromSeconds(10), 0, 48000); // 1 s of preroll
        map.rebase(2, 1, 1);
        CHECK(map.deviceEpoch() == 2);
        CHECK(map.speedAtFrame(24000).isZero()); // still muted...
        CHECK(map.timelineAtFrame(24000) == Ticks::fromSeconds(10)); // ...and holding at the target
        CHECK(map.speedAtFrame(48000).isNormal()); // playback starts where it was scheduled
        CHECK(map.timelineAtFrame(48000 + 480) == Ticks::fromSeconds(10) + ms(10));
    }
    SECTION("prerolling into a new frame domain: the remaining preroll carries over") {
        TransportMap map(k48k);
        map.play(0);
        map.seek(Ticks::fromSeconds(10), 0, 48000);
        map.rebase(3, 24000, 0); // half the preroll was done on the old device
        CHECK(map.speedAtFrame(23999).isZero());
        CHECK(map.timelineAtFrame(23999) == Ticks::fromSeconds(10));
        CHECK(map.speedAtFrame(24000).isNormal());
        CHECK(map.timelineAtFrame(24000 + 480) == Ticks::fromSeconds(10) + ms(10));
        CHECK(map.isPlaying());
    }
    SECTION("before the history: holds at the first segment") {
        TransportMap map(k48k);
        map.play(0);
        map.seek(Ticks::fromSeconds(3), 1000);
        map.rebase(4, 500, 0); // frame 500 predates the seek's segment
        CHECK(map.timelineAtFrame(0) == Ticks::fromSeconds(3));
        CHECK(map.speedAtFrame(0).isZero());
        CHECK(map.speedAtFrame(500).isNormal()); // the seek's segment, 500 frames later as before
        CHECK(map.timelineAtFrame(500 + 480) == Ticks::fromSeconds(3) + ms(10));
    }
}
