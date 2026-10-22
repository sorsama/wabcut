#include <wabcut/model/Spelling.h>

#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/TextFind.h>

#include <algorithm>
#include <fstream>

namespace wabcut {

namespace {

/// The code point at `i` of UTF-8 `s` and its length (a byte that is not UTF-8 is itself, length 1).
std::pair<char32_t, std::size_t> codePointAt(std::string_view s, std::size_t i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    const std::size_t n = b0 < 0x80 ? 1 : (b0 >> 5) == 0x6 ? 2 : (b0 >> 4) == 0xE ? 3 : (b0 >> 3) == 0x1E ? 4 : 0;
    if (n <= 1 || i + n > s.size()) {
        return {n == 1 ? b0 : static_cast<char32_t>(0x110000u + b0), 1};
    }
    char32_t c = n == 2 ? (b0 & 0x1F) : n == 3 ? (b0 & 0x0F) : (b0 & 0x07);
    for (std::size_t k = 1; k < n; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            return {static_cast<char32_t>(0x110000u + b0), 1};
        }
        c = (c << 6) | (b & 0x3F);
    }
    return {c, n};
}

bool isApostrophe(char32_t c) { return c == '\'' || c == 0x2019; }

bool isDigit(char32_t c) { return c >= '0' && c <= '9'; }

/// Whether the word is one a dictionary can hold: 1..kMaxSpellWordBytes bytes, no spaces or control characters.
bool validWord(std::string_view w) {
    if (w.empty() || w.size() > kMaxSpellWordBytes) {
        return false;
    }
    return std::none_of(w.begin(), w.end(), [](char ch) {
        const auto u = static_cast<unsigned char>(ch);
        return u < 0x20 || u == 0x7F || u == ' ';
    });
}

std::string trimmedWord(std::string_view w) {
    std::size_t b = 0;
    std::size_t e = w.size();
    while (b < e && (w[b] == ' ' || w[b] == '\t' || w[b] == '\r' || w[b] == '\n')) {
        ++b;
    }
    while (e > b && (w[e - 1] == ' ' || w[e - 1] == '\t' || w[e - 1] == '\r' || w[e - 1] == '\n')) {
        --e;
    }
    return std::string(w.substr(b, e - b));
}

/// Levenshtein distance between two folded words (code points), capped: anything above `cap` is cap + 1.
std::size_t editDistance(const std::u32string& a, const std::u32string& b, std::size_t cap) {
    const std::size_t la = a.size();
    const std::size_t lb = b.size();
    if ((la > lb ? la - lb : lb - la) > cap) {
        return cap + 1;
    }
    // Optimal string alignment: insertions, deletions, substitutions and swaps of two neighbours ("teh" -> "the").
    std::vector<std::size_t> before(lb + 1); // row i - 2
    std::vector<std::size_t> prev(lb + 1);   // row i - 1
    std::vector<std::size_t> cur(lb + 1);
    for (std::size_t j = 0; j <= lb; ++j) {
        prev[j] = j;
    }
    for (std::size_t i = 1; i <= la; ++i) {
        cur[0] = i;
        std::size_t rowMin = cur[0];
        for (std::size_t j = 1; j <= lb; ++j) {
            const std::size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                cur[j] = std::min(cur[j], before[j - 2] + 1);
            }
            rowMin = std::min(rowMin, cur[j]);
        }
        if (rowMin > cap) {
            return cap + 1;
        }
        before = prev;
        prev = cur;
    }
    return std::min(prev[lb], cap + 1);
}

std::u32string codePoints(std::string_view s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto [c, n] = codePointAt(s, i);
        out.push_back(c);
        i += n;
    }
    return out;
}

class NoSpellChecker final : public SpellChecker {
public:
    bool supports(std::string_view) const override { return false; }
    std::vector<SpellIssue> check(std::string_view, std::string_view) const override { return {}; }
    std::vector<std::string> suggest(std::string_view, std::string_view) const override { return {}; }
};

} // namespace

const SpellChecker& noSpellChecker() {
    static const NoSpellChecker checker;
    return checker;
}

std::string_view toString(SpellAction action) {
    switch (action) {
    case SpellAction::Suggest:
        return "suggest";
    case SpellAction::Replace:
        return "replace";
    case SpellAction::Delete:
        return "delete";
    }
    return "suggest";
}

