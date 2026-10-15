// Caption text (CaptionText.h): display length, the simple line breaker and the balanced line-breaking search.

#include <wabcut/model/CaptionText.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace wabcut {

namespace {

/// The code point at `i` and its length in bytes; an invalid sequence reads as U+FFFD of one byte.
std::pair<char32_t, std::size_t> decode(std::string_view s, std::size_t i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        return {b0, 1};
    }
    std::size_t n = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        n = 2;
        cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        n = 3;
        cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        n = 4;
        cp = b0 & 0x07;
    } else {
        return {0xFFFD, 1};
    }
    if (i + n > s.size()) {
        return {0xFFFD, 1};
    }
    for (std::size_t k = 1; k < n; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            return {0xFFFD, 1};
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    const bool overlong = (n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000);
    if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return {0xFFFD, 1};
    }
    return {cp, n};
}

bool validAt(std::string_view s, std::size_t i, std::size_t& length) {
    const auto [cp, n] = decode(s, i);
    length = n;
    return !(cp == 0xFFFD && n == 1 && static_cast<unsigned char>(s[i]) >= 0x80);
}

/// Combining marks and zero-width characters: they take no cell.
bool takesNoCell(char32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) || (c >= 0x0591 && c <= 0x05BD) ||
           (c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F) || c == 0x0670 ||
           (c >= 0x0900 && c <= 0x0902) || c == 0x093C || (c >= 0x0941 && c <= 0x0948) || c == 0x094D ||
           c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E3A) || (c >= 0x0E47 && c <= 0x0E4E) ||  // Thai
           c == 0x0EB1 || (c >= 0x0EB4 && c <= 0x0EBC) || (c >= 0x0EC8 && c <= 0x0ECD) ||  // Lao
           (c >= 0x17B4 && c <= 0x17D3 && c != 0x17B6 && !(c >= 0x17BE && c <= 0x17C5) && c != 0x17C7 &&
            c != 0x17C8) ||                                                                  // Khmer (vowel signs above/below)
           (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) ||
           c == 0xFEFF;
}

bool isSpace(char c) { return c == ' ' || c == '\t'; }

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

/// A step of the line-breaking search: the longest line of the best way to lay out the rest, and its first break.
struct Cell {
    std::size_t longest = kNone;
    std::size_t next = 0;
    int score = 0; ///< tie-break: breaks after punctuation, a first line no longer than the rest
};

bool endsClause(std::string_view line) {
    for (const std::string_view end : {".", ",", ";", ":", "?", "!", "\xE3\x80\x82", "\xEF\xBC\x8C", "\xE3\x80\x81",
                                       "\xEF\xBC\x9F", "\xEF\xBC\x81", "\xE2\x80\xA6"}) {
        if (line.ends_with(end)) {
            return true;
        }
    }
    return false;
}

/// `text` with newlines as a space (nothing in an unspaced language), trimmed.
std::string lineText(std::string_view text, bool unspaced) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '\n' || c == '\r') {
            if (!unspaced) {
                out += ' ';
            }
        } else {
            out += c;
        }
    }
    const std::size_t a = out.find_first_not_of(" \t");
    if (a == std::string::npos) {
        return {};
    }
    const std::size_t b = out.find_last_not_of(" \t");
    return out.substr(a, b - a + 1);
}

class SimpleBreaker final : public LineBreaker {
public:
    std::vector<std::size_t> breakOpportunities(std::string_view text, std::string_view) const override {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if ((isSpace(c) || c == '\n') && i + 1 < text.size() && !isSpace(text[i + 1]) && text[i + 1] != '\n') {
                out.push_back(i + 1); // after a run of spaces
            } else if (c == '-' && i > 0 && i + 1 < text.size() && !isSpace(text[i - 1]) && !isSpace(text[i + 1])) {
                out.push_back(i + 1); // after a hyphen inside a word
            } else if (i + 3 < text.size() && text.substr(i, 2) == "\xE2\x80" &&
                       (text[i + 2] == '\x93' || text[i + 2] == '\x94')) {
                out.push_back(i + 3); // after an en or em dash
            }
        }
        return out;
    }
};

} // namespace

const LineBreaker& simpleLineBreaker() {
    static const SimpleBreaker breaker;
    return breaker;
}

std::size_t captionDisplayLength(std::string_view text) {
    std::size_t length = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto [cp, n] = decode(text, i);
        if (!takesNoCell(cp)) {
            ++length;
        }
        i += n;
    }
    return length;
}

bool isUnspacedLanguage(std::string_view language) {
    const std::string_view base = language.substr(0, language.find_first_of("-_"));
    for (const std::string_view code : {"th", "lo", "km", "my", "zh", "ja", "bo"}) {
        if (base.size() == code.size() && std::equal(base.begin(), base.end(), code.begin(), [](char a, char b) {
                return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
            })) {
            return true;
        }
    }
    return false;
}

bool isValidCaptionText(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 && c != '\n' && c != '\t') {
            return false;
        }
        if (c == 0x7F) {
            return false;
        }
        std::size_t n = 1;
        if (!validAt(text, i, n)) {
            return false;
        }
        i += n;
    }
    return true;
}

std::string flattenCaptionText(std::string_view text, std::string_view language) {
    const bool unspaced = isUnspacedLanguage(language);
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (c == '\n' || c == '\r') {
            space = space || !unspaced;
            continue;
        }
        if (isSpace(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out += ' ';
        }
        space = false;
        out += c;
    }
    return out;
}

