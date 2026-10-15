// A.5 audio sync (wabcut/audio/edit/AudioSync.h) on synthetic recordings of one event with known offsets: within one
// sample at 48 kHz through noise, a room, gain and partial overlap; the drift of a skewed clock in ppm; failure on
// unrelated audio and on too little overlap; chaining through another recording; cancel.

#include "SyncTestSignals.h"

#include <wabcut/audio/edit/AudioSync.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr int kRate = 48000;

std::int64_t samples(double seconds) { return std::llround(seconds * kRate); }

SyncReport sync(std::vector<RecordingSignal*> recordings, std::size_t reference = 0, SyncOptions options = {}) {
    std::vector<SyncSignal*> signals(recordings.begin(), recordings.end());
    auto r = syncSignals(signals, reference, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    return *r;
}

} // namespace

TEST_CASE("Audio sync finds known offsets to the sample through noise, a room and gain", "[audio][edit][sync]") {
    const SyntheticEvent event(90.0, 7);
    struct Case {
        const char* name;
        Recording b;
    };
    const Case cases[] = {
        {"clean, later", {12.345678, 60.0}},
        {"earlier (negative offset)", {-7.25, 70.0}},
        {"quiet, noisy, in a room", {3.000021, 60.0, 0.2, 0.02, true}},
        {"loud and noisy", {21.5, 50.0, 3.0, 0.15}},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        RecordingSignal a(event, {0.0, 80.0});
        RecordingSignal b(event, c.b);
        const SyncReport r = sync({&a, &b});
        REQUIRE(r.matches.size() == 2);
        CHECK(r.matches[0].synced);
        CHECK(r.matches[0].offset == 0);
        const SyncMatch& m = r.matches[1];
        INFO(m.problem << " psr " << m.peakToSidelobe << " confidence " << m.confidence);
        REQUIRE(m.synced);
        CHECK(std::llabs(m.offset - samples(c.b.start)) <= 1);
        CHECK(m.confidence >= 0.5);
        CHECK(m.windows >= 3);
        CHECK(std::fabs(m.driftPpm) < 2.0);
        CHECK_FALSE(m.driftSignificant);
        CHECK(m.speedProposal == 1.0);
    }
}

TEST_CASE("Audio sync: a partial overlap is enough; too little or unrelated audio fails with a reason",
          "[audio][edit][sync]") {
    const SyntheticEvent event(90.0, 11);
    SECTION("ten seconds in common") {
        RecordingSignal a(event, {0.0, 40.0});
        RecordingSignal b(event, {30.0, 30.0, 0.7, 0.01});
        const SyncReport r = sync({&a, &b});
        REQUIRE(r.matches[1].synced);
        CHECK(std::llabs(r.matches[1].offset - samples(30.0)) <= 1);
        CHECK(r.matches[1].overlapSeconds == Approx(10.0).margin(0.1));
    }
    SECTION("three seconds in common, five required") {
        RecordingSignal a(event, {0.0, 40.0});
        RecordingSignal b(event, {37.0, 30.0});
        const SyncReport r = sync({&a, &b});
        CHECK_FALSE(r.matches[1].synced);
        CHECK_FALSE(r.matches[1].problem.empty());
    }
    SECTION("another event") {
        const SyntheticEvent other(90.0, 12);
        RecordingSignal a(event, {0.0, 60.0});
        RecordingSignal b(other, {5.0, 60.0});
        const SyncReport r = sync({&a, &b});
        CHECK_FALSE(r.matches[1].synced);
        CHECK(r.matches[1].confidence < 0.5);
        CHECK_FALSE(r.matches[1].problem.empty());
    }
    SECTION("silence") {
        RecordingSignal a(event, {0.0, 60.0});
        RecordingSignal b(event, {200.0, 60.0}); // after the event: nothing but zeros
        const SyncReport r = sync({&a, &b});
        CHECK_FALSE(r.matches[1].synced);
    }
}

