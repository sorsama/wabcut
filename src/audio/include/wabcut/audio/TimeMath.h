#pragma once

// Saturating forms of the model's checked time arithmetic, for real-time paths that must always produce a
// value. Every scaling in wabcut_audio goes through wabcut::mulDiv (model/Time.h, 128-bit intermediate,
// explicit wabcut::Rounding) directly or through these wrappers (contract rev 2, Principle 1).

#include <wabcut/model/Time.h>

#include <cstdint>
#include <limits>

namespace wabcut::audio::timemath {

/// wabcut::mulDiv clamped to the int64 range instead of failing (0 when div is 0). The clamp is only reachable
/// with values far beyond anything a timeline or a device counter reaches (~36 years of 48 kHz frames).
constexpr std::int64_t mulDivSaturate(std::int64_t value, std::int64_t mul, std::int64_t div,
                                      Rounding rounding) noexcept {
    if (div == 0) {
        return 0;
    }
    if (const auto r = wabcut::mulDiv(value, mul, div, rounding)) {
        return *r;
    }
    const bool negative = ((value < 0) != (mul < 0)) != (div < 0);
    return negative ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
}

/// a + b, clamped to the int64 range.
constexpr std::int64_t addSaturate(std::int64_t a, std::int64_t b) noexcept {
    if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return a + b;
}

} // namespace wabcut::audio::timemath
