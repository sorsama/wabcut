#include <wabcut/ai/roughcut/ScriptCorrections.h>

#include <wabcut/ai/edit/CaptionBuilder.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/edit/TranscriptEditing.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/CaptionOps.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace wabcut::ai::roughcut {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

// ---- The script's words ------------------------------------------------------------------------------------------

/// A token of the script text: a run of a spaced script's characters between spaces (punctuation attached: "OpenAI,")
/// or one character cluster of an unspaced script (Thai: a base character with its marks). Its units are the units
/// TextMatch gives it, so a stretch of units maps back to the exact script text that says it.
struct Piece {
    std::size_t begin = 0; ///< bytes of the script text
    std::size_t end = 0;
};

/// Decodes the code point at s[i] and advances i; malformed bytes are one U+FFFD each.
char32_t decodeAt(std::string_view s, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    int length = b0 < 0x80 ? 1 : (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : (b0 & 0xF8) == 0xF0 ? 4 : 0;
    if (length == 0 || i + static_cast<std::size_t>(length) > s.size()) {
        ++i;
        return 0xFFFD;
    }
    char32_t c = length == 1 ? b0 : length == 2 ? (b0 & 0x1Fu) : length == 3 ? (b0 & 0x0Fu) : (b0 & 0x07u);
    for (int k = 1; k < length; ++k) {
        const auto b = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        c = (c << 6) | (b & 0x3Fu);
    }
    i += static_cast<std::size_t>(length);
    return c;
}

bool within(char32_t c, char32_t lo, char32_t hi) { return c >= lo && c <= hi; }

/// Marks that attach to the character before them (TextMatch's clusters).
bool attaches(char32_t c) {
    return within(c, 0x0300, 0x036F) || c == 0x0E31 || within(c, 0x0E34, 0x0E3A) || within(c, 0x0E47, 0x0E4E) || c == 0x0EB1 ||
           within(c, 0x0EB4, 0x0EBC) || within(c, 0x0EC8, 0x0ECD) || within(c, 0x17B4, 0x17D3) || within(c, 0x102B, 0x103E) ||
           within(c, 0x1056, 0x1059) || within(c, 0x105E, 0x1060) || within(c, 0x3099, 0x309A) || within(c, 0xFE00, 0xFE0F) ||
           c == 0x200D;
}

/// Thai and Lao punctuation and repetition marks (TextMatch drops them): they stay with the cluster before them.
bool unspacedPunctuation(char32_t c) { return c == 0x0E2F || c == 0x0E46 || c == 0x0E4F || c == 0x0E5A || c == 0x0E5B; }

bool unspacedDigit(char32_t c) { return within(c, 0x0E50, 0x0E59) || within(c, 0x0ED0, 0x0ED9); }

bool isSpace(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x00A0 || c == 0x3000 || c == 0x200B; }

/// The units of `text` with Unit::source = the piece each comes from.
std::vector<Unit> scriptUnits(std::string_view text, std::vector<Piece>& pieces) {
    enum class Open { None, Word, Cluster };
    Open open = Open::None;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t at = i;
        const char32_t c = decodeAt(text, i);
        if (isSpace(c)) {
            open = Open::None;
            continue;
        }
        const bool cluster = isUnspacedCodePoint(c) && !unspacedDigit(c);
        if (attaches(c) || (cluster && unspacedPunctuation(c))) {
            if (!pieces.empty() && open != Open::None) {
                pieces.back().end = i;
            } else if (!pieces.empty() && unspacedPunctuation(c) && pieces.back().end == at) {
                pieces.back().end = i;
            }
            if (unspacedPunctuation(c)) {
                open = Open::None;
            }
            continue;
        }
        if (cluster) {
            pieces.push_back({at, i});
            open = Open::Cluster;
        } else if (open == Open::Word) {
            pieces.back().end = i;
        } else {
            pieces.push_back({at, i});
            open = Open::Word;
        }
    }
    std::vector<Unit> out;
    for (std::size_t p = 0; p < pieces.size(); ++p) {
        appendUnits(out, text.substr(pieces[p].begin, pieces[p].end - pieces[p].begin), static_cast<std::uint32_t>(p));
    }
    return out;
}

/// A line of the script ready to match.
struct Line {
    std::vector<Piece> pieces;
    std::vector<Unit> units;
    std::vector<std::uint32_t> unitsOfPiece; ///< per piece: how many units it gave
    bool shouting = false;                    ///< written mostly in capitals: its capitals mean nothing
};

Line lineOf(std::string_view text) {
    Line line;
    line.units = scriptUnits(text, line.pieces);
    line.unitsOfPiece.assign(line.pieces.size(), 0);
    for (const Unit& u : line.units) {
        ++line.unitsOfPiece[u.source];
    }
    std::size_t upper = 0;
    std::size_t letters = 0;
    for (const char ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ++upper;
            ++letters;
        } else if (ch >= 'a' && ch <= 'z') {
            ++letters;
        }
    }
    line.shouting = letters >= 8 && upper * 10 >= letters * 6;
    return line;
}

// ---- Words, capitals and sentences ----------------------------------------------------------------------------------

/// Part of a word: letters, digits and marks; not the punctuation or quotes around it.
bool wordCodePoint(char32_t c) {
    if (c < 0x80) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }
    return !(within(c, 0x00A0, 0x00BF) || c == 0x00D7 || c == 0x00F7 || within(c, 0x2000, 0x206F) || within(c, 0x3000, 0x303F) ||
             within(c, 0xFF01, 0xFF0F) || within(c, 0xFF1A, 0xFF20) || c == 0xFFFD || unspacedPunctuation(c));
}

/// A word split into the punctuation before it, the word ("OpenAI's": an apostrophe inside stays) and the punctuation
/// after it: "(OpenAI's," -> "(", "OpenAI's", ",".
struct Core {
    std::string_view lead;
    std::string_view word;
    std::string_view trail;
};

Core coreOf(std::string_view text) {
    std::size_t begin = text.size();
    std::size_t end = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t at = i;
        if (wordCodePoint(decodeAt(text, i))) {
            begin = std::min(begin, at);
            end = i;
        }
    }
    if (begin >= end) {
        return Core{text, {}, {}};
    }
    return Core{text.substr(0, begin), text.substr(begin, end - begin), text.substr(end)};
}

std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

bool isUpper(char ch) { return ch >= 'A' && ch <= 'Z'; }

/// Whether the text at `at` starts a sentence of `text`: only spaces, quotes and brackets lie between it and the
/// line's start or a sentence end (. ! ? : …).
bool startsSentence(std::string_view text, std::size_t at) {
    static constexpr std::string_view kOpenQuotes[] = {"\xE2\x80\x9C", "\xE2\x80\x98", "\xC2\xAB"}; // “ ‘ «
    while (at > 0) {
        const char ch = text[at - 1];
        if (ch == ' ' || ch == '\t' || ch == '"' || ch == '\'' || ch == '(' || ch == '[' || ch == '*' || ch == '_') {
            --at;
            continue;
        }
        bool quote = false;
        for (const std::string_view q : kOpenQuotes) {
            if (at >= q.size() && text.substr(at - q.size(), q.size()) == q) {
                at -= q.size();
                quote = true;
                break;
            }
        }
        if (!quote) {
            break;
        }
    }
    if (at == 0) {
        return true;
    }
    const char ch = text[at - 1];
    return ch == '.' || ch == '!' || ch == '?' || ch == ':' || ch == '\n' || (at >= 3 && text.substr(at - 3, 3) == "\xE2\x80\xA6");
}

