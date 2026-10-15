// Checked time arithmetic (contract rev 2, principle 1): overflow-safe helpers, exact rescaling between
// ticks and stream time bases, normalized rates and rational time maps.

#include <wabcut/model/Clip.h>
#include <wabcut/model/Time.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>

using namespace wabcut;

namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

} // namespace

TEST_CASE("checkedAdd, checkedSub, checkedNeg and checkedMul stop at the int64 edges", "[model][time][checked]") {
    CHECK(checkedAdd(kMax, std::int64_t{0}) == kMax);
    CHECK_FALSE(checkedAdd(kMax, std::int64_t{1}));
    CHECK_FALSE(checkedAdd(kMin, std::int64_t{-1}));
    CHECK(checkedAdd(kMin, kMax) == -1);

    CHECK_FALSE(checkedSub(kMin, std::int64_t{1}));
    CHECK_FALSE(checkedSub(std::int64_t{0}, kMin)); // -INT64_MIN does not exist
    CHECK(checkedSub(std::int64_t{-1}, kMin) == kMax);
    CHECK(checkedSub(kMax, kMax) == 0);

    CHECK_FALSE(checkedNeg(kMin));
    CHECK(checkedNeg(kMax) == -kMax);
    CHECK(checkedNeg(Ticks{5}) == Ticks{-5});

    CHECK(checkedMul(kMin, std::int64_t{1}) == kMin);
    CHECK_FALSE(checkedMul(kMin, std::int64_t{-1}));
    CHECK(checkedMul(std::int64_t{-1}, kMax) == -kMax);
    CHECK(checkedMul(std::int64_t{1} << 31, std::int64_t{1} << 31) == std::int64_t{1} << 62);
    CHECK_FALSE(checkedMul(std::int64_t{1} << 32, std::int64_t{1} << 31));

    CHECK(checkedAdd(Ticks{1}, Ticks{2}) == Ticks{3});
    CHECK_FALSE(checkedAdd(Ticks::max(), Ticks{1}));
    CHECK_FALSE(checkedSub(Ticks::min(), Ticks{1}));
}

TEST_CASE("mulDiv rounds as asked for every combination of signs", "[model][time][checked]") {
    struct Case {
        std::int64_t value, mul, div;
        std::int64_t floor, ceil, nearest, towardZero;
    };
    const Case cases[] = {
        {7, 1, 2, 3, 4, 4, 3},       // 3.5
        {-7, 1, 2, -4, -3, -4, -3},  // -3.5: nearest goes away from zero
        {7, -1, 2, -4, -3, -4, -3},
        {7, 1, -2, -4, -3, -4, -3},
        {-7, -1, -2, -4, -3, -4, -3},
        {-7, -1, 2, 3, 4, 4, 3},
        {5, 1, 3, 1, 2, 2, 1},       // 1.67
        {4, 1, 3, 1, 2, 1, 1},       // 1.33
        {-4, 1, 3, -2, -1, -1, -1},
        {6, 1, 3, 2, 2, 2, 2},       // exact
        {0, 5, 7, 0, 0, 0, 0},
    };
    for (const Case& c : cases) {
        INFO(c.value << " * " << c.mul << " / " << c.div);
        CHECK(mulDiv(c.value, c.mul, c.div, Rounding::Floor) == c.floor);
        CHECK(mulDiv(c.value, c.mul, c.div, Rounding::Ceil) == c.ceil);
        CHECK(mulDiv(c.value, c.mul, c.div, Rounding::Nearest) == c.nearest);
        CHECK(mulDiv(c.value, c.mul, c.div, Rounding::TowardZero) == c.towardZero);
    }
    CHECK_FALSE(mulDiv(std::int64_t{1}, 1, 0, Rounding::Floor));
}

