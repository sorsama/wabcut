#pragma once

// counted(): a count with its noun in the right number, for the plain-text messages the engine libraries write
// (command results, import reports, verification notes) that never pass through Qt's translation ("1 clip", "0 clips",
// "5 clips"). The app's own strings use Qt's numerus form instead ("%n clip(s)", EnglishPlurals.h).
//
//   counted(1, "clip")             -> "1 clip"
//   counted(3, "media item")       -> "3 media items"
//   counted(2, "match", "matches") -> "2 matches"

#include <cstdint>
#include <string>
#include <string_view>

namespace wabcut {

inline std::string counted(std::int64_t n, std::string_view one, std::string_view many = {}) {
    std::string out = std::to_string(n) + ' ';
    if (n == 1) {
        out += one;
    } else if (many.empty()) {
        out += one;
        out += 's';
    } else {
        out += many;
    }
    return out;
}

} // namespace wabcut
