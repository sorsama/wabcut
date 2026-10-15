#pragma once

// Exact, checked time conversions for importers.
//
// Interchange files express time as frame counts at NTSC rates, rational seconds ("1001/30000s"), or
// floating-point frame values (OTIO). Everything is converted to Ticks through the model's checked
// helpers in Time.h (128-bit mulDiv, framesToTicks with explicit Rounding); values from untrusted files
// that do not fit are reported, never wrapped.

#include <wabcut/model/Clip.h>
#include <wabcut/model/Parameters.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::detail {

struct MulDivResult {
    std::int64_t value = 0;
    /// The division had no remainder (nothing was rounded).
    bool exact = true;
};

/// a * b / c rounded to nearest (halves away from zero) with Time.h's 128-bit mulDiv, and whether it was
/// exact. nullopt when c == 0 or the result does not fit in int64.
std::optional<MulDivResult> mulDivNearest(std::int64_t a, std::int64_t b, std::int64_t c);

/// frames at `rate` -> ticks (nearest tick for rates that are not a whole number of ticks). nullopt on
/// overflow or an invalid rate.
std::optional<Ticks> framesToTicksChecked(std::int64_t frames, Rate rate);

/// Exact rational seconds num/den -> ticks (nearest). `exact` is false when a tick boundary was rounded.
std::optional<MulDivResult> secondsToTicks(std::int64_t num, std::int64_t den);

/// A floating-point frame value at a floating-point rate (OTIO RationalTime) -> ticks, nearest. Uses the
/// rational form of `rate` (see rateFromDouble) and exact integer math for integral values.
std::optional<MulDivResult> rationalTimeToTicks(double value, double rate);

/// The exact rate a floating-point rate stands for: whole numbers, the NTSC x/1001 family (23.976, 29.97,
/// 47.952, 59.94, 119.88, ...) and short decimals (12.5). nullopt for non-positive, non-finite or
/// unrecognisable values.
std::optional<Rate> rateFromDouble(double fps);

/// FCP7 <rate>: timebase 24 + ntsc TRUE -> 24000/1001, otherwise timebase/1.
std::optional<Rate> rateFromTimebase(std::int64_t timebase, bool ntsc);

/// "23.976", "29.97", "25", "59.94" — for messages.
std::string rateLabel(Rate rate);

/// A speed ratio num/den (both > 0) as the model's Speed, reduced; nullopt when it does not fit in 32 bits even
/// reduced (use speedFromDouble then).
std::optional<Speed> speedFromRatio(std::int64_t num, std::int64_t den);

struct ApproximateSpeed {
    Speed speed;
    bool exact = true; ///< the speed equals `value` (to 1e-9 relative)
};
/// The closest Speed to a positive floating-point ratio (continued fractions, denominators up to 100,000):
/// exact for decimal percentages such as 50, 33.333 or 250. nullopt for non-positive or non-finite values.
std::optional<ApproximateSpeed> speedFromDouble(double ratio);

/// "50%", "33.33%", "200%" — a speed for messages.
std::string speedLabel(Speed speed);

/// Parses an FCPXML time value: "0s", "5s", "1001/30000s", "3.5s" (also accepted without the 's').
/// Returns the value in ticks; `exact` is false when it had to be rounded to a tick.
std::optional<MulDivResult> parseFcpxTime(std::string_view text);

/// Smooth speed-ramp keys (X.1): the keys of `remap` whose `smooth` flag is set become Bezier keys whose speed runs
/// on through the key, with monotone tangents (Fritsch-Carlson: the harmonic mean of the two neighbouring speeds, 0
/// where the direction turns; one-sided at the ends; influence 1/3). Nothing changes, and false is returned, when no
/// key is smooth or the result would not be a valid remap (remapProblem). `smooth` has one flag per key.
bool smoothRemapKeys(Curve& remap, const std::vector<bool>& smooth);

} // namespace wabcut::interchange::detail