TEST_CASE("mulDiv keeps a 128-bit intermediate and reports results that do not fit", "[model][time][checked]") {
    CHECK(mulDiv(kMax, kMax, kMax, Rounding::Floor) == kMax);
    CHECK(mulDiv(kMin, kMax, kMax, Rounding::Floor) == kMin);
    CHECK(mulDiv(kMin, -1, 2, Rounding::Floor) == std::int64_t{1} << 62);
    CHECK_FALSE(mulDiv(kMin, -1, 1, Rounding::Floor)); // 2^63
    CHECK_FALSE(mulDiv(kMax, 2, 1, Rounding::Floor));
    CHECK(mulDiv(kMax, 2, 2, Rounding::Floor) == kMax);
    // Rounding up past the edge is an overflow too.
    CHECK(mulDiv(kMax, 1, 1, Rounding::Ceil) == kMax);
    CHECK_FALSE(mulDiv(kMax, 3, 2, Rounding::Ceil));

    // 30 days of ticks at 192 kHz: the naive product overflows int64, mulDiv does not.
    const std::int64_t thirtyDays = 30LL * 86'400;
    CHECK(mulDiv(Ticks::fromSeconds(thirtyDays).value, 192'000, kTicksPerSecond, Rounding::Floor) ==
          thirtyDays * 192'000);
}

TEST_CASE("The compile-time and run-time 128-bit paths agree", "[model][time][checked]") {
    using Sample = std::tuple<std::int64_t, std::int64_t, std::int64_t>;
    static constexpr std::array<Sample, 8> kSamples{{
        {kMax, kMax, kMax},
        {kMin, kMax, 3},
        {kTicksPerSecond * 86'400 * 400, 48'000, kTicksPerSecond},
        {-123'456'789'012'345, 987'654'321, 1'000'003},
        {0x7FFF'FFFF'FFFF, 0x7FFF'FFFF, 0xFFFF},
        {-1, kMin, kMax},
        {kMax / 3, 7, 11},
        {1, 1, 3},
    }};
    // The constexpr evaluation takes the portable path; the loop below takes the intrinsic one.
    static constexpr auto kCompileTime = [] {
        std::array<std::optional<std::int64_t>, kSamples.size() * 4> out{};
        std::size_t i = 0;
        for (const auto& [v, m, d] : kSamples) {
            for (const Rounding r : {Rounding::Floor, Rounding::Ceil, Rounding::Nearest, Rounding::TowardZero}) {
                out[i++] = mulDiv(v, m, d, r);
            }
        }
        return out;
    }();
    std::size_t i = 0;
    for (const auto& [v, m, d] : kSamples) {
        for (const Rounding r : {Rounding::Floor, Rounding::Ceil, Rounding::Nearest, Rounding::TowardZero}) {
            volatile std::int64_t value = v; // keep the optimizer from folding the run-time call
            INFO("sample " << i);
            CHECK(mulDiv(value, m, d, r) == kCompileTime[i++]);
        }
    }
}

TEST_CASE("Rates are validated, normalized and compared by value", "[model][time][rate]") {
    CHECK(Rate{60, 2} == kRate30);
    CHECK(Rate{60, 2}.normalized().num == 30);
    CHECK(Rate{60, 2}.normalized().den == 1);
    CHECK_FALSE(Rate{60, 2}.isNormalized());
    CHECK(kRate29_97.isNormalized());
    CHECK(Rate::make(60'000, 2002) == kRate29_97);
    CHECK_FALSE(Rate::make(0, 1));
    CHECK_FALSE(Rate::make(30, 0));
    CHECK_FALSE(Rate::make(-30, 1));
    CHECK_FALSE(Rate::make(5'000'000'000, 1)); // does not fit int32 even reduced
    CHECK(Rate::make(5'000'000'000, 2'000'000'000) == Rate{5, 2});

    CHECK_FALSE(Rate{0, 1}.isValid());
    CHECK_FALSE(Rate{0, 1}.isExact());
    CHECK_FALSE(Rate{13, 1}.isExact());              // 1/13 s is not a whole number of ticks
    CHECK(Rate{1, 2'000'000'000}.isExact());         // used to overflow the check
    CHECK(Rate{96'000, 2}.isExact());
    CHECK(Rate{96'000, 2}.ticksPerUnit() == Rate{48'000, 1}.ticksPerUnit());
    CHECK(kRate48kHz.ticksPerUnit() == 5'292'000);
}

TEST_CASE("rescale converts exactly between ticks and stream time bases", "[model][time][rescale]") {
    const TimeBase mpegTs{1, 90'000};
    const TimeBase ntscFrames{1001, 30'000};
    CHECK(rescale(Ticks{kTicksPerSecond}, mpegTs, Rounding::Floor) == 90'000);
    CHECK(rescale(std::int64_t{90'000}, mpegTs, Rounding::Floor) == Ticks{kTicksPerSecond});
    CHECK(timeBaseOf(kRate29_97) == ntscFrames);

    // NTSC frames through MPEG-TS PTS and back, also well past 24 hours.
    for (const std::int64_t frame : {0LL, 1LL, 1799LL, 107'892LL, 2'589'408LL, 10'000'000LL}) {
        INFO("frame " << frame);
        const Ticks t = framesToTicks(frame, kRate29_97);
        const auto pts = rescale(t, mpegTs, Rounding::Floor);
        REQUIRE(pts);
        CHECK(*pts == frame * 3003);
        CHECK(rescale(*pts, mpegTs, Rounding::Floor) == t);
        CHECK(rescale(t, ntscFrames, Rounding::Floor) == frame);
        CHECK(rescale(frame, ntscFrames, Rounding::Floor) == t);
    }

    // A base that is not a whole number of ticks: the rounding mode decides.
    const TimeBase eleventh{1, 11};
    const auto down = rescale(std::int64_t{1}, eleventh, Rounding::Floor);
    const auto up = rescale(std::int64_t{1}, eleventh, Rounding::Ceil);
    REQUIRE(down);
    REQUIRE(up);
    CHECK(up->value == down->value + 1);
    CHECK(rescale(std::int64_t{11}, eleventh, Rounding::Floor) == Ticks{kTicksPerSecond});

    // 30 days of 48 kHz samples.
    const std::int64_t samples = 30LL * 86'400 * 48'000;
    const auto ticks = rescale(samples, TimeBase{1, 48'000}, Rounding::Floor);
    REQUIRE(ticks);
    CHECK(rescale(*ticks, TimeBase{1, 48'000}, Rounding::Floor) == samples);

    CHECK_FALSE(rescale(std::int64_t{1}, TimeBase{0, 1}, Rounding::Floor));
    CHECK_FALSE(rescale(Ticks{1}, TimeBase{1, 0}, Rounding::Floor));
    CHECK_FALSE(rescale(kMax, TimeBase{1, 1}, Rounding::Floor)); // kMax seconds in ticks
    CHECK_FALSE(rescale(std::int64_t{1}, TimeBase{kMax, 1}, Rounding::Floor));
}

TEST_CASE("Checked frame conversions also work for rates that are not exact in ticks", "[model][time][rescale]") {
    const Rate odd{13, 1};
    CHECK(framesToTicks(13, odd, Rounding::Floor) == Ticks{kTicksPerSecond});
    CHECK(framesToTicks(1, odd, Rounding::Floor) != framesToTicks(1, odd, Rounding::Ceil));
    CHECK(ticksToFrames(Ticks{kTicksPerSecond}, odd, Rounding::Floor) == 13);
    for (const std::int64_t frame : {0LL, 7LL, 107'892LL}) {
        CHECK(framesToTicks(frame, kRate29_97, Rounding::Floor) == framesToTicks(frame, kRate29_97));
    }
    CHECK(ticksToFrames(framesToTicks(5, kRate29_97) - Ticks{1}, kRate29_97, Rounding::Floor) == 4);
    CHECK(ticksToFrames(framesToTicks(5, kRate29_97) - Ticks{1}, kRate29_97, Rounding::Ceil) == 5);
    CHECK_FALSE(framesToTicks(1, Rate{0, 1}, Rounding::Floor));
}

TEST_CASE("TimeMap maps timeline time to source time and back", "[model][time][timemap]") {
    SECTION("normal speed with offsets") {
        const TimeMap m{Ticks{1000}, Ticks{5000}, 1, 1};
        CHECK(m.map(Ticks{1000}) == Ticks{5000});
        CHECK(m.map(Ticks{1010}) == Ticks{5010});
        CHECK(m.inverse(Ticks{5010}) == Ticks{1010});
        CHECK(m.isIdentitySpeed());
    }
    SECTION("twice as fast: map then inverse is exact") {
        const TimeMap m{Ticks{0}, Ticks{0}, 2, 1};
        for (const std::int64_t t : {0LL, 1LL, 7LL, 1'000'001LL, -5LL}) {
            CHECK(m.inverse(*m.map(Ticks{t})) == Ticks{t});
        }
        CHECK(m.inverse(Ticks{7}) == Ticks{3});                     // floor
        CHECK(m.inverse(Ticks{7}, Rounding::Ceil) == Ticks{4});
    }
    SECTION("half speed: inverse then map is exact") {
        const TimeMap m{Ticks{0}, Ticks{0}, 1, 2};
        for (const std::int64_t s : {0LL, 1LL, 7LL, 1'000'001LL, -5LL}) {
            CHECK(m.map(*m.inverse(Ticks{s})) == Ticks{s});
        }
        CHECK(m.map(Ticks{3}) == Ticks{1});
        CHECK(m.map(Ticks{3}, Rounding::Ceil) == Ticks{2});
    }
    SECTION("a rational speed round-trips on multiples of its denominator") {
        const TimeMap m{Ticks{10}, Ticks{20}, 3, 2};
        for (std::int64_t k = -5; k <= 5; ++k) {
            const Ticks t{10 + 2 * k};
            CHECK(m.inverse(*m.map(t)) == t);
        }
    }
    SECTION("reverse") {
        const TimeMap m{Ticks{100}, Ticks{1000}, -1, 1};
        CHECK(m.isReverse());
        CHECK(m.map(Ticks{110}) == Ticks{990});
        CHECK(m.inverse(Ticks{990}) == Ticks{110});
        CHECK(m.mapDuration(Ticks{10}) == Ticks{-10});
    }
    SECTION("freeze frame") {
        const TimeMap m{Ticks{0}, Ticks{500}, 0, 1};
        CHECK(m.isFreeze());
        CHECK(m.map(Ticks{0}) == Ticks{500});
        CHECK(m.map(Ticks{123'456}) == Ticks{500});
        CHECK_FALSE(m.inverse(Ticks{500}));
    }
    SECTION("long timelines and the int64 edges") {
        const Ticks twoDays = Ticks::fromSeconds(2 * 86'400);
        const TimeMap slow{Ticks{}, Ticks{}, 1000, 1001};
        CHECK(slow.map(twoDays) == mulDiv(twoDays, 1000, 1001, Rounding::Floor));
        CHECK_FALSE(TimeMap{Ticks::min(), Ticks{}, 1, 1}.map(Ticks::max()));
        CHECK_FALSE(TimeMap{Ticks{}, Ticks::max(), 1, 1}.map(Ticks{1}));
        CHECK_FALSE(TimeMap{Ticks{}, Ticks{}, 1, 0}.map(Ticks{1})); // invalid
    }
    SECTION("a clip's map matches its source time") {
        Clip c;
        c.timelineRange = {Ticks{1000}, Ticks{500}};
        c.sourceIn = Ticks{40};
        c.speed = {2, 1};
        CHECK(c.timeMap().map(Ticks{1100}) == c.sourceTimeAt(Ticks{1100}));
    }
}
