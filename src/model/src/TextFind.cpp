#include <wabcut/model/TextFind.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace wabcut {

namespace {

struct FoldRun {
    char32_t first;
    std::uint32_t count;
    std::int32_t delta;
    std::uint32_t step;
};

#include "CaseFolding.inc"

/// A code point of the text and where it starts. Bytes that are not UTF-8 become values above U+10FFFF (0x110000 +
/// the byte), so they match only themselves.
struct Unit {
    char32_t c;
    std::size_t at;
};

std::vector<Unit> decode(std::string_view s) {
    std::vector<Unit> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        std::size_t n = b0 < 0x80 ? 1 : (b0 >> 5) == 0x6 ? 2 : (b0 >> 4) == 0xE ? 3 : (b0 >> 3) == 0x1E ? 4 : 0;
        char32_t c = n == 1 ? b0 : n == 2 ? (b0 & 0x1F) : n == 3 ? (b0 & 0x0F) : (b0 & 0x07);
        bool ok = n > 0 && i + n <= s.size();
        for (std::size_t k = 1; ok && k < n; ++k) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            ok = (b & 0xC0) == 0x80;
            c = (c << 6) | (b & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF are not UTF-8.
        ok = ok && !(n == 2 && c < 0x80) && !(n == 3 && c < 0x800) && !(n == 4 && (c < 0x10000 || c > 0x10FFFF)) &&
             !(c >= 0xD800 && c <= 0xDFFF);
        if (!ok) {
            out.push_back({static_cast<char32_t>(0x110000u + b0), i});
            ++i;
            continue;
        }
        out.push_back({c, i});
        i += n;
    }
    return out;
}

void appendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

} // namespace

char32_t foldCase(char32_t c) {
    if (c < 0x80) {
        return c >= 'A' && c <= 'Z' ? c + 32 : c;
    }
    // The last run that starts at or before c.
    const auto* end = std::end(kFoldRuns);
    const auto* it = std::upper_bound(std::begin(kFoldRuns), end, c, [](char32_t v, const FoldRun& r) { return v < r.first; });
    if (it == std::begin(kFoldRuns)) {
        return c;
    }
    const FoldRun& r = *(it - 1);
    const char32_t offset = c - r.first;
    if (offset % r.step != 0 || offset / r.step >= r.count) {
        return c;
    }
    return static_cast<char32_t>(static_cast<std::int64_t>(c) + r.delta);
}

std::string foldCase(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    const std::vector<Unit> units = decode(text);
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (units[i].c >= 0x110000u) {
            out.push_back(text[units[i].at]);
        } else {
            appendUtf8(out, foldCase(units[i].c));
        }
    }
    return out;
}

bool isWordCharacter(char32_t c) {
    if (c < 0x80) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    if (c >= 0x110000u) {
        return true; // a byte that is not UTF-8: part of whatever it is glued to
    }
    if (c >= 0xA0 && c <= 0xBF) {
        return c == 0xAA || c == 0xB5 || c == 0xBA; // Latin-1 punctuation, but ª µ º are letters
    }
    if (c == 0xD7 || c == 0xF7) {
        return false; // × ÷
    }
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF01 && c <= 0xFF0F) ||
        (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65) ||
        (c >= 0xFE10 && c <= 0xFE6F) || c == 0x0E2F || c == 0x0E46 || c == 0x0E4F || c == 0x0E5A || c == 0x0E5B) {
        return false; // spaces and punctuation (Thai: paiyannoi, mai yamok, fongman, angkhankhu, khomut)
    }
    if ((c >= 0x2190 && c <= 0x2BFF) || (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0xFE00 && c <= 0xFE0F)) {
        return false; // arrows, symbols, dingbats and emoji (with their variation selectors)
    }
    return true;
}

std::vector<std::pair<std::size_t, std::size_t>> findText(std::string_view text, std::string_view find,
                                                         TextFindOptions options) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (find.empty() || find.size() > 4 * text.size()) {
        return out;
    }
    std::vector<Unit> t = decode(text);
    std::vector<Unit> f = decode(find);
    if (f.empty() || f.size() > t.size()) {
        return out;
    }
    const auto key = [&](char32_t c) { return options.matchCase || c >= 0x110000u ? c : foldCase(c); };
    std::vector<char32_t> tk(t.size());
    std::vector<char32_t> fk(f.size());
    for (std::size_t i = 0; i < t.size(); ++i) {
        tk[i] = key(t[i].c);
    }
    for (std::size_t i = 0; i < f.size(); ++i) {
        fk[i] = key(f[i].c);
    }
    std::size_t i = 0;
    while (i + f.size() <= t.size()) {
        bool same = std::equal(fk.begin(), fk.end(), tk.begin() + static_cast<std::ptrdiff_t>(i));
        if (same && options.wholeWord) {
            const std::size_t last = i + f.size(); // the unit after the match
            same = !(i > 0 && isWordCharacter(t[i - 1].c) && isWordCharacter(f.front().c)) &&
                   !(last < t.size() && isWordCharacter(t[last].c) && isWordCharacter(f.back().c));
        }
        if (same) {
            const std::size_t end = i + f.size() < t.size() ? t[i + f.size()].at : text.size();
            out.emplace_back(t[i].at, end);
            i += f.size();
        } else {
            ++i;
        }
    }
    return out;
}

std::string replaceText(std::string_view text, std::string_view find, std::string_view replacement,
                        TextFindOptions options, std::size_t* count) {
    const auto matches = findText(text, find, options);
    if (count) {
        *count = matches.size();
    }
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    for (const auto& [b, e] : matches) {
        out.append(text.substr(at, b - at));
        out.append(replacement);
        at = e;
    }
    out.append(text.substr(at));
    return out;
}

std::optional<std::string> replaceTextAt(std::string_view text, std::string_view find, std::string_view replacement,
                                         std::size_t at, TextFindOptions options) {
    for (const auto& [b, e] : findText(text, find, options)) {
        if (b == at) {
            std::string out(text.substr(0, b));
            out.append(replacement);
            out.append(text.substr(e));
            return out;
        }
    }
    return std::nullopt;
}

} // namespace wabcut
