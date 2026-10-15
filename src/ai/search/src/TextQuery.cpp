#include <wabcut/ai/search/TextQuery.h>

#include <wabcut/ai/search/Tokenizer.h>

#include <algorithm>
#include <string_view>

namespace wabcut::ai::search {

namespace {

bool separator(char c) {
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
    case ',':
    case '.':
    case ';':
    case ':':
    case '!':
    case '?':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '<':
    case '>':
    case '|':
    case '/':
    case '\\':
    case '*':
    case '+':
    case '=':
    case '~':
    case '^':
    case '`':
        return true;
    default:
        return false;
    }
}

/// Length of a Unicode space at text[i] that is not ASCII (no-break space, ideographic space), else 0.
std::size_t unicodeSpace(std::string_view text, std::size_t i) {
    if (text.substr(i, 2) == "\xC2\xA0") {
        return 2;
    }
    if (text.substr(i, 3) == "\xE3\x80\x80") {
        return 3;
    }
    return 0;
}

/// The first `count` code points of `text`.
std::string firstCodePoints(std::string_view text, std::size_t count) {
    std::size_t i = 0;
    for (std::size_t n = 0; i < text.size() && n < count; ++n) {
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
            ++i;
        }
    }
    return std::string(text.substr(0, i));
}

/// An FTS5 string: the term in double quotes, quotes doubled (a trigram phrase = a substring match).
std::string ftsString(std::string_view term) {
    std::string out = "\"";
    for (const char c : term) {
        if (c == '"') {
            out += '"';
        }
        out += c;
    }
    return out + '"';
}

/// Marks the words a match at [begin, end) of `joined` covers; `starts` holds each word's offset in it.
void markSpan(std::vector<bool>& marked, const std::vector<std::size_t>& starts, const std::vector<std::size_t>& ends,
              std::size_t begin, std::size_t end) {
    for (std::size_t w = 0; w < starts.size(); ++w) {
        if (starts[w] < end && begin < ends[w]) {
            marked[w] = true;
        }
    }
}

} // namespace

std::size_t codePoints(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

std::string escapeLike(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '%' || c == '_' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

bool writtenWithoutSpaces(std::string_view language) {
    const std::string_view base = language.substr(0, language.find_first_of("-_"));
    for (const std::string_view code : {"th", "lo", "km", "my", "zh", "ja", "yue", "bo", "dz"}) {
        if (base == code) {
            return true;
        }
    }
    return false;
}

TextQuery buildTextQuery(std::string_view query) {
    const std::string text = lowercaseUtf8(query);
    std::vector<std::string> raw;
    std::string current;
    bool inQuotes = false;
    const auto flush = [&] {
        // trim what a quoted phrase may carry at its ends
        const auto first = current.find_first_not_of(' ');
        const auto last = current.find_last_not_of(' ');
        if (first != std::string::npos) {
            raw.push_back(current.substr(first, last - first + 1));
        }
        current.clear();
    };
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '"') {
            flush();
            inQuotes = !inQuotes;
            ++i;
            continue;
        }
        if (const std::size_t space = unicodeSpace(text, i); space > 0) {
            if (inQuotes) {
                current += ' ';
            } else {
                flush();
            }
            i += space;
            continue;
        }
        if (!inQuotes && separator(c)) {
            flush();
        } else {
            current += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
        }
        ++i;
    }
    flush();

    TextQuery out;
    for (std::string& term : raw) {
        if (codePoints(term) > kMaxTermCodePoints) {
            term = firstCodePoints(term, kMaxTermCodePoints);
        }
        if (std::ranges::find(out.terms, term) == out.terms.end()) {
            out.terms.push_back(std::move(term));
        }
        if (out.terms.size() == kMaxQueryTerms) {
            break;
        }
    }
    for (const std::string& term : out.terms) {
        if (codePoints(term) >= 3) {
            if (!out.match.empty()) {
                out.match += " OR ";
            }
            out.match += ftsString(term);
        }
    }
    if (out.match.empty()) {
        for (const std::string& term : out.terms) {
            out.likeTerms.push_back("%" + escapeLike(term) + "%");
        }
    }
    return out;
}

std::vector<std::size_t> matchedWords(std::span<const std::string> words, const TextQuery& query) {
    std::vector<bool> marked(words.size(), false);
    // The words joined with spaces (terms with spaces, ordinary words) and without (scripts without spaces).
    for (const bool spaced : {true, false}) {
        std::string joined;
        std::vector<std::size_t> starts;
        std::vector<std::size_t> ends;
        for (std::size_t w = 0; w < words.size(); ++w) {
            if (spaced && w > 0) {
                joined += ' ';
            }
            starts.push_back(joined.size());
            joined += lowercaseUtf8(words[w]);
            ends.push_back(joined.size());
        }
        for (const std::string& term : query.terms) {
            if (term.empty() || (!query.match.empty() && codePoints(term) < 3)) {
                continue; // short terms count only when the query has nothing longer
            }
            for (std::size_t at = joined.find(term); at != std::string::npos; at = joined.find(term, at + 1)) {
                markSpan(marked, starts, ends, at, at + term.size());
            }
        }
    }
    std::vector<std::size_t> out;
    for (std::size_t w = 0; w < marked.size(); ++w) {
        if (marked[w]) {
            out.push_back(w);
        }
    }
    return out;
}

} // namespace wabcut::ai::search
