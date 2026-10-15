#pragma once

// Wabcut time model — the one contract every module shares.
//
// All timeline positions and durations are integer ticks at kTicksPerSecond (Premiere Pro's tick
// rate). It divides evenly by every common video rate (23.976 … 60, incl. NTSC 1001 rates) and
// audio rate (22.05/32/44.1/48/88.2/96/192 kHz), so conversions are exact and .prproj import is
// lossless. Never store floating-point time in the model.
//
// Overflow rules (contract rev 2, principle 1): plain `Ticks` arithmetic is for values known to be in
// range (timeline positions, a few days at most). Anything that scales time — rates, stream time
// bases, speeds, imported values — goes through the checked helpers below. They use a 128-bit
// intermediate, take an explicit Rounding, and return std::nullopt instead of overflowing. Never
// write `ticks * rate / kTicksPerSecond` by hand.
//
// Quick guide:
//   checkedAdd / checkedSub / checkedMul / checkedNeg   int64 or Ticks arithmetic that may overflow
//   mulDiv(v, mul, div, rounding)                      v * mul / div, exact, 128-bit intermediate
//   Rate                                               nominal frame/sample rate (positive ratio)
//   TimeBase                                           stream time base: one unit = num/den seconds
//   rescale(units, base, r) / rescale(ticks, base, r)  stream units <-> ticks (FFmpeg boundary)
//   framesToTicks(f, rate, r) / ticksToFrames(t, rate, r)  checked forms, also for non-exact rates
//   TimeMap                                            timeline -> source mapping with rational speed

#include <compare>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
#include <intrin.h>
#endif

namespace wabcut {

inline constexpr std::int64_t kTicksPerSecond = 254'016'000'000LL;

/// A point or a duration on a timeline, in ticks.
struct Ticks {
    std::int64_t value = 0;

    constexpr auto operator<=>(const Ticks&) const = default;
    constexpr Ticks operator+(Ticks o) const { return {value + o.value}; }
    constexpr Ticks operator-(Ticks o) const { return {value - o.value}; }
    constexpr Ticks operator-() const { return {-value}; }
    constexpr Ticks& operator+=(Ticks o) {
        value += o.value;
        return *this;
    }
    constexpr Ticks& operator-=(Ticks o) {
        value -= o.value;
        return *this;
    }

