#include <wabcut/media/StreamTime.h>

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <random>

using namespace wabcut;
using namespace wabcut::media;

TEST_CASE("StreamTime: large time-base numerators do not overflow (review P0 repro)", "[media][time]") {
    // num * kTicksPerSecond overflows int64 here; the old conversion returned INT64_MIN.
    const auto t = streamToTicks(25, TimeBase{40'000'001, 1'000'000'000});
    REQUIRE(t.has_value());
    CHECK(*t == Ticks{254'016'006'350}); // 1.000000025 s, rounded to nearest
    const auto back = ticksToStream(*t, TimeBase{40'000'001, 1'000'000'000});
    REQUIRE(back.has_value());
    CHECK(*back == 25);
}

TEST_CASE("StreamTime: common container time bases are exact", "[media][time]") {
    CHECK(streamToTicks(256, TimeBase{1, 15360}) == framesToTicks(1, kRate60));     // MP4 at 60 fps
    CHECK(streamToTicks(3003, TimeBase{1, 90000}) == framesToTicks(1, kRate29_97)); // MPEG-TS
    CHECK(streamToTicks(1, TimeBase{1001, 30000}) == framesToTicks(1, kRate29_97)); // frame clock
    CHECK(streamToTicks(1, TimeBase{1, 48000}) == samplesToTicks(1, kRate48kHz));
    CHECK(streamToTicks(40, TimeBase{1, 1000}) == framesToTicks(1, kRate25));             // Matroska (ms)
    CHECK(streamToTicks(-1024, TimeBase{1, 48000}) == samplesToTicks(-1024, kRate48kHz)); // AAC priming
    CHECK(streamToTicks(1'000'000'000'000'000LL, TimeBase{1, 1'000'000'000}) ==
          Ticks::fromSeconds(1'000'000));
}

TEST_CASE("StreamTime: rounding", "[media][time]") {
    // One unit of 1/11 s is 23092363636.36... ticks: nearest going in, floor going out.
    CHECK(streamToTicks(1, TimeBase{1, 11}) == Ticks{23'092'363'636});
    CHECK(streamToTicks(2, TimeBase{1, 11}) == Ticks{46'184'727'273});
    CHECK(ticksToStream(Ticks{1}, TimeBase{1, 48000}) == 0);
    CHECK(ticksToStream(Ticks{-1}, TimeBase{1, 48000}) == -1); // the unit containing t
    CHECK(ticksToStream(samplesToTicks(7, kRate48kHz) - Ticks{1}, TimeBase{1, 48000}) == 6);
}

TEST_CASE("StreamTime: overflow and invalid time bases are reported, never wrapped", "[media][time]") {
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    CHECK_FALSE(streamToTicks(kMax, TimeBase{1, 1}).has_value());
    CHECK_FALSE(streamToTicks(kMin, TimeBase{1, 1}).has_value());
    CHECK_FALSE(streamToTicks(1, TimeBase{kMax, 1}).has_value());
    CHECK_FALSE(streamToTicks(1, TimeBase{0, 1}).has_value());
    CHECK_FALSE(streamToTicks(1, TimeBase{1, 0}).has_value());
    CHECK_FALSE(streamToTicks(1, TimeBase{-1, 25}).has_value());
    CHECK_FALSE(ticksToStream(Ticks::max(), TimeBase{1, kMax}).has_value());
    CHECK_FALSE(ticksToStream(Ticks{1}, TimeBase{0, 1}).has_value());
    // The largest representable values still convert.
    CHECK(streamToTicks(kMax / kTicksPerSecond, TimeBase{1, 1}).has_value());
    CHECK(ticksToStream(Ticks::max(), TimeBase{1, 1}) == kMax / kTicksPerSecond);
}

TEST_CASE("StreamTime: stream units survive a round trip", "[media][time]") {
    std::mt19937_64 rng(11);
    for (const TimeBase base : {TimeBase{1, 15360}, TimeBase{1, 90000}, TimeBase{1001, 24000},
                                TimeBase{1, 1000}, TimeBase{1, 44100}, TimeBase{40'000'001, 1'000'000'000}}) {
        const std::int64_t limit = 1'000'000 * base.den / base.num; // +-10^6 s, well inside the ticks range
        std::uniform_int_distribution<std::int64_t> pts(-limit, limit);
        for (int i = 0; i < 200; ++i) {
            const std::int64_t p = pts(rng);
            const auto t = streamToTicks(p, base);
            REQUIRE(t.has_value());
            CHECK(ticksToStream(*t, base) == p);
        }
    }
}
