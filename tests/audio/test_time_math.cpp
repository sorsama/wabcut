// The audio module's saturating wrappers over the model's checked time arithmetic (wabcut::mulDiv itself is
// tested in tests/model/test_time_checked.cpp).

#include <wabcut/audio/TimeMath.h>

#include <wabcut/model/Time.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

using wabcut::kTicksPerSecond;
using wabcut::Rounding;
using wabcut::Ticks;
using wabcut::audio::timemath::addSaturate;
using wabcut::audio::timemath::mulDivSaturate;

namespace {
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
} // namespace

TEST_CASE("mulDivSaturate rounds like wabcut::mulDiv where the result fits", "[audio][timemath]") {
    CHECK(mulDivSaturate(7, 1, 2, Rounding::Floor) == 3);
    CHECK(mulDivSaturate(-7, 1, 2, Rounding::Floor) == -4);
    CHECK(mulDivSaturate(-7, 1, 2, Rounding::Ceil) == -3);
    CHECK(mulDivSaturate(-7, 1, 2, Rounding::Nearest) == -4);
    CHECK(mulDivSaturate(7, 1, -2, Rounding::TowardZero) == -3);
    // 30 days of ns -> ticks needs the 128-bit intermediate.
    const std::int64_t thirtyDaysNs = 30LL * 86400 * 1'000'000'000;
    CHECK(mulDivSaturate(thirtyDaysNs, kTicksPerSecond, 1'000'000'000, Rounding::Floor) ==
          Ticks::fromSeconds(30LL * 86400).value);
    CHECK(mulDivSaturate(kMax, 2, 3, Rounding::Floor) == *wabcut::mulDiv(kMax, 2, 3, Rounding::Floor));
}

TEST_CASE("Saturating helpers clamp instead of failing", "[audio][timemath]") {
    CHECK(mulDivSaturate(kMax, 2, 1, Rounding::Floor) == kMax);
    CHECK(mulDivSaturate(kMax, -2, 1, Rounding::Floor) == kMin);
    CHECK(mulDivSaturate(kMin, -1, 1, Rounding::Floor) == kMax);
    CHECK(mulDivSaturate(5, 2, 0, Rounding::Floor) == 0);
    CHECK(addSaturate(kMax, 1) == kMax);
    CHECK(addSaturate(kMin, -1) == kMin);
    CHECK(addSaturate(40, 2) == 42);
    static_assert(mulDivSaturate(9, 1, 3, Rounding::Ceil) == 3); // usable in constant expressions
}