    static constexpr Ticks zero() { return {0}; }
    static constexpr Ticks fromSeconds(std::int64_t s) { return {s * kTicksPerSecond}; }
    static constexpr Ticks min() { return {std::numeric_limits<std::int64_t>::min()}; }
    static constexpr Ticks max() { return {std::numeric_limits<std::int64_t>::max()}; }
    /// Lossy; for UI display and logging only.
    constexpr double toSeconds() const { return static_cast<double>(value) / static_cast<double>(kTicksPerSecond); }
};

// --- Checked arithmetic -------------------------------------------------------------------------------

/// How a non-integral result is turned into an integer.
enum class Rounding {
    Floor,      ///< toward negative infinity (frame containing a time)
    Ceil,       ///< toward positive infinity
    Nearest,    ///< to the nearest integer; exact halves go away from zero (FFmpeg's AV_ROUND_NEAR_INF)
    TowardZero, ///< truncation (C++ integer division)
};

namespace detail {

struct U128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
};

#if defined(__SIZEOF_INT128__)
// GCC's and Clang's 128-bit integer is an extension: `__extension__` keeps -Wpedantic (an error in the presets) quiet
// about it (I.7 portability audit). MSVC uses its intrinsics below instead.
__extension__ typedef unsigned __int128 NativeU128;
#endif

constexpr std::uint64_t magnitude(std::int64_t v) {
    return v < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
}

/// Full 64x64 -> 128-bit product. Portable code at compile time and on targets without a fast path.
constexpr U128 mul64(std::uint64_t a, std::uint64_t b) {
    if (!std::is_constant_evaluated()) {
#if defined(__SIZEOF_INT128__)
        const NativeU128 p = static_cast<NativeU128>(a) * b;
        return {static_cast<std::uint64_t>(p >> 64), static_cast<std::uint64_t>(p)};
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
        std::uint64_t hi = 0;
        const std::uint64_t lo = _umul128(a, b, &hi);
        return {hi, lo};
#endif
    }
    const std::uint64_t aLo = a & 0xFFFF'FFFFu;
    const std::uint64_t aHi = a >> 32;
    const std::uint64_t bLo = b & 0xFFFF'FFFFu;
    const std::uint64_t bHi = b >> 32;
    const std::uint64_t p0 = aLo * bLo;
    const std::uint64_t p1 = aLo * bHi;
    const std::uint64_t p2 = aHi * bLo;
    const std::uint64_t p3 = aHi * bHi;
    const std::uint64_t mid = (p0 >> 32) + (p1 & 0xFFFF'FFFFu) + (p2 & 0xFFFF'FFFFu);
    return {p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32), (mid << 32) | (p0 & 0xFFFF'FFFFu)};
}

/// (n.hi:n.lo) / d for n.hi < d (so the quotient fits 64 bits): {quotient, remainder}.
constexpr std::pair<std::uint64_t, std::uint64_t> div128(U128 n, std::uint64_t d) {
    if (!std::is_constant_evaluated()) {
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
        // A 128-bit division is a runtime call (__udivti3) that libgcc / compiler-rt provide; with clang-cl the MSVC
        // runtime does not, so clang-cl takes the portable loop below.
        const NativeU128 v = (static_cast<NativeU128>(n.hi) << 64) | n.lo;
        return {static_cast<std::uint64_t>(v / d), static_cast<std::uint64_t>(v % d)};
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
        std::uint64_t remainder = 0;
        const std::uint64_t quotient = _udiv128(n.hi, n.lo, d, &remainder);
        return {quotient, remainder};
#endif
    }
    // Restoring shift-subtract division, one quotient bit per step.
    std::uint64_t rem = n.hi;
    std::uint64_t quotient = 0;
    for (int bit = 63; bit >= 0; --bit) {
        const bool carry = (rem >> 63) != 0;
        rem = (rem << 1) | ((n.lo >> bit) & 1u);
        quotient <<= 1;
        if (carry || rem >= d) {
            rem -= d;
            quotient |= 1u;
        }
    }
    return {quotient, rem};
}

/// Applies the sign and range check to a magnitude.
constexpr std::optional<std::int64_t> signedFromMagnitude(std::uint64_t mag, bool negative) {
    constexpr std::uint64_t kMaxPositive = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (negative) {
        if (mag > kMaxPositive + 1) {
            return std::nullopt;
        }
        return mag == 0 ? std::int64_t{0} : -static_cast<std::int64_t>(mag - 1) - 1;
    }
    if (mag > kMaxPositive) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(mag);
}

} // namespace detail

constexpr std::optional<std::int64_t> checkedAdd(std::int64_t a, std::int64_t b) {
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    if ((b > 0 && a > hi - b) || (b < 0 && a < lo - b)) {
        return std::nullopt;
    }
    return a + b;
}

constexpr std::optional<std::int64_t> checkedSub(std::int64_t a, std::int64_t b) {
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    if ((b < 0 && a > hi + b) || (b > 0 && a < lo + b)) {
        return std::nullopt;
    }
    return a - b;
}

/// Negation; fails only for INT64_MIN.
constexpr std::optional<std::int64_t> checkedNeg(std::int64_t a) {
    if (a == std::numeric_limits<std::int64_t>::min()) {
        return std::nullopt;
    }
    return -a;
}

constexpr std::optional<std::int64_t> checkedMul(std::int64_t a, std::int64_t b) {
    const detail::U128 p = detail::mul64(detail::magnitude(a), detail::magnitude(b));
    if (p.hi != 0) {
        return std::nullopt;
    }
    return detail::signedFromMagnitude(p.lo, (a < 0) != (b < 0));
}

constexpr std::optional<Ticks> checkedAdd(Ticks a, Ticks b) {
    const auto v = checkedAdd(a.value, b.value);
    return v ? std::optional<Ticks>(Ticks{*v}) : std::nullopt;
}

constexpr std::optional<Ticks> checkedSub(Ticks a, Ticks b) {
    const auto v = checkedSub(a.value, b.value);
    return v ? std::optional<Ticks>(Ticks{*v}) : std::nullopt;
}

constexpr std::optional<Ticks> checkedNeg(Ticks a) {
    const auto v = checkedNeg(a.value);
    return v ? std::optional<Ticks>(Ticks{*v}) : std::nullopt;
}

/// value * mul / div computed exactly through a 128-bit intermediate and rounded as asked.
/// Returns std::nullopt when div is 0 or the result does not fit int64. Any signs are allowed.
constexpr std::optional<std::int64_t> mulDiv(std::int64_t value, std::int64_t mul, std::int64_t div,
                                             Rounding rounding) {
    if (div == 0) {
        return std::nullopt;
    }
    const bool negative = ((value < 0) != (mul < 0)) != (div < 0);
    const detail::U128 product = detail::mul64(detail::magnitude(value), detail::magnitude(mul));
    const std::uint64_t d = detail::magnitude(div);
    if (product.hi >= d) {
        return std::nullopt; // the quotient alone needs more than 64 bits
    }
    const auto [quotient, remainder] = detail::div128(product, d);
    std::uint64_t mag = quotient;
    if (remainder != 0) {
        bool awayFromZero = false;
        switch (rounding) {
        case Rounding::Floor:
            awayFromZero = negative;
            break;
        case Rounding::Ceil:
            awayFromZero = !negative;
            break;
        case Rounding::Nearest:
            awayFromZero = remainder >= d - remainder; // 2r >= d without overflowing
            break;
        case Rounding::TowardZero:
            break;
        }
        if (awayFromZero) {
            if (mag == std::numeric_limits<std::uint64_t>::max()) {
                return std::nullopt;
            }
            ++mag;
        }
    }
    return detail::signedFromMagnitude(mag, negative);
}

constexpr std::optional<Ticks> mulDiv(Ticks value, std::int64_t mul, std::int64_t div, Rounding rounding) {
    const auto v = mulDiv(value.value, mul, div, rounding);
    return v ? std::optional<Ticks>(Ticks{*v}) : std::nullopt;
}

// --- Rates and time bases -----------------------------------------------------------------------------

/// Exact rational rate (frames or samples per second), e.g. {30000, 1001} for 29.97 fps.
/// Rates are positive. Two rates are equal when their values are equal ({60, 2} == {30, 1});
/// normalized() gives the reduced form and make() validates untrusted input.
struct Rate {
    std::int32_t num = 30;
    std::int32_t den = 1;