using WordSet = std::unordered_set<std::string>;

/// Words the script writes with a first capital where no sentence starts and never in lower case: names ("Somchai")
/// keep that capital at the start of a sentence too.
WordSet properWords(const Script& script, const std::vector<Line>& lines) {
    WordSet capital;
    WordSet lower;
    for (std::size_t b = 0; b < lines.size(); ++b) {
        if (lines[b].shouting) {
            continue;
        }
        const std::string_view text = script.beats[b].text;
        for (const Piece& p : lines[b].pieces) {
            const Core core = coreOf(text.substr(p.begin, p.end - p.begin));
            if (core.word.empty()) {
                continue;
            }
            const char first = core.word.front();
            if (isUpper(first) && !startsSentence(text, p.begin)) {
                capital.insert(asciiLower(core.word));
            } else if (first >= 'a' && first <= 'z') {
                lower.insert(asciiLower(core.word));
            }
        }
    }
    std::erase_if(capital, [&](const std::string& w) { return lower.contains(w); });
    return capital;
}

/// Capitals of a script word that mean something: inside the word ("OpenAI", "iPhone", "NASA"), or a first capital
/// that does not merely start a sentence ("Kubernetes" mid-sentence, a known name anywhere).
bool meaningfulCapitals(std::string_view word, bool sentenceStart, bool properWord) {
    if (word.empty()) {
        return false;
    }
    for (std::size_t i = 1; i < word.size(); ++i) {
        if (isUpper(word[i])) {
            return true;
        }
    }
    return isUpper(word.front()) && (!sentenceStart || properWord);
}

// ---- Telling a mishearing from a different word ---------------------------------------------------------------------

std::u32string codePoints(std::span<const Unit> units) {
    std::u32string out;
    for (const Unit& u : units) {
        for (std::size_t i = 0; i < u.text.size();) {
            out += decodeAt(u.text, i);
        }
    }
    return out;
}

/// 1 - Levenshtein distance / the longer length.
template <class S>
double editSimilarity(const S& a, const S& b) {
    if (a.empty() && b.empty()) {
        return 1.0;
    }
    std::vector<std::size_t> prev(b.size() + 1);
    std::vector<std::size_t> cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        prev[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            cur[j] = std::min({prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1), prev[j] + 1, cur[j - 1] + 1});
        }
        std::swap(prev, cur);
    }
    return 1.0 - static_cast<double>(prev[b.size()]) / static_cast<double>(std::max(a.size(), b.size()));
}

/// A rough sound key of one lower-case ASCII word: a consonant skeleton with like sounds in one class (b/p, d/t,
/// c/g/k/q, f/v, s/z), digraphs as one sound (ph, th, sh, ch, ck), a vowel only where it starts the word, doubles
/// collapsed. "cooper" and "kuber" both read KPR.
std::string wordSoundKey(std::string_view w) {
    std::size_t i = 0;
    if (w.size() >= 2 && ((w[0] == 'k' || w[0] == 'g' || w[0] == 'p') && w[1] == 'n')) {
        i = 1; // knife, gnome, pneumatic
    } else if (w.size() >= 2 && ((w[0] == 'w' && w[1] == 'r') || (w[0] == 'p' && w[1] == 's'))) {
        i = 1; // write, psalm
    }
    std::string out;
    const auto push = [&out](char k) {
        if (out.empty() || out.back() != k) {
            out += k;
        }
    };
    const auto at = [&w](std::size_t k) { return k < w.size() ? w[k] : '\0'; };
    const std::size_t start = i;
    while (i < w.size()) {
        const char c = w[i];
        const char n1 = at(i + 1);
        const char n2 = at(i + 2);
        if (c == 't' && n1 == 'c' && n2 == 'h') {
            push('X');
            i += 3;
        } else if (c == 'd' && n1 == 'g') {
            push('J');
            i += 2;
        } else if (c == 'p' && n1 == 'h') {
            push('F');
            i += 2;
        } else if (c == 't' && n1 == 'h') {
            push('0');
            i += 2;
        } else if ((c == 's' || c == 'c') && n1 == 'h') {
            push('X');
            i += 2;
        } else if (c == 'c' && n1 == 'k') {
            push('K');
            i += 2;
        } else if (c == 'q' && n1 == 'u') {
            push('K');
            i += 2;
        } else if (c == 'x') {
            if (i == start) {
                push('S');
            } else {
                push('K');
                push('S');
            }
            ++i;
        } else if (c == 'c') {
            push(n1 == 'e' || n1 == 'i' || n1 == 'y' ? 'S' : 'K');
            ++i;
        } else if (c == 'g' && n1 == 'h' && i > start) {
            i += 2; // night, though
        } else if (c == 'w' && n1 == 'h') {
            if (i == start) {
                push('W');
            }
            i += 2;
        } else if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y') {
            if (i == start) {
                push('A');
            }
            ++i;
        } else if (c == 'h' || c == 'w') {
            if (i == start) {
                push(c == 'h' ? 'H' : 'W');
            }
            ++i;
        } else {
            switch (c) {
            case 'b': case 'p': push('P'); break;
            case 'd': case 't': push('T'); break;
            case 'g': case 'k': case 'q': push('K'); break;
            case 'f': case 'v': push('F'); break;
            case 's': case 'z': push('S'); break;
            case 'j': push('J'); break;
            case 'm': push('M'); break;
            case 'n': push('N'); break;
            case 'l': push('L'); break;
            case 'r': push('R'); break;
            default: break; // digits and anything else: not a sound here
            }
            ++i;
        }
    }
    return out;
}

/// The sound key of words of a spaced script ("web cut" and "Wabcut" both read WPKT); "" when any unit is not a
/// lower-case ASCII word (Thai and other scripts: letters alone decide).
std::string soundKey(std::span<const Unit> units) {
    std::string out;
    for (const Unit& u : units) {
        if (!u.word) {
            return {};
        }
        std::string letters;
        for (const char ch : u.text) {
            if (ch >= 'a' && ch <= 'z') {
                letters += ch;
            } else if (static_cast<unsigned char>(ch) >= 0x80) {
                return {};
            }
        }
        std::string key = wordSoundKey(letters);
        if (!out.empty() && !key.empty() && out.back() == key.front()) {
            key.erase(0, 1);
        }
        out += key;
    }
    return out;
}

/// How much `said` looks or sounds like `script` (0..1): letter similarity, raised towards the sound similarity when
/// the words sound alike ("cooper nettie's" / "Kubernetes": 0.54 in letters, 1 in sound -> 0.77).
double likeness(std::span<const Unit> said, std::span<const Unit> script) {
    const double letters = editSimilarity(codePoints(said), codePoints(script));
    const std::string a = soundKey(said);
    const std::string b = soundKey(script);
    if (a.empty() || b.empty()) {
        return letters;
    }
    return std::max(letters, (letters + editSimilarity(a, b)) / 2.0);
}

/// Above this a difference is a mishearing; at or below it, other words. Tuned on misheard names and jargon (all
/// above 0.5: "communities" / "Kubernetes" 0.52, "wildcat" / "Wabcut" 0.51) against everyday word swaps (all at or
/// below: "this" / "that" 0.5, "good" / "great" 0.43, "is" / "was" 0.42).
constexpr double kAlike = 0.5;