std::optional<std::vector<std::string>> breakCaptionLines(std::string_view text, std::string_view language,
                                                          int maxCharsPerLine, int maxLines, const LineBreaker& breaker,
                                                          std::span<const std::size_t> extra) {
    const bool unspaced = isUnspacedLanguage(language);
    if (lineText(text, unspaced).empty()) {
        return std::vector<std::string>{};
    }
    // Pieces between break opportunities (UTF-8 boundaries only); a newline is always one.
    std::vector<std::size_t> cuts = breaker.breakOpportunities(text, language);
    cuts.insert(cuts.end(), extra.begin(), extra.end());
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == '\n') {
            cuts.push_back(i + 1);
        }
    }
    cuts.push_back(0);
    cuts.push_back(text.size());
    std::erase_if(cuts, [&](std::size_t c) {
        return c > text.size() || (c < text.size() && (static_cast<unsigned char>(text[c]) & 0xC0) == 0x80);
    });
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    const std::size_t n = cuts.size() - 1;
    const auto max = static_cast<std::size_t>(std::max(1, maxCharsPerLine));
    const auto piece = [&](std::size_t i, std::size_t j) { return lineText(text.substr(cuts[i], cuts[j] - cuts[i]), unspaced); };
    // Line lengths: prefix sums of the pieces' lengths with their spaces; a line drops its leading and trailing ones.
    std::vector<std::size_t> rawLength(n), lead(n), trail(n);
    std::vector<bool> clause(n); // a line ending with the piece ends a clause
    for (std::size_t k = 0; k < n; ++k) {
        std::string raw(text.substr(cuts[k], cuts[k + 1] - cuts[k]));
        for (char& c : raw) {
            if (c == '\n' || c == '\r') {
                c = unspaced ? '\0' : ' ';
            }
        }
        std::erase(raw, '\0');
        rawLength[k] = captionDisplayLength(raw);
        lead[k] = raw.find_first_not_of(" \t") == std::string::npos ? raw.size() : raw.find_first_not_of(" \t");
        const std::size_t last = raw.find_last_not_of(" \t");
        trail[k] = last == std::string::npos ? raw.size() : raw.size() - 1 - last;
        clause[k] = endsClause(lineText(raw, unspaced));
    }
    std::vector<std::size_t> prefix(n + 1, 0);
    for (std::size_t k = 0; k < n; ++k) {
        prefix[k + 1] = prefix[k] + rawLength[k];
    }
    const auto len = [&](std::size_t i, std::size_t j) -> std::size_t {
        const std::size_t total = prefix[j] - prefix[i];
        const std::size_t trimmed = lead[i] + trail[j - 1];
        return total > trimmed ? total - trimmed : 0;
    };
    if (n > 512) {
        // Too many pieces for the balanced search: greedy lines.
        std::vector<std::string> out;
        std::size_t i = 0;
        while (i < n) {
            std::size_t j = i + 1;
            while (j < n && len(i, j + 1) <= max) {
                ++j;
            }
            out.push_back(piece(i, j));
            i = j;
        }
        if (out.size() > static_cast<std::size_t>(std::max(1, maxLines))) {
            return std::nullopt;
        }
        return out;
    }
    for (int lines = 1; lines <= std::max(1, maxLines); ++lines) {
        std::vector<std::vector<Cell>> best(static_cast<std::size_t>(lines) + 1, std::vector<Cell>(n + 1));
        best[0][n] = Cell{0, n, 0};
        for (int k = 1; k <= lines; ++k) {
            for (std::size_t i = n; i-- > 0;) {
                Cell& cell = best[static_cast<std::size_t>(k)][i];
                for (std::size_t j = i + 1; j <= n; ++j) {
                    const std::size_t here = len(i, j);
                    if (here > max && j > i + 1) {
                        break;
                    }
                    const Cell& rest = best[static_cast<std::size_t>(k) - 1][j];
                    if (rest.longest == kNone) {
                        continue;
                    }
                    const std::size_t longest = std::max(here, rest.longest);
                    int score = rest.score;
                    if (j < n) {
                        score += clause[j - 1] ? 4 : 0;
                        score += here <= len(j, n) ? 1 : 0;
                    }
                    if (longest < cell.longest || (longest == cell.longest && score > cell.score)) {
                        cell = Cell{longest, j, score};
                    }
                }
            }
        }
        if (best[static_cast<std::size_t>(lines)][0].longest == kNone) {
            continue;
        }
        std::vector<std::string> out;
        bool fits = true;
        std::size_t i = 0;
        for (int k = lines; k > 0 && i < n; --k) {
            const std::size_t j = best[static_cast<std::size_t>(k)][i].next;
            if (len(i, j) > max && j > i + 1) {
                fits = false;
            }
            std::string line = piece(i, j);
            if (!line.empty()) {
                out.push_back(std::move(line));
            }
            i = j;
        }
        if (fits && i == n) {
            return out;
        }
    }
    return std::nullopt;
}

std::string wrapCaptionText(std::string_view text, std::string_view language, int maxCharsPerLine, int maxLines,
                            const LineBreaker& breaker, std::span<const std::size_t> extra) {
    const auto lines = breakCaptionLines(text, language, maxCharsPerLine, maxLines, breaker, extra);
    if (!lines) {
        return flattenCaptionText(text, language);
    }
    std::string out;
    for (const std::string& line : *lines) {
        out += (out.empty() ? "" : "\n") + line;
    }
    return out;
}

} // namespace wabcut