    constexpr bool operator==(const Rate& o) const {
        return static_cast<std::int64_t>(num) * o.den == static_cast<std::int64_t>(o.num) * den &&
               isValid() == o.isValid();
    }
    constexpr bool isValid() const { return num > 0 && den > 0; }
    constexpr bool isNormalized() const { return isValid() && std::gcd(num, den) == 1; }
    /// The reduced form ({60, 2} -> {30, 1}); invalid rates are returned unchanged.
    constexpr Rate normalized() const {
        if (!isValid()) {
            return *this;
        }
        const std::int32_t g = std::gcd(num, den);
        return {num / g, den / g};
    }
    /// A validated, normalized rate from untrusted values; nullopt unless both are positive and the
    /// reduced form fits int32.
    static constexpr std::optional<Rate> make(std::int64_t num, std::int64_t den) {
        if (num <= 0 || den <= 0) {
            return std::nullopt;
        }
        const std::int64_t g = std::gcd(num, den);
        num /= g;
        den /= g;
        if (num > std::numeric_limits<std::int32_t>::max() || den > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        return Rate{static_cast<std::int32_t>(num), static_cast<std::int32_t>(den)};
    }
    /// True when one frame/sample is a whole number of ticks (all supported rates).
    constexpr bool isExact() const {
        return isValid() && kTicksPerSecond % (num / std::gcd(num, den)) == 0;
    }
    /// Ticks per frame/sample. Only meaningful for exact rates; use the checked helpers otherwise.
    constexpr std::int64_t ticksPerUnit() const {
        const std::int32_t g = std::gcd(num, den);
        return kTicksPerSecond / (num / g) * (den / g);
    }
    constexpr double toDouble() const { return static_cast<double>(num) / static_cast<double>(den); }
};

inline constexpr Rate kRate23_976{24000, 1001};
inline constexpr Rate kRate24{24, 1};
inline constexpr Rate kRate25{25, 1};
inline constexpr Rate kRate29_97{30000, 1001};
inline constexpr Rate kRate30{30, 1};
inline constexpr Rate kRate50{50, 1};
inline constexpr Rate kRate59_94{60000, 1001};
inline constexpr Rate kRate60{60, 1};
inline constexpr Rate kRate48kHz{48000, 1};
inline constexpr Rate kRate44_1kHz{44100, 1};

/// A stream time base, as FFmpeg's AVStream::time_base: one unit lasts num/den seconds
/// ({1, 90000} for MPEG-TS, {1001, 30000} for an NTSC frame clock). Unlike Rate, any positive
/// ratio is allowed and it need not be exact in ticks; convert through rescale().
struct TimeBase {
    std::int64_t num = 1;
    std::int64_t den = 1;

