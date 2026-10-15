#include <wabcut/ai/roughcut/TextMatch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace wabcut::ai::roughcut {

namespace {

/// Decodes the code point at s[i] and advances i; malformed bytes decode as U+FFFD one byte at a time.
char32_t decode(std::string_view s, std::size_t& i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char b0 = byte(i);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    int length = 0;
    char32_t c = 0;
    if ((b0 & 0xE0) == 0xC0) {
        length = 2;
        c = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        length = 3;
        c = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        length = 4;
        c = b0 & 0x07u;
    } else {
        ++i;
        return 0xFFFD;
    }
    if (i + static_cast<std::size_t>(length) > s.size()) {
        ++i;
        return 0xFFFD;
    }
    for (int k = 1; k < length; ++k) {
        const unsigned char b = byte(i + static_cast<std::size_t>(k));
        if ((b & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        c = (c << 6) | (b & 0x3Fu);
    }
    i += static_cast<std::size_t>(length);
    return c;
}

void encode(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

std::u32string toCodePoints(std::string_view s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size();) {
        out += decode(s, i);
    }
    return out;
}

bool in(char32_t c, char32_t lo, char32_t hi) { return c >= lo && c <= hi; }

/// Marks that attach to the character before them (they never start a unit).
bool isCombining(char32_t c) {
    return in(c, 0x0300, 0x036F) ||                                                 // combining diacritics
           c == 0x0E31 || in(c, 0x0E34, 0x0E3A) || in(c, 0x0E47, 0x0E4E) ||         // Thai above/below vowels, tones
           c == 0x0EB1 || in(c, 0x0EB4, 0x0EBC) || in(c, 0x0EC8, 0x0ECD) ||         // Lao
           in(c, 0x17B4, 0x17D3) ||                                                // Khmer dependent signs
           in(c, 0x102B, 0x103E) || in(c, 0x1056, 0x1059) || in(c, 0x105E, 0x1060) || // Myanmar
           in(c, 0x0900, 0x0903) || in(c, 0x093A, 0x094F) || in(c, 0x0951, 0x0957) || in(c, 0x0962, 0x0963) ||
           in(c, 0x3099, 0x309A) || in(c, 0xFE00, 0xFE0F) || c == 0x200D;          // kana voicing, variation, ZWJ
}

bool isSeparator(char32_t c) {
    if (c < 0x80) {
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        return !alnum && c != '\'';
    }
    return in(c, 0x0080, 0x00BF) || c == 0x00D7 || c == 0x00F7 || in(c, 0x2000, 0x206F) || in(c, 0x20A0, 0x2BFF) ||
           in(c, 0x3000, 0x303F) || in(c, 0xFF00, 0xFF0F) || in(c, 0xFF1A, 0xFF20) || in(c, 0xFF3B, 0xFF40) ||
           in(c, 0xFF5B, 0xFF65) || c == 0x0E2F || c == 0x0E46 || c == 0x0E4F || c == 0x0E5A || c == 0x0E5B ||
           c == 0xFEFF || c == 0xFFFD || c >= 0x1F000;
}

bool isApostrophe(char32_t c) { return c == '\'' || c == 0x2019 || c == 0x02BC; }

/// Lowercase for the scripts transcripts use most; digits of other scripts as ASCII.
char32_t fold(char32_t c) {
    if (c >= 'A' && c <= 'Z') {
        return c + 32;
    }
    if (c < 0x80) {
        return c;
    }
    if (in(c, 0x00C0, 0x00DE) && c != 0x00D7) {
        return c + 0x20;
    }
    if (in(c, 0x0100, 0x017F)) {
        return (c % 2 == 0) ? c + 1 : c; // Latin Extended-A pairs (close enough for matching)
    }
    if (in(c, 0x0391, 0x03A9)) {
        return c + 0x20;
    }
    if (in(c, 0x0410, 0x042F)) {
        return c + 0x20;
    }
    if (in(c, 0x0400, 0x040F)) {
        return c + 0x50;
    }
    if (in(c, 0x0E50, 0x0E59)) { // Thai digits
        return U'0' + (c - 0x0E50);
    }
    if (in(c, 0x0ED0, 0x0ED9)) { // Lao digits
        return U'0' + (c - 0x0ED0);
    }
    if (in(c, 0xFF10, 0xFF19)) {
        return U'0' + (c - 0xFF10);
    }
    if (in(c, 0xFF21, 0xFF3A)) {
        return U'a' + (c - 0xFF21);
    }
    if (in(c, 0xFF41, 0xFF5A)) {
        return U'a' + (c - 0xFF41);
    }
    return c;
}

/// English number words as digits, so "ten" and "10" match.
std::string_view numberWord(std::string_view word) {
    static const std::map<std::string_view, std::string_view> kNumbers{
        {"zero", "0"},     {"one", "1"},       {"two", "2"},       {"three", "3"},     {"four", "4"},     {"five", "5"},
        {"six", "6"},      {"seven", "7"},     {"eight", "8"},     {"nine", "9"},      {"ten", "10"},     {"eleven", "11"},
        {"twelve", "12"},  {"thirteen", "13"}, {"fourteen", "14"}, {"fifteen", "15"},  {"sixteen", "16"}, {"seventeen", "17"},
        {"eighteen", "18"}, {"nineteen", "19"}, {"twenty", "20"},  {"thirty", "30"},   {"forty", "40"},   {"fifty", "50"},
        {"sixty", "60"},   {"seventy", "70"},  {"eighty", "80"},   {"ninety", "90"},   {"hundred", "100"}};
    const auto it = kNumbers.find(word);
    return it == kNumbers.end() ? word : it->second;
}

/// Levenshtein distance over code points (two rows).
std::size_t levenshtein(const std::u32string& a, const std::u32string& b) {
    std::vector<std::size_t> prev(b.size() + 1);
    std::vector<std::size_t> cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        prev[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t substitute = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({substitute, prev[j] + 1, cur[j - 1] + 1});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

bool allDigits(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char ch) { return ch >= '0' && ch <= '9'; });
}

} // namespace

bool isUnspacedCodePoint(char32_t c) {
    return in(c, 0x0E00, 0x0E7F) ||  // Thai
           in(c, 0x0E80, 0x0EFF) ||  // Lao
           in(c, 0x1000, 0x109F) ||  // Myanmar
           in(c, 0x1780, 0x17FF) ||  // Khmer
           in(c, 0x3040, 0x30FF) ||  // Hiragana, Katakana
           in(c, 0x3400, 0x4DBF) ||  // CJK Extension A
           in(c, 0x4E00, 0x9FFF) ||  // CJK Unified Ideographs
           in(c, 0xF900, 0xFAFF) ||  // CJK compatibility
           in(c, 0x20000, 0x2FA1F);  // CJK Extensions B..
}

namespace {

/// English contractions as the words they stand for, so "we're" matches "we are" and "don't" matches "do not".
/// `apostrophe` is where the apostrophe was in `word` (it is not in it). Possessives ("john's") stay one word.
std::vector<std::string> expandContraction(const std::string& word, std::size_t apostrophe) {
    const std::string base = word.substr(0, apostrophe);
    const std::string tail = word.substr(apostrophe);
    static const std::map<std::string, std::string, std::less<>> suffixes{{"re", "are"}, {"ve", "have"}, {"ll", "will"}, {"m", "am"}, {"d", "would"}};
    if (base.empty() || tail.empty()) {
        return {word};
    }
    if (const auto it = suffixes.find(tail); it != suffixes.end()) {
        return {base, it->second};
    }
    if (tail == "s") {
        static const std::set<std::string, std::less<>> pronouns{"it", "that", "what", "there", "here", "he", "she", "who", "where", "how"};
        if (base == "let") {
            return {"let", "us"};
        }
        return pronouns.contains(base) ? std::vector<std::string>{base, "is"} : std::vector<std::string>{word};
    }
    if (tail == "t" && base.size() >= 2 && base.back() == 'n') {
        if (base == "can") {
            return {"can", "not"};
        }
        if (base == "won") {
            return {"will", "not"};
        }
        if (base == "shan") {
            return {"shall", "not"};
        }
        return {base.substr(0, base.size() - 1), "not"};
    }
    return {word};
}

} // namespace

void appendUnits(std::vector<Unit>& out, std::string_view text, std::uint32_t source, bool filler) {
    std::string word;
    std::optional<std::size_t> apostrophe;
    bool clusterOpen = false; // the last unit is a cluster that can still take combining marks
    const auto flushWord = [&] {
        if (!word.empty()) {
            for (const std::string& part : apostrophe ? expandContraction(word, *apostrophe) : std::vector<std::string>{word}) {
                out.push_back(Unit{std::string(numberWord(part)), source, true, filler});
            }
            word.clear();
        }
        apostrophe.reset();
    };
    for (std::size_t i = 0; i < text.size();) {
        const char32_t raw = decode(text, i);
        const char32_t c = fold(raw);
        if (isUnspacedCodePoint(raw) && !(c >= '0' && c <= '9')) {
            if (isCombining(raw)) {
                if (clusterOpen && !out.empty()) {
                    encode(out.back().text, c);
                }
                continue; // a stray mark with nothing to attach to is dropped
            }
            if (raw == 0x0E2F || raw == 0x0E46 || raw == 0x0E4F || raw == 0x0E5A || raw == 0x0E5B) {
                flushWord(); // Thai punctuation (paiyannoi, mai yamok, ...)
                clusterOpen = false;
                continue;
            }
            flushWord();
            std::string cluster;
            encode(cluster, c);
            out.push_back(Unit{std::move(cluster), source, false, filler});
            clusterOpen = true;
            continue;
        }
        if (isCombining(raw)) {
            if (!word.empty()) {
                encode(word, c); // a diacritic on a word of a spaced script
            } else if (clusterOpen && !out.empty()) {
                encode(out.back().text, c);
            }
            continue;
        }
        clusterOpen = false;
        if (isApostrophe(raw)) {
            if (!word.empty() && !apostrophe) {
                apostrophe = word.size(); // "we're" -> "we" "are"; "john's" -> "johns"; quotes around a word vanish
            }
            continue;
        }
        if (isSeparator(raw)) {
            flushWord();
            continue;
        }
        encode(word, c);
    }
    flushWord();
}

std::vector<Unit> units(std::string_view text, std::uint32_t source) {
    std::vector<Unit> out;
    appendUnits(out, text, source);
    return out;
}

std::string normalizedText(std::string_view text) {
    const std::vector<Unit> us = units(text);
    std::string out;
    bool previousWord = false;
    for (const Unit& u : us) {
        if (!out.empty() && (u.word || previousWord)) {
            out += ' ';
        }
        out += u.text;
        previousWord = u.word;
    }
    return out;
}

double unitSimilarity(const Unit& a, const Unit& b) {
    if (a.text == b.text) {
        return 1.0;
    }
    if (a.word != b.word) {
        return 0.0;
    }
    if (!a.word) {
        // Clusters: the same base character with other marks (a tone mark or vowel missed or added).
        std::size_t i = 0;
        std::size_t k = 0;
        const char32_t baseA = a.text.empty() ? 0 : decode(a.text, i);
        const char32_t baseB = b.text.empty() ? 0 : decode(b.text, k);
        return baseA != 0 && baseA == baseB ? 0.6 : 0.0;
    }
    if (allDigits(a.text) || allDigits(b.text)) {
        return 0.0;
    }
    const std::u32string x = toCodePoints(a.text);
    const std::u32string y = toCodePoints(b.text);
    const std::size_t longest = std::max(x.size(), y.size());
    const std::size_t shortest = std::min(x.size(), y.size());
    if (longest < 4) {
        return 0.0;
    }
    // A stem of 4+ letters: "edit" / "editing", "launch" / "launched".
    const bool prefix = shortest >= 4 && (x.compare(0, shortest, y, 0, shortest) == 0);
    double ratio = 0.0;
    if (longest - shortest <= longest / 4) {
        ratio = 1.0 - static_cast<double>(levenshtein(x, y)) / static_cast<double>(longest);
    }
    if (ratio >= 0.75) {
        return ratio;
    }
    return prefix ? 0.75 : 0.0;
}

Alignment align(std::span<const Unit> query, std::span<const Unit> target, const AlignOptions& options) {
    Alignment best;
    const std::size_t m = query.size();
    const std::size_t n = target.size();
    if (m == 0 || n == 0) {
        return best;
    }
    struct Cell {
        double score = 0.0;
        double matched = 0.0;
        std::uint32_t queryMatched = 0;
        std::uint32_t start = 0;
    };
    std::vector<Cell> prev(n + 1);
    std::vector<Cell> cur(n + 1);
    for (std::size_t j = 0; j <= n; ++j) {
        prev[j] = Cell{0.0, 0.0, 0, static_cast<std::uint32_t>(j)}; // the query may start anywhere in the target
    }
    for (std::size_t i = 1; i <= m; ++i) {
        cur[0] = Cell{prev[0].score + options.skipQuery, prev[0].matched, prev[0].queryMatched, 0};
        const Unit& q = query[i - 1];
        for (std::size_t j = 1; j <= n; ++j) {
            const Unit& t = target[j - 1];
            const double sim = unitSimilarity(q, t);
            const bool matches = sim >= options.minSimilarity;
            Cell diagonal = prev[j - 1];
            diagonal.score += matches ? options.match * sim : options.mismatch;
            if (matches) {
                diagonal.matched += sim;
                ++diagonal.queryMatched;
            }
            Cell up = prev[j];
            up.score += options.skipQuery;
            Cell left = cur[j - 1];
            left.score += t.filler ? 0.0 : options.skipTarget;
            Cell chosen = diagonal;
            if (up.score > chosen.score) {
                chosen = up;
            }
            if (left.score > chosen.score) {
                chosen = left;
            }
            cur[j] = chosen;
        }
        std::swap(prev, cur);
    }
    std::size_t endAt = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (std::size_t j = 1; j <= n; ++j) {
        if (prev[j].score > bestScore) {
            bestScore = prev[j].score;
            endAt = j;
        }
    }
    const Cell& cell = prev[endAt];
    std::size_t begin = cell.start;
    std::size_t end = endAt;
    while (begin < end && target[begin].filler) {
        ++begin;
    }
    while (end > begin && target[end - 1].filler) {
        --end;
    }
    best.begin = begin;
    best.end = end;
    best.score = cell.score;
    best.matched = cell.matched;
    best.queryMatched = cell.queryMatched;
    for (std::size_t j = begin; j < end; ++j) {
        if (!target[j].filler) {
            ++best.targetUnits;
        }
    }
    best.coverage = std::min(1.0, cell.matched / static_cast<double>(m));
    best.precision = best.targetUnits > 0 ? std::min(1.0, cell.matched / static_cast<double>(best.targetUnits)) : 0.0;
    return best;
}

std::vector<DiffRun> diffUnits(std::span<const Unit> a, std::span<const Unit> b, double minSimilarity) {
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    // lcs[i][j] = LCS length of a[i..] and b[j..].
    std::vector<std::uint32_t> lcs((n + 1) * (m + 1), 0);
    const auto at = [&](std::size_t i, std::size_t j) -> std::uint32_t& { return lcs[i * (m + 1) + j]; };
    const auto same = [&](std::size_t i, std::size_t j) { return unitSimilarity(a[i], b[j]) >= minSimilarity; };
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            at(i, j) = same(i, j) ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
        }
    }
    std::vector<DiffRun> runs;
    const auto push = [&](DiffRun::Kind kind, std::size_t i, std::size_t j) {
        if (!runs.empty() && runs.back().kind == kind) {
            DiffRun& r = runs.back();
            if (kind != DiffRun::Kind::Inserted) {
                r.aEnd = i + 1;
            }
            if (kind != DiffRun::Kind::Deleted) {
                r.bEnd = j + 1;
            }
            return;
        }
        DiffRun r;
        r.kind = kind;
        r.aBegin = i;
        r.aEnd = kind == DiffRun::Kind::Inserted ? i : i + 1;
        r.bBegin = j;
        r.bEnd = kind == DiffRun::Kind::Deleted ? j : j + 1;
        runs.push_back(r);
    };
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && same(i, j)) {
            push(DiffRun::Kind::Equal, i, j);
            ++i;
            ++j;
        } else if (j < m && (i == n || at(i, j + 1) >= at(i + 1, j))) {
            // Prefer deleting before inserting at a tie, so a replacement reads "old -> new".
            if (i < n && at(i + 1, j) == at(i, j + 1)) {
                push(DiffRun::Kind::Deleted, i, j);
                ++i;
            } else {
                push(DiffRun::Kind::Inserted, i, j);
                ++j;
            }
        } else {
            push(DiffRun::Kind::Deleted, i, j);
            ++i;
        }
    }
    return runs;
}

