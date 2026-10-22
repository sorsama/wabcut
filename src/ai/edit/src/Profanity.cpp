#include <wabcut/ai/edit/Profanity.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace wabcut::ai::edit {

namespace {

// English: strong swear words and slurs (a trailing '*' covers the words that start with it). Mild words ("damn",
// "crap") are left out; add them per request.
constexpr std::array<std::string_view, 41> kEnglish{
    "fuck*",   "motherfuck*", "fck*",     "fuk*",    "fucc*",  "shit*",    "bullshit*", "bitch*",  "cunt*",
    "asshole*", "arsehole*",  "bastard*", "dick",    "dicks",  "dickhead*", "cock",     "cocks",   "cocksuck*",
    "pussy",   "pussies",     "twat*",    "wank*",   "slut*",  "whore*",   "fag",       "fags",    "faggot*",
    "nigger*", "nigga*",      "retard*",  "douche*", "prick*", "bollocks", "jackass*",  "dumbass*", "wtf",
    "stfu",    "goddamn*",    "piss",     "pissed",  "pisses"};

// Thai: whole words, or with '*' inside any word (only entries that are not part of ordinary words).
constexpr std::array<std::string_view, 20> kThai{
    "เหี้ย*", "เชี่ย*", "ควย*", "เย็ด*", "หี", "แตด", "สัส", "สัด", "ไอ้สัตว์*", "อีสัตว์*",
    "อีดอก*", "ส้นตีน*", "กระหรี่*", "ระยำ*", "ชาติหมา*", "พ่อมึงตาย*", "แม่มึงตาย*", "อีเวร*", "แม่ง", "ไอ้เวร*"};

bool isThaiByte(unsigned char lead, unsigned char next) {
    // Thai is U+0E00..U+0E7F: UTF-8 E0 B8 xx / E0 B9 xx.
    return lead == 0xE0 && (next == 0xB8 || next == 0xB9);
}

bool hasThai(std::string_view s) {
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (isThaiByte(static_cast<unsigned char>(s[i]), static_cast<unsigned char>(s[i + 1]))) {
            return true;
        }
    }
    return false;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        if (static_cast<unsigned char>(ch) < 0x80) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    return out;
}

bool isWordByte(unsigned char ch) { return ch >= 0x80 || std::isalnum(ch) != 0 || ch == '\''; }

/// UTF-8 code point length of the lead byte.
std::size_t codePointLength(unsigned char lead) {
    return lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
}

/// `s[begin, end)` with its first code point kept and every later one replaced by '*'.
std::string masked(std::string_view s, std::size_t begin, std::size_t end) {
    std::string out(s.substr(0, begin));
    std::size_t i = begin;
    bool first = true;
    while (i < end) {
        const std::size_t n = std::min(codePointLength(static_cast<unsigned char>(s[i])), end - i);
        if (first) {
            out.append(s.substr(i, n));
            first = false;
        } else {
            out.push_back('*');
        }
        i += n;
    }
    out.append(s.substr(end));
    return out;
}

} // namespace

ProfanityFilter::ProfanityFilter(std::span<const std::string> extra, std::span<const std::string> allowed) {
    const auto addEntry = [&](std::string_view text) {
        if (text.empty() || text.size() > kMaxWordBytes || m_entries.size() >= kMaxWords) {
            return;
        }
        Entry e;
        e.prefix = text.back() == '*';
        e.word = lower(e.prefix ? text.substr(0, text.size() - 1) : text);
        e.thai = hasThai(e.word);
        if (e.word.empty()) {
            return;
        }
        m_entries.push_back(std::move(e));
    };
    for (const std::string_view w : kEnglish) {
        addEntry(w);
    }
    for (const std::string_view w : kThai) {
        addEntry(w);
    }
    for (std::size_t i = 0; i < extra.size() && i < kMaxWords; ++i) {
        addEntry(extra[i]);
    }
    for (std::size_t i = 0; i < allowed.size() && i < kMaxWords; ++i) {
        std::string a = lower(allowed[i]);
        if (!a.empty() && a.back() == '*') {
            a.pop_back();
        }
        std::erase_if(m_entries, [&](const Entry& e) { return e.word == a; });
    }
}

std::span<const std::string_view> ProfanityFilter::builtin(std::string_view language) {
    if (language == "en") {
        return kEnglish;
    }
    if (language == "th") {
        return kThai;
    }
    return {};
}

std::pair<std::size_t, std::size_t> ProfanityFilter::find(std::string_view word) const {
    // The word without the punctuation around it.
    std::size_t b = 0;
    std::size_t e = word.size();
    while (b < e && !isWordByte(static_cast<unsigned char>(word[b]))) {
        ++b;
    }
    while (e > b && !isWordByte(static_cast<unsigned char>(word[e - 1]))) {
        --e;
    }
    if (b == e) {
        return {0, 0};
    }
    const std::string core = lower(word.substr(b, e - b));
    for (const Entry& entry : m_entries) {
        if (entry.thai) {
            if (core == entry.word) {
                return {b, e};
            }
            if (entry.prefix) {
                if (const auto at = core.find(entry.word); at != std::string::npos) {
                    return {b + at, b + at + entry.word.size()};
                }
            }
        } else if (entry.prefix ? core.starts_with(entry.word) : core == entry.word) {
            return {b, e};
        }
    }
    return {0, 0};
}

bool ProfanityFilter::isProfane(std::string_view word) const {
    const auto [b, e] = find(word);
    return e > b;
}

std::string ProfanityFilter::mask(std::string_view word) const {
    const auto [b, e] = find(word);
    return e > b ? masked(word, b, e) : std::string(word);
}

std::string ProfanityFilter::maskText(std::string_view text) const {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        // Keep whitespace as it is; mask each run between.
        if (text[i] == ' ' || text[i] == '\n' || text[i] == '\t') {
            out.push_back(text[i]);
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n' && text[j] != '\t') {
            ++j;
        }
        std::string token = mask(text.substr(i, j - i));
        // An unspaced Thai run may hold several words: its '*' entries are masked wherever they are inside.
        if (hasThai(token)) {
            for (const Entry& entry : m_entries) {
                if (!entry.thai || !entry.prefix) {
                    continue;
                }
                for (std::size_t at = token.find(entry.word); at != std::string::npos;
                     at = token.find(entry.word, at + 1)) {
                    token = masked(token, at, at + entry.word.size());
                }
            }
        }
        out += token;
        i = j;
    }
    return out;
}

} // namespace wabcut::ai::edit