bool allDigits(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char ch) { return ch >= '0' && ch <= '9'; });
}

/// The value of number units ("25"; "20" "5" from "twenty-five"; "1" "100" "and" "20"; "19" "90" as a year; "3" "000"
/// from "3,000"; "2" "million"), or nullopt when they are not one number. (TextMatch already writes number words up
/// to a hundred as digits.)
std::optional<std::int64_t> numberValue(std::span<const Unit> us) {
    constexpr std::int64_t kLimit = 1'000'000'000'000'000;
    std::int64_t total = 0;
    std::int64_t current = 0;
    bool any = false;
    for (std::size_t i = 0; i < us.size(); ++i) {
        const std::string& t = us[i].text;
        if ((t == "and" && any && i + 1 < us.size()) || (t == "a" && i == 0 && us.size() > 1)) {
            continue; // "one hundred and five", "a hundred"
        }
        const std::int64_t scale = t == "thousand" ? 1'000 : t == "million" ? 1'000'000 : t == "billion" ? 1'000'000'000 : 0;
        if (scale > 0) {
            total += (current == 0 ? 1 : current) * scale;
            current = 0;
            any = true;
            continue;
        }
        if (!allDigits(t) || t.size() > 12) {
            return std::nullopt;
        }
        const std::int64_t v = std::stoll(t);
        if (!any || current == 0) {
            current = v;
        } else if (t.size() == 3 && t != "100") {
            current = current * 1000 + v; // "1,200" -> "1" "200"
        } else if (v == 100 && current < 100) {
            current *= 100; // "one hundred", "twelve hundred"
        } else if (v == 100) {
            current = current * 1000 + v; // "1,100"
        } else if (v < 10 && current >= 20 && current % 10 == 0) {
            current += v; // "twenty five", "one hundred twenty five"
        } else if (v < 100 && current >= 100 && current % 100 == 0) {
            current += v; // "one hundred five"
        } else if (v >= 10 && v < 100 && current >= 10 && current < 100) {
            current = current * 100 + v; // "nineteen ninety", "twenty twenty"
        } else {
            return std::nullopt;
        }
        if (current > kLimit || total > kLimit) {
            return std::nullopt;
        }
        any = true;
    }
    return any ? std::optional<std::int64_t>(total + current) : std::nullopt;
}

/// One spelling of words written several ways (abbreviations, ordinals), joined without spaces.
std::string canonicalForm(std::span<const Unit> us) {
    std::string joined;
    for (const Unit& u : us) {
        joined += u.text;
    }
    static const std::map<std::string_view, std::string_view> kForms{
        {"doctor", "dr"},       {"mister", "mr"},       {"missus", "mrs"},       {"misses", "mrs"},       {"saint", "st"},
        {"street", "st"},       {"mount", "mt"},        {"junior", "jr"},        {"senior", "sr"},        {"professor", "prof"},
        {"versus", "vs"},       {"v", "vs"},            {"etcetera", "etc"},     {"approximately", "approx"},
        {"okay", "ok"},         {"government", "govt"}, {"department", "dept"},  {"avenue", "ave"},       {"road", "rd"},
        {"feet", "ft"},         {"foot", "ft"},         {"kilometers", "km"},    {"kilometres", "km"},    {"kilometer", "km"},
        {"kilometre", "km"},    {"kilograms", "kg"},    {"kilogram", "kg"},      {"minutes", "min"},      {"minute", "min"},
        {"mins", "min"},        {"seconds", "sec"},     {"secs", "sec"},         {"hours", "hr"},         {"hour", "hr"},
        {"hrs", "hr"},          {"television", "tv"},   {"forexample", "eg"},    {"thatis", "ie"},        {"unitedstates", "us"},
        {"unitedkingdom", "uk"}, {"first", "1st"},      {"second", "2nd"},       {"third", "3rd"},        {"fourth", "4th"},
        {"fifth", "5th"},       {"sixth", "6th"},       {"seventh", "7th"},      {"eighth", "8th"},       {"ninth", "9th"},
        {"tenth", "10th"}};
    if (const auto it = kForms.find(joined); it != kForms.end()) {
        return std::string(it->second);
    }
    return joined;
}

/// The same thing said, written another way: a number ("25" / "twenty-five"), an abbreviation ("Dr." / "Doctor"), an
/// ordinal ("1st" / "first") or words run together ("chat GPT" / "ChatGPT").
bool sameSaying(std::span<const Unit> said, std::span<const Unit> script) {
    const auto a = numberValue(said);
    const auto b = numberValue(script);
    if (a || b) {
        return a && b && *a == *b;
    }
    return canonicalForm(said) == canonicalForm(script);
}

/// The script's words may replace what was said: the same saying, or a likely mishearing of it.
bool alike(std::span<const Unit> said, std::span<const Unit> script) {
    return !said.empty() && !script.empty() && (sameSaying(said, script) || likeness(said, script) > kAlike);
}

// ---- Finding the lines ----------------------------------------------------------------------------------------------

/// The transcript's sentences as feature bags, with an inverted index (TextMatch's FeatureBag).
struct SentenceIndex {
    std::vector<double> norms;
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, float>>> postings;

    explicit SentenceIndex(const SpeechSource& source) {
        norms.resize(source.sentences.size(), 0.0);
        for (std::size_t s = 0; s < source.sentences.size(); ++s) {
            const shorts::Sentence& sentence = source.sentences[s];
            const std::size_t begin = source.unitStart[sentence.first];
            const std::size_t end = source.unitStart[sentence.last + 1];
            const FeatureBag bag = featuresOf(std::span<const Unit>(source.units.data() + begin, end - begin));
            double norm = 0.0;
            for (const auto& [feature, count] : bag.features) {
                norm += static_cast<double>(count) * count;
                postings[feature].emplace_back(static_cast<std::uint32_t>(s), count);
            }
            norms[s] = std::sqrt(norm);
        }
    }
};

struct Found {
    std::size_t beat = 0;
    std::size_t unitBegin = 0;  ///< units of the source, [begin, end)
    std::size_t unitEnd = 0;
    double quality = 0.0;
    std::size_t queryBegin = 0; ///< the line's units it says, [begin, end): all of them unless found in parts
    std::size_t queryEnd = 0;
};

struct LineSearch {
    std::vector<Found> takes;  ///< stretches that say the line well enough, best first
    double best = 0.0;         ///< the best quality any stretch reached
    std::size_t bestBegin = 0; ///< and where (units of the source)
    std::size_t bestEnd = 0;
};

