#include <wabcut/model/Time.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;

TEST_CASE("Common video rates have a whole number of ticks per frame", "[model][time]") {
    for (Rate r : {kRate23_976, kRate24, kRate25, kRate29_97, kRate30, kRate50, kRate59_94, kRate60}) {
        CHECK(r.isExact());
    }
    CHECK(kRate24.ticksPerUnit() == 10'584'000'000LL);
    CHECK(kRate29_97.ticksPerUnit() == 8'475'667'200LL);
}

TEST_CASE("Common audio rates have a whole number of ticks per sample", "[model][time]") {
    CHECK(kRate48kHz.ticksPerUnit() == 5'292'000LL);
    CHECK(kRate44_1kHz.ticksPerUnit() == 5'760'000LL);
}

TEST_CASE("Frame <-> tick conversion round-trips exactly", "[model][time]") {
    for (Rate r : {kRate23_976, kRate29_97, kRate59_94, kRate25}) {
        for (std::int64_t f : {0LL, 1LL, 1799LL, 107892LL, 5'000'000LL}) {
            CHECK(ticksToFrames(framesToTicks(f, r), r) == f);
        }
    }
}

TEST_CASE("Tick positions floor to the containing frame, also before zero", "[model][time]") {
    const Ticks oneFrame = framesToTicks(1, kRate30);
    CHECK(ticksToFrames(oneFrame - Ticks{1}, kRate30) == 0);
    CHECK(ticksToFrames(Ticks{-1}, kRate30) == -1);
    CHECK(snapToFrame(oneFrame + Ticks{5}, kRate30) == oneFrame);
}

TEST_CASE("TimeRange is half-open", "[model][time]") {
    const TimeRange r{Ticks::fromSeconds(1), Ticks::fromSeconds(2)};
    CHECK(r.contains(Ticks::fromSeconds(1)));
    CHECK_FALSE(r.contains(Ticks::fromSeconds(3)));
    CHECK(r.overlaps({Ticks::fromSeconds(2), Ticks::fromSeconds(5)}));
    CHECK_FALSE(r.overlaps({Ticks::fromSeconds(3), Ticks::fromSeconds(1)}));
}
