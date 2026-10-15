// Exact time arithmetic used by every importer (src/interchange/src/TimeMath.h).

#include "TimeMath.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace wabcut;
using namespace wabcut::interchange::detail;

namespace {
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
} // namespace

// Time.h's checked mulDiv is tested with the model; these cover the importers' wrapper around it.
TEST_CASE("interchange: mulDivNearest rounds to nearest and says whether it was exact",
          "[interchange][time]") {
    const auto r = mulDivNearest(9'000'000'000LL, kTicksPerSecond, 24'000); // the product needs 72 bits
    REQUIRE(r);
    CHECK(r->value == 9'000'000'000LL / 24'000 * kTicksPerSecond);
    CHECK(r->exact);
    CHECK(mulDivNearest(7, 1, 2)->value == 4); // halves away from zero
    CHECK_FALSE(mulDivNearest(7, 1, 2)->exact);
    CHECK(mulDivNearest(-7, 1, 2)->value == -4);
    CHECK(mulDivNearest(5, 1, 3)->value == 2);
    CHECK_FALSE(mulDivNearest(5, 1, 3)->exact);
    CHECK(mulDivNearest(6, 1, -3)->value == -2);
    CHECK(mulDivNearest(6, 1, -3)->exact);
    CHECK_FALSE(mulDivNearest(std::int64_t{1} << 62, 6, 3)); // 2^63 does not fit
    CHECK(mulDivNearest(std::int64_t{1} << 62, -6, 3)->value == kMin);
    CHECK_FALSE(mulDivNearest(1, 1, 0));
}

TEST_CASE("interchange: frame counts become exact ticks at NTSC rates", "[interchange][time]") {
    CHECK(framesToTicksChecked(1, kRate29_97) == Ticks{8'475'667'200});
    CHECK(framesToTicksChecked(30, kRate23_976) == framesToTicks(30, kRate23_976));
    CHECK(framesToTicksChecked(-3, kRate25) == Ticks{-3 * 10'160'640'000LL});
    CHECK_FALSE(framesToTicksChecked(kMax / 2, kRate24));
    CHECK_FALSE(framesToTicksChecked(1, Rate{0, 1}));
}

TEST_CASE("interchange: rational seconds and OTIO RationalTime convert exactly", "[interchange][time]") {
    CHECK(secondsToTicks(1001, 30000)->value == 8'475'667'200);
    CHECK(secondsToTicks(1001, 30000)->exact);
    CHECK(rationalTimeToTicks(86400.0, 24.0)->value == 3600 * kTicksPerSecond);
    // 23.976 written as a double resolves to 24000/1001 exactly.
    CHECK(rationalTimeToTicks(48.0, 23.976023976023978)->value == framesToTicks(48, kRate23_976).value);
    CHECK(rationalTimeToTicks(48.0, 23.976)->value == framesToTicks(48, kRate23_976).value);
    // Sample-based audio times.
    CHECK(rationalTimeToTicks(672672.0, 48000.0)->value == framesToTicks(336, kRate23_976).value);
    // A fractional frame rounds to the nearest tick and says so.
    const auto half = rationalTimeToTicks(0.5, 25.0);
    REQUIRE(half);
    CHECK(half->value == kTicksPerSecond / 50);
    CHECK_FALSE(half->exact);
    CHECK_FALSE(rationalTimeToTicks(1.0, 0.0));
    CHECK_FALSE(rationalTimeToTicks(1e300, 24.0));
}

TEST_CASE("interchange: floating-point rates snap to the exact rate they stand for", "[interchange][time]") {
    CHECK(rateFromDouble(24.0) == Rate{24, 1});
    CHECK(rateFromDouble(23.976023976023978) == kRate23_976);
    CHECK(rateFromDouble(23.976) == kRate23_976);
    CHECK(rateFromDouble(29.97) == kRate29_97);
    CHECK(rateFromDouble(59.94005994005994) == kRate59_94);
    CHECK(rateFromDouble(119.88) == Rate{120000, 1001});
    CHECK(rateFromDouble(12.5) == Rate{25, 2});
    CHECK(rateFromDouble(48000.0) == kRate48kHz);
    CHECK_FALSE(rateFromDouble(0.0));
    CHECK_FALSE(rateFromDouble(-24.0));
    CHECK_FALSE(rateFromDouble(std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(rateFromDouble(3.14159265));
}

TEST_CASE("interchange: FCP7 timebase + ntsc flag give the exact rate", "[interchange][time]") {
    CHECK(rateFromTimebase(30, true) == kRate29_97);
    CHECK(rateFromTimebase(24, true) == kRate23_976);
    CHECK(rateFromTimebase(60, true) == kRate59_94);
    CHECK(rateFromTimebase(25, false) == kRate25);
    CHECK_FALSE(rateFromTimebase(0, false));
    CHECK_FALSE(rateFromTimebase(100'000, false));
    CHECK(rateLabel(kRate29_97) == "29.97");
    CHECK(rateLabel(kRate23_976) == "23.976");
    CHECK(rateLabel(kRate25) == "25");
}

TEST_CASE("interchange: FCPXML time strings parse to exact ticks", "[interchange][time]") {
    CHECK(parseFcpxTime("0s")->value == 0);
    CHECK(parseFcpxTime("3600s")->value == 3600 * kTicksPerSecond);
    CHECK(parseFcpxTime("1001/30000s")->value == 8'475'667'200);
    CHECK(parseFcpxTime("86486400/24000s")->value == framesToTicks(86400, kRate23_976).value);
    CHECK(parseFcpxTime("3.5s")->value == 7 * kTicksPerSecond / 2);
    CHECK(parseFcpxTime("-1/2s")->value == -kTicksPerSecond / 2);
    CHECK(parseFcpxTime(" 10s ")->value == 10 * kTicksPerSecond);
    CHECK_FALSE(parseFcpxTime(""));
    CHECK_FALSE(parseFcpxTime("s"));
    CHECK_FALSE(parseFcpxTime("1/0s"));
    CHECK_FALSE(parseFcpxTime("1/-5s"));
    CHECK_FALSE(parseFcpxTime("abc"));
    CHECK_FALSE(parseFcpxTime("1/s"));
    CHECK_FALSE(parseFcpxTime("99999999999999999999s"));
    CHECK_FALSE(parseFcpxTime("1.s"));
}