std::uint64_t featureHash(std::string_view text) {
    std::uint64_t h = 14695981039346656037ull;
    for (const char ch : text) {
        h ^= static_cast<unsigned char>(ch);
        h *= 1099511628211ull;
    }
    return h;
}

FeatureBag featuresOf(std::span<const Unit> us) {
    std::map<std::uint64_t, float> counts;
    std::vector<const Unit*> words;
    std::vector<const Unit*> clusters;
    const auto flushClusters = [&] {
        for (std::size_t k = 0; k + 1 < clusters.size(); ++k) {
            counts[featureHash("c2:" + clusters[k]->text + clusters[k + 1]->text)] += 1.0f;
            if (k + 2 < clusters.size()) {
                counts[featureHash("c3:" + clusters[k]->text + clusters[k + 1]->text + clusters[k + 2]->text)] += 1.0f;
            }
        }
        if (clusters.size() == 1) {
            counts[featureHash("c1:" + clusters[0]->text)] += 1.0f;
        }
        clusters.clear();
    };
    for (const Unit& u : us) {
        if (u.filler) {
            continue;
        }
        if (u.word) {
            flushClusters();
            counts[featureHash("w:" + u.text)] += 1.0f;
            if (!words.empty()) {
                counts[featureHash("b:" + words.back()->text + " " + u.text)] += 1.0f;
            }
            words.push_back(&u);
        } else {
            clusters.push_back(&u);
        }
    }
    flushClusters();
    FeatureBag bag;
    bag.features.assign(counts.begin(), counts.end());
    return bag;
}

double cosine(const FeatureBag& a, const FeatureBag& b) {
    double dotProduct = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (const auto& feature : a.features) {
        na += static_cast<double>(feature.second) * feature.second;
    }
    for (const auto& feature : b.features) {
        nb += static_cast<double>(feature.second) * feature.second;
    }
    std::size_t i = 0;
    std::size_t k = 0;
    while (i < a.features.size() && k < b.features.size()) {
        if (a.features[i].first < b.features[k].first) {
            ++i;
        } else if (b.features[k].first < a.features[i].first) {
            ++k;
        } else {
            dotProduct += static_cast<double>(a.features[i].second) * b.features[k].second;
            ++i;
            ++k;
        }
    }
    return na > 0.0 && nb > 0.0 ? dotProduct / std::sqrt(na * nb) : 0.0;
}

} // namespace wabcut::ai::roughcut