std::vector<std::pair<std::size_t, std::size_t>> spellWords(std::string_view text) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto [c, n] = codePointAt(text, i);
        if (!isWordCharacter(c) || c == '_') {
            i += n;
            continue;
        }
        const std::size_t start = i;
        std::size_t end = i;
        bool letters = false;
        while (i < text.size()) {
            const auto [d, m] = codePointAt(text, i);
            if (isWordCharacter(d) && d != '_') {
                letters = letters || !isDigit(d);
                i += m;
                end = i;
                continue;
            }
            // An apostrophe inside a word belongs to it ("don't", "rock'n'roll").
            if (isApostrophe(d) && i + m < text.size()) {
                const char32_t after = codePointAt(text, i + m).first;
                if (isWordCharacter(after) && after != '_') {
                    i += m;
                    continue;
                }
            }
            break;
        }
        if (letters) {
            out.emplace_back(start, end);
        }
    }
    return out;
}

// ---- WordListSpellChecker ---------------------------------------------------------------------------------------------

WordListSpellChecker::WordListSpellChecker(std::vector<std::string> words, std::vector<std::string> languages)
    : m_original(std::move(words)), m_languages(std::move(languages)) {
    for (const std::string& w : m_original) {
        m_words.insert(foldCase(w));
    }
}

bool WordListSpellChecker::supports(std::string_view language) const {
    if (m_languages.empty() || language.empty()) {
        return true;
    }
    return std::any_of(m_languages.begin(), m_languages.end(), [&](const std::string& l) {
        return foldCase(l) == foldCase(language) || foldCase(language).starts_with(foldCase(l) + "-") ||
               foldCase(l).starts_with(foldCase(language) + "-");
    });
}

std::vector<SpellIssue> WordListSpellChecker::check(std::string_view text, std::string_view language) const {
    std::vector<SpellIssue> out;
    if (!supports(language)) {
        return out;
    }
    for (const auto& [b, e] : spellWords(text)) {
        const std::string word(text.substr(b, e - b));
        if (!m_words.contains(foldCase(word))) {
            out.push_back(SpellIssue{b, e, word, SpellAction::Suggest, {}});
        }
    }
    return out;
}

