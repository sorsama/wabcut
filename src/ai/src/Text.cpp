#include <wabcut/ai/Text.h>

#include <array>
#include <cstdint>

namespace wabcut::ai::text {

namespace {

/// Length of the well-formed UTF-8 sequence at s[i], 0 if malformed, -1 if it is cut off by the end of `s`
/// (a valid start of a sequence that may continue).
int sequenceAt(std::string_view s, std::size_t i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char b0 = byte(i);
    if (b0 < 0x80) {
        return 1;
    }
    int length = 0;
    unsigned char lo = 0x80; // allowed range of the second byte (rules out overlongs and surrogates)
    unsigned char hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        length = 2;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        length = 3;
        if (b0 == 0xE0) {
            lo = 0xA0;
        } else if (b0 == 0xED) {
            hi = 0x9F;
        }
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        length = 4;
        if (b0 == 0xF0) {
            lo = 0x90;
        } else if (b0 == 0xF4) {
            hi = 0x8F;
        }
    } else {
        return 0;
    }
    for (int k = 1; k < length; ++k) {
        if (i + static_cast<std::size_t>(k) >= s.size()) {
            return -1;
        }
        const unsigned char b = byte(i + static_cast<std::size_t>(k));
        const unsigned char min = k == 1 ? lo : static_cast<unsigned char>(0x80);
        const unsigned char max = k == 1 ? hi : static_cast<unsigned char>(0xBF);
        if (b < min || b > max) {
            return 0;
        }
    }
    return length;
}

/// Decodes the (well-formed) sequence at s[i].
char32_t decodeAt(std::string_view s, std::size_t i, int length) {
    const auto b = [&](int k) { return static_cast<char32_t>(static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)])); };
    switch (length) {
    case 1:
        return b(0);
    case 2:
        return ((b(0) & 0x1F) << 6) | (b(1) & 0x3F);
    case 3:
        return ((b(0) & 0x0F) << 12) | ((b(1) & 0x3F) << 6) | (b(2) & 0x3F);
    default:
        return ((b(0) & 0x07) << 18) | ((b(1) & 0x3F) << 12) | ((b(2) & 0x3F) << 6) | (b(3) & 0x3F);
    }
}

} // namespace

int utf8SequenceAt(std::string_view s, std::size_t i) { return i < s.size() ? sequenceAt(s, i) : 0; }

bool isValidUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const int n = sequenceAt(s, i);
        if (n <= 0) {
            return false;
        }
        i += static_cast<std::size_t>(n);
    }
    return true;
}

std::size_t completeUtf8Prefix(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const int n = sequenceAt(s, i);
        if (n <= 0) {
            break;
        }
        i += static_cast<std::size_t>(n);
    }
    return i;
}

std::string sanitizeUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const int n = sequenceAt(s, i);
        if (n > 0) {
            out.append(s.substr(i, static_cast<std::size_t>(n)));
            i += static_cast<std::size_t>(n);
        } else {
            out.append("\xEF\xBF\xBD"); // U+FFFD
            ++i;
        }
    }
    return out;
}

std::string_view trimAsciiSpace(std::string_view s) {
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

bool startsWithCombiningMark(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    const int n = sequenceAt(s, 0);
    if (n <= 0) {
        return false;
    }
    const char32_t c = decodeAt(s, 0, n);
    const auto in = [c](char32_t lo, char32_t hi) { return c >= lo && c <= hi; };
    return in(0x0300, 0x036F) ||                                             // combining diacritical marks
           c == 0x0E31 || in(0x0E34, 0x0E3A) || in(0x0E47, 0x0E4E) ||        // Thai
           c == 0x0EB1 || in(0x0EB4, 0x0EBC) || in(0x0EC8, 0x0ECE) ||        // Lao
           in(0x17B4, 0x17D3) ||                                             // Khmer dependent signs
           in(0x102B, 0x103E) ||                                             // Myanmar dependent signs
           in(0x0900, 0x0903) || in(0x093A, 0x094F) || in(0x0951, 0x0957) || // Devanagari
           in(0x3099, 0x309A) ||                                             // kana voicing marks
           in(0x1AB0, 0x1AFF) || in(0x20D0, 0x20FF) || in(0xFE20, 0xFE2F);   // more combining marks
}

bool isTrailingPunctuation(std::string_view s) {
    static constexpr std::array<char32_t, 25> kPunctuation = {
        U'.',  U',',  U'!',  U'?',  U';',  U':',  U')',  U']',  U'}',  U'%',  U'\'', U'"',  U'、',
        U'。', U'，', U'！', U'？', U'：', U'；', U'」', U'』', U'）',
        U'…', U'ฯ', U'ๆ'};
    if (s.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < s.size();) {
        const int n = sequenceAt(s, i);
        if (n <= 0) {
            return false;
        }
        const char32_t c = decodeAt(s, i, n);
        bool found = false;
        for (const char32_t p : kPunctuation) {
            found = found || p == c;
        }
        if (!found) {
            return false;
        }
        i += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace wabcut::ai::text
