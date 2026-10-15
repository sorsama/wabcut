#include <wabcut/ai/WordBuilder.h>

#include <wabcut/ai/Text.h>

#include <algorithm>
#include <array>

namespace wabcut::ai {

namespace {

/// Complete characters from one or more tokens, with the span and probabilities of the tokens whose bytes
/// it holds.
struct Unit {
    std::string text;
    std::int64_t t0 = 0;
    std::int64_t t1 = 0;
    double probabilitySum = 0.0;
    int tokens = 0;
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool startsWithSpace(std::string_view s) { return !s.empty() && isSpace(s.front()); }
bool endsWithSpace(std::string_view s) { return !s.empty() && isSpace(s.back()); }

bool isAsciiAlnum(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
}

/// Joins token bytes into units of complete UTF-8 characters (see the header comment).
std::vector<Unit> unitsOf(const RecognizedSegment& segment) {
    std::vector<Unit> units;
    Unit pending;
    bool empty = true;
    for (const RecognizedToken& token : segment.tokens) {
        if (empty) {
            pending = Unit{{}, token.t0, token.t1, 0.0, 0};
            empty = false;
        }
        pending.text += token.bytes;
        pending.t0 = std::min(pending.t0, token.t0);
        pending.t1 = std::max(pending.t1, token.t1);
        pending.probabilitySum += token.probability;
        pending.tokens += 1;

        for (;;) {
            const std::size_t complete = text::completeUtf8Prefix(pending.text);
            if (complete == pending.text.size()) {
                units.push_back(std::move(pending));
                pending = Unit{};
                empty = true;
                break;
            }
            if (text::utf8SequenceAt(pending.text, complete) < 0) {
                // A character cut off at the end of this token: its remaining bytes come with the next one.
                if (complete > 0) {
                    Unit done = pending;
                    done.text.resize(complete);
                    units.push_back(std::move(done));
                    pending.text.erase(0, complete);
                    pending.t0 = token.t0;
                    pending.t1 = token.t1;
                    pending.probabilitySum = token.probability;
                    pending.tokens = 1;
                }
                break;
            }
            pending.text.replace(complete, 1, "\xEF\xBF\xBD"); // a malformed byte becomes U+FFFD
        }
    }
    if (!empty && !pending.text.empty()) {
        pending.text = text::sanitizeUtf8(pending.text); // a character that never completed
        units.push_back(std::move(pending));
    }
    return units;
}

struct Builder {
    std::vector<BuiltWord> words;
    double probabilitySum = 0.0; // of the last word
    int tokens = 0;

    void start(const Unit& u, std::string_view core) {
        words.push_back(BuiltWord{std::string(core), u.t0, u.t1, 0.0f});
        probabilitySum = 0.0;
        tokens = 0;
        add(u);
    }
    void extend(const Unit& u, std::string_view core) {
        words.back().text += core;
        add(u);
    }
    void add(const Unit& u) {
        BuiltWord& w = words.back();
        w.t0 = std::min(w.t0, u.t0);
        w.t1 = std::max(w.t1, u.t1);
        probabilitySum += u.probabilitySum;
        tokens += u.tokens;
        w.confidence = tokens > 0 ? static_cast<float>(probabilitySum / tokens) : 0.0f;
    }
};

} // namespace

WordSplit wordSplitFor(std::string_view language) {
    static constexpr std::array<std::string_view, 8> kNoSpaces = {"th", "lo", "km", "my", "zh", "ja", "bo", "yue"};
    return std::ranges::find(kNoSpaces, language) != kNoSpaces.end() ? WordSplit::PerUnit : WordSplit::Whitespace;
}

std::vector<BuiltWord> buildWords(const RecognizedSegment& segment, WordSplit split) {
    Builder b;
    bool open = false; // the last word may continue (no whitespace after it yet)
    for (const Unit& unit : unitsOf(segment)) {
        const std::string_view core = text::trimAsciiSpace(unit.text);
        if (core.empty()) {
            open = false;
            continue;
        }
        const bool lead = startsWithSpace(unit.text);
        const bool trail = endsWithSpace(unit.text);
        if (!b.words.empty() && (text::isTrailingPunctuation(core) || (!lead && text::startsWithCombiningMark(core)))) {
            b.extend(unit, core); // "Hello" "," -> "Hello,"; "ส" "ั" -> "สั"
            open = !trail;
            continue;
        }
        bool continues = false;
        if (split == WordSplit::Whitespace) {
            continues = open && !lead;
        } else {
            // Without spaces every unit is a word, except Latin letters and digits written inside such text
            // ("iPhone" in Thai), which do use spaces: those join like in Whitespace mode.
            continues = open && !lead && isAsciiAlnum(core) && !b.words.empty() &&
                        isAsciiAlnum(std::string_view(b.words.back().text).substr(b.words.back().text.size() - 1));
        }
        if (continues) {
            b.extend(unit, core);
        } else {
            b.start(unit, core);
        }
        open = !trail;
    }

    // Clamp into the segment and keep times non-decreasing.
    const std::int64_t lo = std::min(segment.t0, segment.t1);
    const std::int64_t hi = std::max(segment.t0, segment.t1);
    std::int64_t floor = lo;
    for (BuiltWord& w : b.words) {
        w.t0 = std::max(std::clamp(w.t0, lo, hi), floor);
        w.t1 = std::max(std::clamp(w.t1, lo, hi), w.t0);
        floor = w.t0;
    }
    return std::move(b.words);
}

std::string segmentText(const RecognizedSegment& segment) {
    std::string joined;
    for (const RecognizedToken& token : segment.tokens) {
        joined += token.bytes;
    }
    return std::string(text::trimAsciiSpace(text::sanitizeUtf8(joined)));
}

} // namespace wabcut::ai