std::vector<std::string> WordListSpellChecker::suggest(std::string_view word, std::string_view language) const {
    std::vector<std::string> out;
    if (!supports(language) || word.empty()) {
        return out;
    }
    const std::u32string w = codePoints(foldCase(word));
    std::vector<std::pair<std::size_t, std::string>> scored;
    for (const std::string& candidate : m_original) {
        const std::size_t d = editDistance(w, codePoints(foldCase(candidate)), 2);
        if (d >= 1 && d <= 2) {
            scored.emplace_back(d, candidate);
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [d, candidate] : scored) {
        if (out.size() >= kMaxSpellSuggestions) {
            break;
        }
        if (std::find(out.begin(), out.end(), candidate) == out.end()) {
            out.push_back(candidate);
        }
    }
    return out;
}

// ---- PersonalSpelling -------------------------------------------------------------------------------------------------

PersonalSpelling::PersonalSpelling(std::shared_ptr<const SpellChecker> base, std::filesystem::path dictionary)
    : m_base(std::move(base)), m_file(std::move(dictionary)) {
    if (m_file.empty()) {
        return;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(m_file, ec);
    if (ec || size > 4u * 1024u * 1024u) {
        return;
    }
    std::ifstream in(m_file, std::ios::binary);
    std::string line;
    while (in && std::getline(in, line) && m_words.size() < kMaxPersonalWords) {
        const std::string w = trimmedWord(line);
        if (validWord(w) && m_accepted.insert(foldCase(w)).second) {
            m_words.push_back(w);
        }
    }
}

bool PersonalSpelling::supports(std::string_view language) const {
    std::lock_guard lock(m_mutex);
    return m_base && m_base->supports(language);
}

std::vector<SpellIssue> PersonalSpelling::check(std::string_view text, std::string_view language) const {
    std::shared_ptr<const SpellChecker> base;
    {
        std::lock_guard lock(m_mutex);
        base = m_base;
    }
    if (!base) {
        return {};
    }
    std::vector<SpellIssue> issues = base->check(text, language);
    std::lock_guard lock(m_mutex);
    std::erase_if(issues, [&](const SpellIssue& i) {
        // A Delete (a repeated word) is the checker's call about the words around it, not about the word.
        return i.action != SpellAction::Delete && m_accepted.contains(foldCase(i.word));
    });
    return issues;
}

std::vector<std::string> PersonalSpelling::suggest(std::string_view word, std::string_view language) const {
    std::shared_ptr<const SpellChecker> base;
    {
        std::lock_guard lock(m_mutex);
        base = m_base;
    }
    return base ? base->suggest(word, language) : std::vector<std::string>{};
}

bool PersonalSpelling::addWord(std::string_view word) {
    const std::string w = trimmedWord(word);
    if (!validWord(w)) {
        return false;
    }
    std::lock_guard lock(m_mutex);
    const std::string folded = foldCase(w);
    const bool known = std::any_of(m_words.begin(), m_words.end(), [&](const std::string& x) { return foldCase(x) == folded; });
    if (!known) {
        if (m_words.size() >= kMaxPersonalWords) {
            return false;
        }
        m_words.push_back(w);
    }
    m_accepted.insert(folded);
    return saveLocked();
}

bool PersonalSpelling::removeWord(std::string_view word) {
    const std::string folded = foldCase(trimmedWord(word));
    std::lock_guard lock(m_mutex);
    const auto it = std::find_if(m_words.begin(), m_words.end(), [&](const std::string& x) { return foldCase(x) == folded; });
    if (it == m_words.end()) {
        return false;
    }
    m_words.erase(it);
    m_accepted.erase(folded);
    saveLocked();
    return true;
}

void PersonalSpelling::ignoreWord(std::string_view word) {
    const std::string w = trimmedWord(word);
    if (!validWord(w)) {
        return;
    }
    std::lock_guard lock(m_mutex);
    m_accepted.insert(foldCase(w));
}

std::vector<std::string> PersonalSpelling::words() const {
    std::lock_guard lock(m_mutex);
    return m_words;
}

bool PersonalSpelling::accepted(std::string_view word) const {
    std::lock_guard lock(m_mutex);
    return m_accepted.contains(foldCase(trimmedWord(word)));
}

void PersonalSpelling::setBase(std::shared_ptr<const SpellChecker> base) {
    std::lock_guard lock(m_mutex);
    m_base = std::move(base);
}

bool PersonalSpelling::saveLocked() const {
    if (m_file.empty()) {
        return true;
    }
    std::string contents;
    for (const std::string& w : m_words) {
        contents += w;
        contents += '\n';
    }
    std::error_code ec;
    if (m_file.has_parent_path()) {
        std::filesystem::create_directories(m_file.parent_path(), ec);
    }
    return io::writeFileAtomically(m_file, contents).has_value();
}

// ---- Captions and graphics --------------------------------------------------------------------------------------------

namespace {

void addIssues(std::vector<TextSpellIssue>& out, std::string_view text, std::string_view language, const SpellChecker& checker,
               bool suggestions, std::size_t limit, std::size_t cue, std::uint32_t layer) {
    for (SpellIssue& issue : checker.check(text, language)) {
        if (out.size() >= limit) {
            return;
        }
        if (suggestions && issue.suggestions.empty() && issue.action != SpellAction::Delete) {
            issue.suggestions = checker.suggest(issue.word, language);
        }
        if (issue.suggestions.size() > kMaxSpellSuggestions) {
            issue.suggestions.resize(kMaxSpellSuggestions);
        }
        out.push_back(TextSpellIssue{cue, layer, std::move(issue)});
    }
}

void layerIssues(std::vector<TextSpellIssue>& out, const std::vector<GraphicLayer>& layers, const SpellChecker& checker,
                 std::string_view language, bool suggestions, std::size_t limit, std::size_t depth) {
    if (depth > kMaxGraphicDepth) {
        return;
    }
    for (const GraphicLayer& l : layers) {
        if (out.size() >= limit) {
            return;
        }
        if (l.kind == LayerKind::Text) {
            const std::string lang = !l.text.language.empty() ? l.text.language : std::string(language);
            addIssues(out, l.text.text(), lang, checker, suggestions, limit, 0, l.id);
        } else if (l.kind == LayerKind::Group) {
            layerIssues(out, l.children, checker, language, suggestions, limit, depth + 1);
        }
    }
}

} // namespace

std::vector<TextSpellIssue> spellingOf(const CaptionTrack& track, const SpellChecker& checker, std::string_view language,
                                       bool suggestions, std::size_t limit) {
    std::vector<TextSpellIssue> out;
    const std::string lang = !language.empty() ? std::string(language) : track.language;
    for (std::size_t i = 0; i < track.cues.size() && out.size() < limit; ++i) {
        addIssues(out, track.cues[i].text, lang, checker, suggestions, limit, i, 0);
    }
    return out;
}

std::vector<TextSpellIssue> spellingOf(const Graphic& graphic, const SpellChecker& checker, std::string_view language,
                                       bool suggestions, std::size_t limit) {
    std::vector<TextSpellIssue> out;
    layerIssues(out, graphic.layers, checker, language, suggestions, limit, 1);
    return out;
}

} // namespace wabcut