    constexpr bool isValid() const { return num > 0 && den > 0; }
    constexpr bool operator==(const TimeBase& o) const {
        // Compare num/den == o.num/o.den without overflowing.
        return isValid() && o.isValid() ? num / std::gcd(num, den) == o.num / std::gcd(o.num, o.den) &&
                                              den / std::gcd(num, den) == o.den / std::gcd(o.num, o.den)
                                        : num == o.num && den == o.den;
    }
};

/// The time base whose unit is one frame/sample of `rate`.
constexpr TimeBase timeBaseOf(Rate rate) { return {rate.den, rate.num}; }

namespace detail {

/// value * (a * b) / c with the factors reduced first so the combined multiplier fits int64.
constexpr std::optional<std::int64_t> scaleBy(std::int64_t value, std::int64_t a, std::int64_t b, std::int64_t c,
                                              Rounding rounding) {
    if (c <= 0 || a <= 0 || b <= 0) {
        return std::nullopt;
    }
    const std::int64_t g1 = std::gcd(a, c);
    a /= g1;
    c /= g1;
    const std::int64_t g2 = std::gcd(b, c);
    b /= g2;
    c /= g2;
    const auto mul = checkedMul(a, b);
    if (!mul) {
        return std::nullopt;
    }
    return mulDiv(value, *mul, c, rounding);
}

} // namespace detail

/// Stream units -> ticks: units * num * kTicksPerSecond / den.
constexpr std::optional<Ticks> rescale(std::int64_t units, TimeBase from, Rounding rounding) {
    if (!from.isValid()) {
        return std::nullopt;
    }
    const auto v = detail::scaleBy(units, from.num, kTicksPerSecond, from.den, rounding);
    return v ? std::optional<Ticks>(Ticks{*v}) : std::nullopt;
}

/// Ticks -> stream units: ticks * den / (num * kTicksPerSecond).
constexpr std::optional<std::int64_t> rescale(Ticks t, TimeBase to, Rounding rounding) {
    if (!to.isValid()) {
        return std::nullopt;
    }
    // Reduce den against both divisor factors, then divide by num * kTicksPerSecond.
    std::int64_t den = to.den;
    std::int64_t num = to.num;
    std::int64_t ticks = kTicksPerSecond;
    const std::int64_t g1 = std::gcd(den, ticks);
    den /= g1;
    ticks /= g1;
    const std::int64_t g2 = std::gcd(den, num);
    den /= g2;
    num /= g2;
    const auto divisor = checkedMul(num, ticks);
    if (!divisor) {
        return std::nullopt;
    }
    return mulDiv(t.value, den, *divisor, rounding);
}

/// Checked frame/sample conversions that also work for rates that are not a whole number of ticks.
constexpr std::optional<Ticks> framesToTicks(std::int64_t frames, Rate rate, Rounding rounding) {
    return rate.isValid() ? rescale(frames, timeBaseOf(rate), rounding) : std::nullopt;
}
constexpr std::optional<std::int64_t> ticksToFrames(Ticks t, Rate rate, Rounding rounding) {
    return rate.isValid() ? rescale(t, timeBaseOf(rate), rounding) : std::nullopt;
}

/// Floor division (toward negative infinity) — frame index of a tick position.
constexpr std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Fast unchecked conversions for exact rates and values in timeline range (they are what the edit engine
// uses). Prefer the checked forms above for untrusted or extreme values.
constexpr Ticks unitsToTicks(std::int64_t units, Rate r) { return {units * r.ticksPerUnit()}; }
constexpr std::int64_t ticksToUnits(Ticks t, Rate r) { return floorDiv(t.value, r.ticksPerUnit()); }

/// Frame-accurate helpers (aliases that read better at call sites).
constexpr Ticks framesToTicks(std::int64_t frames, Rate fps) { return unitsToTicks(frames, fps); }
constexpr std::int64_t ticksToFrames(Ticks t, Rate fps) { return ticksToUnits(t, fps); }
constexpr Ticks samplesToTicks(std::int64_t samples, Rate sampleRate) { return unitsToTicks(samples, sampleRate); }
constexpr std::int64_t ticksToSamples(Ticks t, Rate sampleRate) { return ticksToUnits(t, sampleRate); }

/// Snap a position down to the start of the frame that contains it.
constexpr Ticks snapToFrame(Ticks t, Rate fps) { return framesToTicks(ticksToFrames(t, fps), fps); }

/// Half-open range [start, start + duration).
struct TimeRange {
    Ticks start;
    Ticks duration;

    constexpr bool operator==(const TimeRange&) const = default;
    constexpr Ticks end() const { return start + duration; }
    constexpr bool isEmpty() const { return duration.value <= 0; }
    constexpr bool contains(Ticks t) const { return t >= start && t < end(); }
    constexpr bool overlaps(const TimeRange& o) const { return start < o.end() && o.start < end(); }
    static constexpr TimeRange fromStartEnd(Ticks s, Ticks e) { return {s, e - s}; }
};

// --- Time maps (retiming) -----------------------------------------------------------------------------

/// Maps timeline time to source time with a rational speed:
///     source = sourceOrigin + (timeline - timelineOrigin) * speedNum / speedDen
/// speedNum > 0 plays forward (2/1 = twice as fast), < 0 plays in reverse, 0 freezes on sourceOrigin.
/// speedDen must be positive. A clip's map has timelineOrigin = its start and sourceOrigin = its
/// source In.
///
/// Round trips: map() then inverse() returns the input exactly whenever the speed's denominator
/// divides (timeline - timelineOrigin) — always for integer speeds; inverse() then map() is exact
/// whenever |speedNum| divides (source - sourceOrigin) * speedDen — always for 1/n slow motion.
/// Otherwise the result is rounded as asked (Floor: the frame that contains the point).
struct TimeMap {
    Ticks timelineOrigin;
    Ticks sourceOrigin;
    std::int64_t speedNum = 1;
    std::int64_t speedDen = 1;

    constexpr bool operator==(const TimeMap&) const = default;
    constexpr bool isValid() const { return speedDen > 0; }
    constexpr bool isFreeze() const { return speedNum == 0; }
    constexpr bool isReverse() const { return speedNum < 0; }
    constexpr bool isIdentitySpeed() const { return speedNum == speedDen; }

    /// Source time shown at timeline time `t`.
    constexpr std::optional<Ticks> map(Ticks t, Rounding rounding = Rounding::Floor) const {
        if (!isValid()) {
            return std::nullopt;
        }
        const auto delta = checkedSub(t, timelineOrigin);
        if (!delta) {
            return std::nullopt;
        }
        const auto scaled = mulDiv(*delta, speedNum, speedDen, rounding);
        return scaled ? checkedAdd(sourceOrigin, *scaled) : std::nullopt;
    }

    /// Timeline time at which source time `s` is shown; nullopt for a freeze (no single answer).
    constexpr std::optional<Ticks> inverse(Ticks s, Rounding rounding = Rounding::Floor) const {
        if (!isValid() || isFreeze()) {
            return std::nullopt;
        }
        const auto delta = checkedSub(s, sourceOrigin);
        if (!delta) {
            return std::nullopt;
        }
        const auto scaled = mulDiv(*delta, speedDen, speedNum, rounding);
        return scaled ? checkedAdd(timelineOrigin, *scaled) : std::nullopt;
    }

    /// Source duration covered by a timeline duration (negative in reverse).
    constexpr std::optional<Ticks> mapDuration(Ticks d, Rounding rounding = Rounding::Floor) const {
        return isValid() ? mulDiv(d, speedNum, speedDen, rounding) : std::nullopt;
    }
};

static_assert(kRate23_976.isExact() && kRate29_97.isExact() && kRate59_94.isExact());
static_assert(kRate48kHz.isExact() && kRate44_1kHz.isExact());
static_assert(Rate{96000, 1}.isExact() && Rate{192000, 1}.isExact() && Rate{32000, 1}.isExact());
static_assert(Rate{60, 2} == kRate30 && Rate{60, 2}.normalized().num == 30);
static_assert(mulDiv(std::int64_t{7}, 1, 2, Rounding::Nearest) == 4 && mulDiv(std::int64_t{-7}, 1, 2, Rounding::Nearest) == -4);
static_assert(mulDiv(std::int64_t{-7}, 1, 2, Rounding::Floor) == -4 && mulDiv(std::int64_t{-7}, 1, 2, Rounding::Ceil) == -3);
static_assert(rescale(Ticks{kTicksPerSecond}, TimeBase{1, 90000}, Rounding::Floor) == 90000);

} // namespace wabcut