TEST_CASE("Audio sync measures the clock drift of a skewed recorder and proposes the speed that cancels it",
          "[audio][edit][sync][drift]") {
    const SyntheticEvent event(400.0, 21);
    RecordingSignal a(event, {0.0, 360.0});
    RecordingSignal b(event, {10.0, 300.0, 1.0, 0.01, false, 40.0}); // fast by 40 ppm: 12 ms over the overlap
    const SyncReport r = sync({&a, &b});
    const SyncMatch& m = r.matches[1];
    INFO(m.problem << " drift " << m.driftPpm << " residual " << m.driftResidualMs);
    REQUIRE(m.synced);
    CHECK(m.driftPpm == Approx(40.0).margin(2.0));
    CHECK(m.driftSignificant);
    CHECK(m.speedProposal == Approx(1.0 + 40e-6).margin(2e-6));
    // The offset at the overlap's middle: the event at b's 150 s is at a's 10 + 150 / (1 + 40e-6) s.
    const double middleB = 150.0;
    const double expected = (10.0 + middleB / (1.0 + 40e-6) - middleB) * kRate;
    CHECK(std::fabs(static_cast<double>(m.offset) - expected) <= 2.0);
    CHECK(m.offsetAtStart != m.offset);
    // The reference against itself drifts nothing.
    RecordingSignal c(event, {20.0, 300.0});
    CHECK(std::fabs(sync({&a, &c}).matches[1].driftPpm) < 1.0);
}

TEST_CASE("Audio sync: several cameras, one synced through another; a stop cancels", "[audio][edit][sync]") {
    const SyntheticEvent event(200.0, 31);
    RecordingSignal ref(event, {0.0, 60.0});
    RecordingSignal cam2(event, {40.0, 60.0, 0.5, 0.01, true});
    RecordingSignal cam3(event, {90.0, 60.0, 2.0, 0.01}); // no overlap with the reference, 10 s with cam2
    RecordingSignal cam4(event, {-20.0, 40.0});
    const SyncReport r = sync({&ref, &cam2, &cam3, &cam4});
    REQUIRE(r.matches.size() == 4);
    REQUIRE(r.matches[1].synced);
    REQUIRE(r.matches[2].synced);
    REQUIRE(r.matches[3].synced);
    CHECK(std::llabs(r.matches[1].offset - samples(40.0)) <= 1);
    CHECK(std::llabs(r.matches[2].offset - samples(90.0)) <= 2);
    CHECK(r.matches[2].relativeTo == 1);
    CHECK(std::llabs(r.matches[3].offset - samples(-20.0)) <= 1);
    // Without chaining the third camera does not sync.
    SyncOptions single;
    single.chain = false;
    CHECK_FALSE(sync({&ref, &cam2, &cam3}, 0, single).matches[2].synced);
    // Another reference: the offsets are relative to it.
    const SyncReport r2 = sync({&ref, &cam2}, 1);
    CHECK(std::llabs(r2.matches[0].offset - samples(-40.0)) <= 1);

    std::stop_source stop;
    stop.request_stop();
    std::vector<SyncSignal*> signals{&ref, &cam2};
    auto cancelled = syncSignals(signals, 0, {}, stop.get_token());
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
    CHECK_FALSE(syncSignals(std::span<SyncSignal* const>(signals.data(), 1), 0));
}

TEST_CASE("Audio sync speed: four synthetic hour-long angles (the algorithm without decoding)", "[audio][edit][sync][.benchmark]") {
    const SyntheticEvent event(3700.0, 41);
    RecordingSignal a(event, {0.0, 3600.0});
    RecordingSignal b(event, {12.5, 3600.0, 0.6, 0.01});
    RecordingSignal c(event, {-30.25, 3600.0, 1.5, 0.01, true});
    RecordingSignal d(event, {61.0, 3600.0, 0.8, 0.02});
    const SyncReport r = sync({&a, &b, &c, &d});
    std::printf("audio sync: four 60-minute synthetic angles in %.2f s\n", r.seconds);
    CHECK(std::llabs(r.matches[1].offset - samples(12.5)) <= 1);
    CHECK(std::llabs(r.matches[2].offset - samples(-30.25)) <= 1);
    CHECK(std::llabs(r.matches[3].offset - samples(61.0)) <= 1);
}
