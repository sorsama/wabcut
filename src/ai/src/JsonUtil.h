#pragma once

// Internal helpers shared by the JSON codecs of wabcut_ai.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace wabcut::ai::detail {

/// 16 lowercase hex digits.
inline std::string hex64(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

inline std::optional<std::uint64_t> parseHex64(std::string_view s) {
    if (s.empty() || s.size() > 16) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc{} || end != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

/// The current UTC time as "YYYY-MM-DDTHH:MM:SSZ".
std::string utcNowIso8601();

} // namespace wabcut::ai::detail