/// Every stretch of `source` that says `query` (alignment quality >= `floor`), best first, from the sentences most
/// like it and a few words around them.
LineSearch findLine(const SpeechSource& source, const SentenceIndex& index, std::span<const Unit> query, std::size_t beat,
                    const ScriptCorrectionOptions& options, double floor) {
    LineSearch search;
    const FeatureBag bag = featuresOf(query);
    double queryNorm = 0.0;
    std::unordered_map<std::uint32_t, double> overlap;
    for (const auto& [feature, count] : bag.features) {
        queryNorm += static_cast<double>(count) * count;
        if (const auto it = index.postings.find(feature); it != index.postings.end()) {
            for (const auto& [sentence, weight] : it->second) {
                overlap[sentence] += static_cast<double>(count) * weight;
            }
        }
    }
    queryNorm = std::sqrt(queryNorm);
    std::vector<std::pair<double, std::uint32_t>> ranked;
    for (const auto& [sentence, dot] : overlap) {
        const double norm = index.norms[sentence];
        if (norm > 0.0 && queryNorm > 0.0) {
            ranked.emplace_back(dot / (norm * queryNorm), sentence);
        }
    }
    const std::size_t keep = std::min(ranked.size(), options.candidates);
    std::partial_sort(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(keep), ranked.end(),
                      [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    ranked.resize(keep);
    // Windows of two sentences each side of a seed (a line the recogniser split at a pause, or two lines said as one),
    // merged where they meet.
    std::vector<std::pair<std::size_t, std::size_t>> windows;
    const std::size_t sentences = source.sentences.size();
    for (const auto& seed : ranked) {
        const std::size_t s = seed.second;
        windows.emplace_back(s >= 2 ? s - 2 : 0, std::min<std::size_t>(s + 2, sentences - 1));
    }
    std::ranges::sort(windows);
    std::vector<std::pair<std::size_t, std::size_t>> merged;
    for (const auto& w : windows) {
        if (!merged.empty() && w.first <= merged.back().second + 1) {
            merged.back().second = std::max(merged.back().second, w.second);
        } else {
            merged.push_back(w);
        }
    }
    const std::size_t minUnits = std::max<std::size_t>(2, (query.size() + 1) / 2);
    for (const auto& [s0, s1] : merged) {
        const std::size_t firstWord = source.sentences[s0].first > 3 ? source.sentences[s0].first - 3 : 0;
        const std::size_t lastWord = std::min(source.sentences[s1].last + 3, source.transcript.size() - 1);
        // The stretch, then what is left on either side of each find (a retake said right after the first).
        std::vector<std::pair<std::size_t, std::size_t>> rest{{source.unitStart[firstWord], source.unitStart[lastWord + 1]}};
        while (!rest.empty()) {
            const auto [lo, hi] = rest.back();
            rest.pop_back();
            if (hi <= lo || hi - lo < minUnits) {
                continue;
            }
            const Alignment a = align(query, std::span<const Unit>(source.units.data() + lo, hi - lo));
            if (!a.valid()) {
                continue;
            }
            if (a.quality() > search.best) {
                search.best = a.quality();
                search.bestBegin = lo + a.begin;
                search.bestEnd = lo + a.end;
            }
            if (a.quality() < floor) {
                continue;
            }
            search.takes.push_back(Found{beat, lo + a.begin, lo + a.end, a.quality(), 0, query.size()});
            rest.emplace_back(lo, lo + a.begin);
            rest.emplace_back(lo + a.end, hi);
        }
    }
    std::ranges::sort(search.takes, [](const Found& a, const Found& b) {
        return a.quality != b.quality ? a.quality > b.quality : a.unitBegin < b.unitBegin;
    });
    return search;
}

/// A line not found whole is tried in parts when its best stretch reached this quality (it is there, with a long
/// aside in the middle or half of it skipped).
constexpr double kPartsFloor = 0.3;
constexpr std::size_t kMinPartUnits = 4;

/// The parts a line is tried in: about ten units each, cut between script words, after a comma, colon or dash where
/// one is near; none for a line under eight units.
std::vector<std::pair<std::size_t, std::size_t>> partsOf(std::string_view text, const Line& line) {
    const std::span<const Unit> us(line.units);
    const std::size_t n = us.size();
    std::vector<std::pair<std::size_t, std::size_t>> parts;
    if (n < 2 * kMinPartUnits) {
        return parts;
    }
    const std::size_t count = std::max<std::size_t>(2, (n + 9) / 10);
    const auto punctuated = [&](std::size_t k) {
        const Piece& p = line.pieces[us[k - 1].source];
        const Piece& q = line.pieces[us[k].source];
        const std::string_view tail = coreOf(text.substr(p.begin, p.end - p.begin)).trail;
        const std::string_view gap = text.substr(p.end, q.begin > p.end ? q.begin - p.end : 0);
        const auto has = [&](std::string_view mark) { return tail.find(mark) != std::string_view::npos || gap.find(mark) != std::string_view::npos; };
        return has(",") || has(";") || has(":") || gap.find('-') != std::string_view::npos || has("\xE2\x80\x94") || has("\xE2\x80\x93");
    };
    std::size_t start = 0;
    for (std::size_t k = 1; k < count; ++k) {
        const std::size_t target = n * k / count;
        std::optional<std::size_t> cut;
        std::pair<int, std::size_t> bestScore{2, 0};
        for (std::size_t c = target > 3 ? target - 3 : 1; c <= target + 3 && c < n; ++c) {
            if (c < start + kMinPartUnits || c + kMinPartUnits > n || us[c - 1].source == us[c].source) {
                continue;
            }
            const std::pair<int, std::size_t> score{punctuated(c) ? 0 : 1, c > target ? c - target : target - c};
            if (!cut || score < bestScore) {
                cut = c;
                bestScore = score;
            }
        }
        if (cut) {
            parts.emplace_back(start, *cut);
            start = *cut;
        }
    }
    parts.emplace_back(start, n);
    if (parts.size() < 2) {
        parts.clear();
    }
    return parts;
}

/// The parts of a line found near where the whole line almost was (never elsewhere: a short part alone could match
/// any common phrase). The best stretch may hold only the part said before an aside, so the search reaches a few
/// line-lengths past it.
std::vector<Found> findParts(const SpeechSource& source, std::span<const Unit> query, const std::vector<std::pair<std::size_t, std::size_t>>& parts,
                             std::size_t beat, const LineSearch& whole, double floor) {
    std::vector<Found> out;
    const std::size_t pad = std::max<std::size_t>(12, 3 * query.size());
    const std::size_t lo = whole.bestBegin > pad ? whole.bestBegin - pad : 0;
    const std::size_t hi = std::min(source.units.size(), whole.bestEnd + pad);
    if (hi <= lo) {
        return out;
    }
    const std::span<const Unit> region(source.units.data() + lo, hi - lo);
    for (const auto& [q0, q1] : parts) {
        const Alignment a = align(query.subspan(q0, q1 - q0), region);
        if (a.valid() && a.quality() >= floor) {
            out.push_back(Found{beat, lo + a.begin, lo + a.end, a.quality(), q0, q1});
        }
    }
    return out;
}

// ---- Substitutions --------------------------------------------------------------------------------------------------

/// The media stream and source range of sequence words first..last, when they all come from one clip of a media file
/// (not a nest or a multi-camera clip).
struct WordsSource {
    MediaId media;
    int stream = 0;
    TimeRange source;
};

std::optional<WordsSource> sourceOfWords(const edit::SequenceTranscript& st, const Sequence& sequence, std::size_t first,
                                         std::size_t last) {
    const ClipId clip = st[first].clip;
    const auto loc = sequence.findClip(clip);
    if (!loc || loc->clip().isNested() || !loc->clip().media.isValid()) {
        return std::nullopt;
    }
    Ticks start = st[first].source.start;
    Ticks end = st[first].source.end();
    for (std::size_t i = first; i <= last; ++i) {
        if (st[i].clip != clip) {
            return std::nullopt;
        }
        start = std::min(start, st[i].source.start);
        end = std::max(end, st[i].source.end());
    }
    if (end <= start) {
        end = start + Ticks{1};
    }
    return WordsSource{loc->clip().media, loc->clip().stream.index, TimeRange::fromStartEnd(start, end)};
}

TimeRange timelineOf(const edit::SequenceTranscript& st, std::size_t first, std::size_t last) {
    return TimeRange::fromStartEnd(st[first].timeline.start, std::max(st[first].timeline.start, st[last].timeline.end()));
}

/// A correction about to be made: said units [aBegin, aEnd) of the stretch read script units [bBegin, bEnd).
struct Candidate {
    std::size_t aBegin = 0;
    std::size_t aEnd = 0;
    std::size_t bBegin = 0;
    std::size_t bEnd = 0;
    bool caseOnly = false;
    bool gateWhole = false; ///< told apart from a different word once grown to whole words (Thai clusters)
};

/// The corrections (and the differences left as said) inside one stretch of speech that says part `f` of a line.
void correctStretch(const SpeechSource& source, const Sequence& sequence, std::string_view beatText, const Line& line, const Found& f,
                    const WordSet& proper, const ScriptCorrectionOptions& options, ScriptCorrectionPlan& plan) {
    const edit::SequenceTranscript& st = source.transcript;
    const std::size_t beat = f.beat;
    const std::size_t spanFirst = source.units[f.unitBegin].source;
    const std::size_t spanLast = source.units[f.unitEnd - 1].source;
    const std::size_t offset = source.unitStart[spanFirst];
    const std::span<const Unit> said(source.units.data() + offset, source.unitStart[spanLast + 1] - offset);
    const std::span<const Unit> beatUnits = std::span<const Unit>(line.units).subspan(f.queryBegin, f.queryEnd - f.queryBegin);
    if (said.empty() || beatUnits.empty()) {
        return;
    }
    // Exact unit equality: a near miss ("Somchay" for "Somchai") is exactly what gets corrected.
    const std::vector<DiffRun> runs = diffUnits(said, beatUnits, 1.0);

    // For every unit of either side, the stretch of the other side it lines up with: its partner for an equal unit,
    // the other side of its change for a changed one (an empty stretch at a position for a word only one side has).
    // Growing a correction to whole words and whole script tokens maps through these.
    const std::size_t m = beatUnits.size();
    std::vector<std::pair<std::size_t, std::size_t>> toBeat(said.size());
    std::vector<std::pair<std::size_t, std::size_t>> toSaid(m);
    struct Change {
        std::size_t aBegin, aEnd, bBegin, bEnd;
    };
    std::vector<Change> changes;
    for (std::size_t r = 0; r < runs.size();) {
        if (runs[r].kind == DiffRun::Kind::Equal) {
            for (std::size_t k = 0; k < runs[r].aEnd - runs[r].aBegin; ++k) {
                toBeat[runs[r].aBegin + k] = {runs[r].bBegin + k, runs[r].bBegin + k + 1};
                toSaid[runs[r].bBegin + k] = {runs[r].aBegin + k, runs[r].aBegin + k + 1};
            }
            ++r;
            continue;
        }
        // Consecutive changed runs make one change: what was said there [aBegin, aEnd) for what the script has there.
        // (A run of the other kind is empty on that side, at the change's position.)
        Change c{runs[r].aBegin, runs[r].aBegin, runs[r].bBegin, runs[r].bBegin};
        for (; r < runs.size() && runs[r].kind != DiffRun::Kind::Equal; ++r) {
            if (runs[r].kind == DiffRun::Kind::Deleted) {
                c.aEnd = runs[r].aEnd;
            } else {
                c.bEnd = runs[r].bEnd;
            }
        }
        // Words the speaker added at its edges (a filler before a misheard name) stay as they are: they line up with
        // nothing in the script.
        while (c.aBegin < c.aEnd && said[c.aBegin].filler) {
            toBeat[c.aBegin] = {c.bBegin, c.bBegin};
            ++c.aBegin;
        }
        while (c.aEnd > c.aBegin && said[c.aEnd - 1].filler) {
            toBeat[c.aEnd - 1] = {c.bEnd, c.bEnd};
            --c.aEnd;
        }
        // A change of uneven size whose shorter side matches part of the longer one unit for unit ("Somchay" for
        // "Dr. Somchai", "hone" for "quickly home") is a skipped or added word next to a misheard one: only the matching
        // part is corrected, the rest is left as said.
        const std::size_t saidCount = c.aEnd - c.aBegin;
        const std::size_t scriptCount = c.bEnd - c.bBegin;
        if (saidCount > 0 && scriptCount > 0 && saidCount != scriptCount) {
            const std::size_t k = std::min(saidCount, scriptCount);
            const std::size_t longer = std::max(saidCount, scriptCount);
            std::optional<std::size_t> bestOffset;
            double bestScore = 0.0;
            for (std::size_t o = 0; o + k <= longer; ++o) {
                double score = 0.0;
                bool all = true;
                for (std::size_t i = 0; i < k && all; ++i) {
                    const Unit& a = said[c.aBegin + (saidCount > scriptCount ? o : 0) + i];
                    const Unit& b = beatUnits[c.bBegin + (scriptCount > saidCount ? o : 0) + i];
                    const double s = unitSimilarity(a, b);
                    all = s > 0.0;
                    score += s;
                }
                if (all && score > bestScore) {
                    bestScore = score;
                    bestOffset = o;
                }
            }
            if (bestOffset && saidCount > scriptCount) {
                for (std::size_t a = c.aBegin; a < c.aBegin + *bestOffset; ++a) {
                    toBeat[a] = {c.bBegin, c.bBegin};
                }
                for (std::size_t a = c.aBegin + *bestOffset + k; a < c.aEnd; ++a) {
                    toBeat[a] = {c.bEnd, c.bEnd};
                }
                c.aBegin += *bestOffset;
                c.aEnd = c.aBegin + k;
            } else if (bestOffset) {
                for (std::size_t b = c.bBegin; b < c.bBegin + *bestOffset; ++b) {
                    toSaid[b] = {c.aBegin, c.aBegin};
                }
                for (std::size_t b = c.bBegin + *bestOffset + k; b < c.bEnd; ++b) {
                    toSaid[b] = {c.aEnd, c.aEnd};
                }
                c.bBegin += *bestOffset;
                c.bEnd = c.bBegin + k;
            }
        }
        for (std::size_t a = c.aBegin; a < c.aEnd; ++a) {
            toBeat[a] = {c.bBegin, c.bEnd};
        }
        for (std::size_t b = c.bBegin; b < c.bEnd; ++b) {
            toSaid[b] = {c.aBegin, c.aEnd};
        }
        changes.push_back(c);
    }

    const auto scriptText = [&](std::size_t bBegin, std::size_t bEnd) {
        const Piece& p0 = line.pieces[beatUnits[bBegin].source];
        const Piece& p1 = line.pieces[beatUnits[bEnd - 1].source];
        return std::string(beatText.substr(p0.begin, p1.end - p0.begin));
    };
    const auto keepAsSaid = [&](std::size_t aBegin, std::size_t aEnd, std::size_t bBegin, std::size_t bEnd) {
        ScriptDifference d;
        d.beat = beat;
        d.first = said[aBegin].source;
        d.last = said[aEnd - 1].source;
        d.timeline = timelineOf(st, d.first, d.last);
        d.said = st.textOf(d.first, d.last);
        d.script = scriptText(bBegin, bEnd);
        plan.kept.push_back(std::move(d));
    };

    // Only substitutions (a word skipped - nothing said - or added - nothing in the script - is left alone), and only
    // those that can be a mishearing of the script; the rest is what the speaker said instead.
    std::vector<Candidate> candidates;
    for (const Change& c : changes) {
        const std::size_t saidCount = c.aEnd - c.aBegin;
        const std::size_t scriptCount = c.bEnd - c.bBegin;
        if (saidCount == 0 || scriptCount == 0) {
            continue;
        }
        const std::span<const Unit> saidPart = said.subspan(c.aBegin, saidCount);
        const std::span<const Unit> scriptPart = beatUnits.subspan(c.bBegin, scriptCount);
        const bool words = std::ranges::all_of(saidPart, &Unit::word) && std::ranges::all_of(scriptPart, &Unit::word);
        if (words && saidCount == scriptCount && saidCount > 1) {
            // Word for word, each pair on its own: "great Somchay" for "good Somchai" fixes the name, keeps "great".
            std::vector<bool> ok(saidCount);
            bool any = false;
            for (std::size_t k = 0; k < saidCount; ++k) {
                ok[k] = alike(saidPart.subspan(k, 1), scriptPart.subspan(k, 1));
                any = any || ok[k];
            }
            if (any) {
                for (std::size_t k = 0; k < saidCount; ++k) {
                    toBeat[c.aBegin + k] = {c.bBegin + k, c.bBegin + k + 1};
                    toSaid[c.bBegin + k] = {c.aBegin + k, c.aBegin + k + 1};
                }
                for (std::size_t k = 0; k < saidCount;) {
                    std::size_t e = k + 1;
                    while (e < saidCount && ok[e] == ok[k]) {
                        ++e;
                    }
                    if (ok[k]) {
                        candidates.push_back(Candidate{c.aBegin + k, c.aBegin + e, c.bBegin + k, c.bBegin + e, false, false});
                    } else {
                        keepAsSaid(c.aBegin + k, c.aBegin + e, c.bBegin + k, c.bBegin + e);
                    }
                    k = e;
                }
                continue;
            }
        }
        if (!words) {
            // Character clusters (Thai): one cluster says little; the whole words they grow to decide.
            candidates.push_back(Candidate{c.aBegin, c.aEnd, c.bBegin, c.bEnd, false, true});
        } else if (alike(saidPart, scriptPart)) {
            candidates.push_back(Candidate{c.aBegin, c.aEnd, c.bBegin, c.bEnd, false, false});
        } else {
            keepAsSaid(c.aBegin, c.aEnd, c.bBegin, c.bEnd);
        }
    }

    // The same word in other capitals: a name or term gets the script's when they mean something there.
    if (options.fixCase && !line.shouting) {
        for (const DiffRun& r : runs) {
            if (r.kind != DiffRun::Kind::Equal) {
                continue;
            }
            for (std::size_t k = 0; k < r.aEnd - r.aBegin; ++k) {
                const std::size_t a = r.aBegin + k;
                const std::size_t b = r.bBegin + k;
                const std::size_t word = said[a].source;
                const std::uint32_t piece = beatUnits[b].source;
                if (!said[a].word || st[word].redacted || source.unitStart[word + 1] - source.unitStart[word] != 1 ||
                    line.unitsOfPiece[piece] != 1) {
                    continue;
                }
                const Piece& p = line.pieces[piece];
                const Core script = coreOf(beatText.substr(p.begin, p.end - p.begin));
                const Core heard = coreOf(st.text(word));
                if (script.word.empty() || heard.word == script.word || asciiLower(heard.word) != asciiLower(script.word)) {
                    continue;
                }
                if (meaningfulCapitals(script.word, startsSentence(beatText, p.begin), proper.contains(asciiLower(script.word)))) {
                    candidates.push_back(Candidate{a, a + 1, b, b + 1, true, false});
                }
            }
        }
    }

    std::ranges::sort(candidates, [](const Candidate& x, const Candidate& y) { return x.aBegin < y.aBegin; });
    std::size_t consumedUpTo = 0; // said units already in a correction
    for (const Candidate& c : candidates) {
        if (c.aBegin < consumedUpTo) {
            continue;
        }
        std::size_t aB = c.aBegin, aE = c.aEnd, bB = c.bBegin, bE = c.bEnd;
        if (!c.caseOnly) {
            // Grow to whole transcript words and whole script tokens (Thai clusters, "state-of-the-art"), each side
            // following the other, until both stop growing.
            for (int pass = 0; pass < 16; ++pass) {
                const std::size_t wordB = said[aB].source;
                const std::size_t wordE = said[aE - 1].source;
                std::size_t naB = std::min(aB, source.unitStart[wordB] - offset);
                std::size_t naE = std::max(aE, source.unitStart[wordE + 1] - offset);
                std::size_t nbB = std::min(bB, toBeat[naB].first);
                std::size_t nbE = std::max(bE, toBeat[naE - 1].second);
                if (nbE <= nbB) {
                    break;
                }
                const std::size_t pieceB = beatUnits[nbB].source;
                const std::size_t pieceE = beatUnits[nbE - 1].source;
                while (nbB > 0 && beatUnits[nbB - 1].source == pieceB) {
                    --nbB;
                }
                while (nbE < m && beatUnits[nbE].source == pieceE) {
                    ++nbE;
                }
                naB = std::min(naB, toSaid[nbB].first);
                naE = std::max(naE, toSaid[nbE - 1].second);
                if (naB == aB && naE == aE && nbB == bB && nbE == bE) {
                    break;
                }
                aB = naB;
                aE = naE;
                bB = nbB;
                bE = nbE;
            }
            if (aB < consumedUpTo) {
                continue; // it grew into the previous correction's words
            }
            if (c.gateWhole && !alike(said.subspan(aB, aE - aB), beatUnits.subspan(bB, bE - bB))) {
                keepAsSaid(aB, aE, bB, bE);
                continue;
            }
        }
        consumedUpTo = aE;
        const std::size_t first = said[aB].source;
        const std::size_t last = said[aE - 1].source;
        ScriptCorrection fix;
        fix.beat = beat;
        fix.first = first;
        fix.last = last;
        fix.before = st.textOf(first, last);
        if (c.caseOnly) {
            // The transcript's own punctuation around the word stays; only its letters take the script's capitals.
            const Piece& p = line.pieces[beatUnits[bB].source];
            const Core script = coreOf(beatText.substr(p.begin, p.end - p.begin));
            const Core heard = coreOf(st.text(first));
            fix.after = std::string(heard.lead) + std::string(script.word) + std::string(heard.trail);
            fix.caseOnly = true;
        } else {
            fix.after = scriptText(bB, bE);
        }
        if (fix.before == fix.after) {
            continue;
        }
        bool redacted = false;
        for (std::size_t i = first; i <= last; ++i) {
            redacted = redacted || st[i].redacted;
        }
        if (redacted) {
            ++plan.redacted;
            continue;
        }
        const auto from = sourceOfWords(st, sequence, first, last);
        if (!from) {
            ++plan.crossClip;
            continue;
        }
        fix.timeline = timelineOf(st, first, last);
        fix.media = from->media;
        fix.audioStream = from->stream;
        fix.source = from->source;
        plan.corrections.push_back(std::move(fix));
    }
}

} // namespace

bool soundsAlike(std::string_view said, std::string_view script) {
    const std::vector<Unit> a = units(said);
    const std::vector<Unit> b = units(script);
    return alike(a, b);
}

ScriptCorrectionPlan planScriptCorrections(const SpeechSource& source, const Sequence& sequence, const Script& script,
                                           const ScriptCorrectionOptions& options) {
    ScriptCorrectionPlan plan;
    plan.beats = script.beats.size();
    if (source.transcript.empty() || source.sentences.empty()) {
        for (std::size_t b = 0; b < script.beats.size(); ++b) {
            plan.skipped.push_back(b);
            plan.skippedQuality.push_back(0.0);
        }
        return plan;
    }
    const SentenceIndex index(source);
    std::vector<Line> lines;
    lines.reserve(script.beats.size());
    for (const ScriptBeat& beat : script.beats) {
        lines.push_back(lineOf(beat.text));
    }
    const WordSet proper = properWords(script, lines);
    std::vector<Found> found;
    for (std::size_t b = 0; b < script.beats.size(); ++b) {
        const Line& line = lines[b];
        LineSearch search;
        if (!line.units.empty()) {
            search = findLine(source, index, line.units, b, options, options.minMatch);
        }
        if (!search.takes.empty()) {
            ++plan.beatsMatched;
            // Retakes of the line: those about as good as its best one (an ad-lib that merely resembles it is not).
            const double floor = std::max(options.minMatch, search.takes.front().quality - options.retakeMargin);
            for (const Found& f : search.takes) {
                if (f.quality >= floor) {
                    found.push_back(f);
                }
            }
        } else {
            // Almost found: a long aside in the middle, or half of it skipped. Its parts, where it almost was.
            std::vector<Found> parts;
            if (search.best >= kPartsFloor && search.bestEnd > search.bestBegin) {
                parts = findParts(source, line.units, partsOf(script.beats[b].text, line), b, search, options.minMatch);
            }
            if (!parts.empty()) {
                ++plan.beatsMatched;
                ++plan.beatsPartial;
                found.insert(found.end(), parts.begin(), parts.end());
            } else {
                plan.skipped.push_back(b);
                plan.skippedQuality.push_back(std::round(search.best * 100.0) / 100.0);
            }
        }
        if (options.progress) {
            options.progress(b + 1, script.beats.size());
        }
    }
    // Every stretch of speech is corrected from one line at most: the best match claims it.
    std::ranges::stable_sort(found, [](const Found& a, const Found& b) {
        return a.quality != b.quality ? a.quality > b.quality : a.unitBegin < b.unitBegin;
    });
    std::map<std::size_t, std::size_t> claimed; // first word -> last word, disjoint
    for (const Found& f : found) {
        const std::size_t first = source.units[f.unitBegin].source;
        const std::size_t last = source.units[f.unitEnd - 1].source;
        if (auto it = claimed.upper_bound(last); it != claimed.begin() && std::prev(it)->second >= first) {
            continue;
        }
        claimed.emplace(first, last);
        correctStretch(source, sequence, script.beats[f.beat].text, lines[f.beat], f, proper, options, plan);
    }
    std::ranges::sort(plan.corrections, [](const ScriptCorrection& a, const ScriptCorrection& b) { return a.first < b.first; });
    // The same words of a media file heard twice in the sequence (a clip used again) are corrected once.
    std::set<std::tuple<std::uint64_t, int, std::int64_t, std::int64_t>> seen;
    std::erase_if(plan.corrections, [&](const ScriptCorrection& c) {
        return !seen.emplace(c.media.value, c.audioStream, c.source.start.value, c.source.duration.value).second;
    });
    std::ranges::sort(plan.kept, [](const ScriptDifference& a, const ScriptDifference& b) { return a.first < b.first; });
    if (!plan.skipped.empty()) {
        plan.warnings.push_back(std::to_string(plan.skipped.size()) + " of " + std::to_string(plan.beats) +
                                " lines were not found in the transcript closely enough to correct from");
    }
    if (!plan.kept.empty()) {
        plan.warnings.push_back(std::to_string(plan.kept.size()) +
                                " differences were left as said: the words do not sound like the script's (the speaker changed them)");
    }
    if (plan.crossClip > 0) {
        plan.warnings.push_back(std::to_string(plan.crossClip) +
                                " corrections were left out: their words cross a cut or are heard through a nested clip");
    }
    if (plan.redacted > 0) {
        plan.warnings.push_back(std::to_string(plan.redacted) + " corrections were left out: their words are redacted");
    }
    return plan;
}

namespace {

constexpr std::size_t kMaxListed = 200;
constexpr std::size_t kMaxListedOther = 50;
constexpr std::size_t kMaxLines = 4000; ///< a 10,000-word script has about 700

Ticks middleOf(const TranscriptWord& w) { return w.t0 + Ticks{(w.t1 - w.t0).value / 2}; }

/// The correction to store for `fix`: itself, or - where it overlaps corrections made before (by hand, or an earlier
/// run) - one over all of them that keeps their words and changes only fix's (correctTranscript replaces the
/// corrections a new one overlaps). nullopt when the media's transcript is not there.
std::optional<WordCorrection> storedCorrection(const ProjectState& state, const edit::TranscriptProvider& provider,
                                               const ScriptCorrection& fix) {
    WordCorrection plain{fix.source, fix.before, fix.after};
    const TranscriptEdits* edits = state.findTranscriptEdits(fix.media, fix.audioStream);
    std::vector<const WordCorrection*> overlapping;
    if (edits) {
        for (const WordCorrection& c : edits->corrections) {
            if (c.source.overlaps(fix.source)) {
                overlapping.push_back(&c);
            }
        }
    }
    if (overlapping.empty()) {
        return plain;
    }
    const MediaItem* item = state.findMedia(fix.media);
    const std::shared_ptr<const Transcript> raw = item ? provider.transcript(*item, fix.audioStream) : nullptr;
    if (!raw) {
        return std::nullopt;
    }
    Ticks start = fix.source.start;
    Ticks end = fix.source.end();
    for (const WordCorrection* c : overlapping) {
        start = std::min(start, c->source.start);
        end = std::max(end, c->source.end());
    }
    const TimeRange all = TimeRange::fromStartEnd(start, end);
    // What the words read now, fix's replaced.
    const Transcript now = edit::applyCorrections(*raw, *edits);
    std::vector<std::string> text;
    bool placed = false;
    for (const TranscriptWord& w : now.words) {
        const Ticks mid = middleOf(w);
        if (!all.contains(mid)) {
            continue;
        }
        if (fix.source.contains(mid)) {
            if (!placed) {
                text.push_back(fix.after);
                placed = true;
            }
            continue;
        }
        text.push_back(w.text);
    }
    if (!placed) {
        text.push_back(fix.after);
    }
    // What was transcribed there: the earlier corrections' originals and the words between them.
    std::vector<std::string> original;
    std::vector<bool> used(overlapping.size(), false);
    for (const TranscriptWord& w : raw->words) {
        const Ticks mid = middleOf(w);
        if (!all.contains(mid)) {
            continue;
        }
        bool inside = false;
        for (std::size_t k = 0; k < overlapping.size() && !inside; ++k) {
            if (overlapping[k]->source.contains(mid)) {
                inside = true;
                if (!used[k]) {
                    original.push_back(overlapping[k]->original);
                    used[k] = true;
                }
            }
        }
        if (!inside) {
            original.push_back(w.text);
        }
    }
    for (std::size_t k = 0; k < overlapping.size(); ++k) {
        if (!used[k] && !overlapping[k]->original.empty()) {
            original.push_back(overlapping[k]->original); // its words are gone from the transcript (restored text)
        }
    }
    return WordCorrection{all, edit::joinWords(original, raw->language), edit::joinWords(text, raw->language)};
}

CommandOutput correctFromScript(ExecutionContext& c) {
    auto* services = command::findService<edit::TranscriptServices>(c.services());
    if (!services || !services->transcripts) {
        fail(ErrorCode::InvalidRequest, "transcripts are not available here: the host provides none");
    }
    ScriptOptions scriptOptions;
    const std::string split = c.string("split", "auto");
    scriptOptions.split = split == "lines" ? ScriptSplit::Lines : split == "sentences" ? ScriptSplit::Sentences : ScriptSplit::Auto;
    scriptOptions.maxBeats = kMaxLines;
    auto parsed = parseScript(c.string("text"), scriptOptions);
    if (!parsed) {
        fail(ErrorCode::InvalidParams, parsed.error().message, Json{{"path", "/text"}});
    }
    const Script& script = *parsed;
    if (script.beats.empty()) {
        fail(ErrorCode::InvalidParams, "the script has no lines to correct from", Json{{"path", "/text"}});
    }
    const Sequence& sequence = c.sequence();
    c.checkpoint("reading the transcript");
    SpeechCorpus corpus;
    addSequenceSource(corpus, c.state(), sequence.id, *services->transcripts, nullptr);
    if (corpus.sources.empty() || corpus.sources.front().transcript.empty()) {
        fail(ErrorCode::EditRejected, "sequence '" + sequence.name + "' has no transcript: transcribe it first");
    }
    ScriptCorrectionOptions options;
    if (c.has("minMatch")) {
        options.minMatch = c.number("minMatch");
    }
    options.fixCase = c.boolean("fixCase", true);
    // Progress in whole percents, which is also where a cancel stops it.
    std::size_t reported = 101;
    options.progress = [&c, &reported](std::size_t done, std::size_t total) {
        const std::size_t percent = total > 0 ? done * 100 / total : 100;
        if (percent != reported) {
            reported = percent;
            c.checkpoint("finding your lines in the transcript (" + std::to_string(percent) + "%)");
        }
    };
    c.checkpoint("finding your lines in the transcript (0%)");
    const SpeechSource& source = corpus.sources.front();
    const Sequence* mapped = corpus.state->findSequence(sequence.id);
    const ScriptCorrectionPlan plan = planScriptCorrections(source, mapped ? *mapped : sequence, script, options);

    c.checkpoint("correcting the transcript");
    ProjectState state = c.state();
    Json changes = Json::array();
    std::size_t corrected = 0;
    std::size_t caseOnly = 0;
    for (const ScriptCorrection& fix : plan.corrections) {
        const std::optional<WordCorrection> stored = storedCorrection(state, *services->transcripts, fix);
        if (!stored) {
            continue;
        }
        state = c.take(::wabcut::edit::correctTranscript(state, fix.media, fix.audioStream, *stored));
        ++corrected;
        caseOnly += fix.caseOnly ? 1 : 0;
        if (changes.size() < kMaxListed) {
            changes.push_back(Json{{"words", Json::array({fix.first, fix.last})},
                                   {"time", c.timecode(fix.timeline.start)},
                                   {"ticks", fix.timeline.start.value},
                                   {"line", fix.beat + 1},
                                   {"before", fix.before},
                                   {"after", fix.after},
                                   {"kind", fix.caseOnly ? "case" : "words"}});
        }
    }
    Json skipped = Json::array();
    for (std::size_t k = 0; k < plan.skipped.size() && k < kMaxListedOther; ++k) {
        const std::size_t b = plan.skipped[k];
        skipped.push_back(Json{{"line", b + 1}, {"text", script.beats[b].text}, {"quality", plan.skippedQuality[k]}});
    }
    Json kept = Json::array();
    for (std::size_t k = 0; k < plan.kept.size() && k < kMaxListedOther; ++k) {
        const ScriptDifference& d = plan.kept[k];
        kept.push_back(Json{{"words", Json::array({d.first, d.last})},
                            {"time", c.timecode(d.timeline.start)},
                            {"ticks", d.timeline.start.value},
                            {"line", d.beat + 1},
                            {"said", d.said},
                            {"script", d.script}});
    }
    CommandOutput out;
    out.state = std::move(state);
    out.data = Json{{"corrected", corrected},
                    {"caseFixes", caseOnly},
                    {"beats", plan.beats},
                    {"beatsMatched", plan.beatsMatched},
                    {"beatsPartial", plan.beatsPartial},
                    {"beatsSkipped", plan.skipped.size()},
                    {"keptCount", plan.kept.size()},
                    {"leftOut", plan.crossClip + plan.redacted},
                    {"words", corpus.words},
                    {"changes", std::move(changes)},
                    {"skipped", std::move(skipped)},
                    {"kept", std::move(kept)}};
    out.warnings = plan.warnings;
    out.warnings.insert(out.warnings.end(), script.warnings.begin(), script.warnings.end());
    return out;
}

} // namespace

void registerScriptCorrectionCommands(command::CommandRegistry& registry) {
    Json params{
        {"type", "object"},
        {"properties",
         Json{{"text", Json{{"type", "string"}, {"description", "The script as it should read (plain text or Markdown)."}, {"minLength", 1}, {"maxLength", 262144}}},
              {"sequence", schema::integer("The sequence whose transcript is corrected; only needed when the project has several.", 1)},
              {"split", schema::enumeration({"auto", "lines", "sentences"}, "Lines of the script: \"auto\" (bullets and short lines whole, prose per sentence; default), every line, or every sentence.")},
              {"minMatch", schema::number("How closely what was said must follow a line before its words are corrected, 0.3..0.99 (default 0.7).", 0.3, 0.99)},
              {"fixCase", schema::boolean("Also give names and terms the script's capitals where only those differ (\"kubernetes\" -> \"Kubernetes\"; default true).")}}},
        {"required", Json::array({"text"})},
        {"additionalProperties", false}};
    [[maybe_unused]] const auto added = registry.add(
        {"transcript.correctFromScript", 1, "Correct from Script",
         "Corrects the sequence's transcript from the script it was read from: each line is found in what was said "
         "(fuzzy, Thai too; a line with a long aside is found in parts), and where the speech follows the line, the "
         "words the recogniser got wrong take the script's spelling, casing and punctuation (names, jargon, numbers "
         "written another way). Only substitutions that sound like the script: a word the speaker skipped is never "
         "added, a word they added never removed, a word they changed (\"great\" for \"good\") is kept and listed; "
         "lines that do not match closely enough are skipped and listed. The corrections are saved with the project like "
         "transcript.correctRange (merged with corrections made before), so captions follow on "
         "captions.updateFromTranscript. One undo step; dryRun lists the changes (at most 200) without making them.",
         command::Capability::EditProject, std::move(params),
         Json{{"text", "Today we test Wabcut with Dr. Somchai on Kubernetes."}}, {}, correctFromScript});
}

} // namespace wabcut::ai::roughcut
