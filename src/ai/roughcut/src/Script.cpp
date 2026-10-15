#include <wabcut/ai/roughcut/Script.h>

#include <wabcut/ai/Text.h>
#include <wabcut/ai/roughcut/TextMatch.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace wabcut::ai::roughcut {

namespace {

std::string_view trim(std::string_view s) {
    // ASCII whitespace plus the no-break and ideographic spaces people paste from documents.
    const auto isSpace = [](std::string_view rest) -> std::size_t {
        if (rest.empty()) {
            return 0;
        }
        const unsigned char c = static_cast<unsigned char>(rest[0]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            return 1;
        }
        if (rest.starts_with("\xC2\xA0")) {
            return 2;
        }
        if (rest.starts_with("\xE3\x80\x80") || rest.starts_with("\xE2\x80\x8B")) {
            return 3;
        }
        return 0;
    };
    while (std::size_t n = isSpace(s)) {
        s.remove_prefix(n);
    }
    while (!s.empty()) {
        std::size_t n = 0;
        for (const std::size_t k : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
            if (s.size() >= k && isSpace(s.substr(s.size() - k)) == k) {
                n = k;
                break;
            }
        }
        if (n == 0) {
            break;
        }
        s.remove_suffix(n);
    }
    return s;
}

/// Drops Markdown emphasis and code markers (**, __, *, _, `) that stand next to words.
std::string stripDecoration(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '`') {
            continue;
        }
        if (c == '*' || c == '_') {
            // Inside a word ("snake_case", "2*3") it stays; at a word edge it is decoration.
            const bool before = i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1])) != 0;
            const bool after = i + 1 < s.size() && std::isalnum(static_cast<unsigned char>(s[i + 1])) != 0;
            if (before && after) {
                out += c;
            }
            continue;
        }
        out += c;
    }
    return out;
}

bool startsWithCi(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

/// A visual cue's query: "[B-roll: drone shot]" -> "drone shot"; "(cut to the product)" -> "the product".
std::string cueQuery(std::string_view inner) {
    std::string_view s = trim(inner);
    for (const std::string_view prefix : {"b-roll", "broll", "b roll", "visual", "visuals", "shot", "insert", "graphic", "screen",
                                          "cut to", "show", "footage", "ภาพ"}) {
        if (startsWithCi(s, prefix)) {
            std::string_view rest = s.substr(prefix.size());
            rest = trim(rest);
            if (!rest.empty() && (rest.front() == ':' || rest.front() == '-')) {
                rest.remove_prefix(1);
            }
            return std::string(trim(rest));
        }
    }
    return std::string(s);
}

/// A speaker label at the start of a line: "JOHN: ...", "Host: ...", "พิธีกร: ...". Returns its length (with the colon
/// and the space after it) or 0.
std::size_t speakerLabel(std::string_view line, std::string& speaker) {
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 40 || colon + 1 >= line.size()) {
        return 0;
    }
    const std::string_view label = line.substr(0, colon);
    if (line[colon + 1] != ' ' && line[colon + 1] != '\t') {
        return 0; // "10:30" or a URL
    }
    std::size_t words = 1;
    bool upper = true;
    bool ascii = true;
    for (const char c : label) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80) {
            ascii = false;
            continue;
        }
        if (c == ' ') {
            ++words;
        } else if (std::isdigit(u) != 0) {
            return 0;
        } else if (std::islower(u) != 0) {
            upper = false;
        }
    }
    // ASCII labels: all caps (a screenplay) or at most two words ("Host", "Dr Smith"); others: no spaces (Thai names).
    const bool isLabel = ascii ? (upper || words <= 2) && words <= 3 : words == 1 && label.size() <= 48;
    if (!isLabel) {
        return 0;
    }
    speaker = std::string(trim(label));
    std::size_t end = colon + 1;
    while (end < line.size() && (line[end] == ' ' || line[end] == '\t')) {
        ++end;
    }
    return end;
}

/// Splits bracketed cues out of a line: the spoken rest and the cues' queries.
std::string extractCues(std::string_view line, std::vector<std::string>& cues) {
    std::string spoken;
    std::size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == '[' || c == '(') {
            const char close = c == '[' ? ']' : ')';
            const std::size_t end = line.find(close, i + 1);
            if (end != std::string_view::npos) {
                const std::string_view inner = line.substr(i + 1, end - i - 1);
                // Brackets hold stage directions; parentheses only when they read like one (a cue word inside).
                const std::string query = cueQuery(inner);
                const bool direction = c == '[' || query.size() != trim(inner).size();
                if (direction) {
                    if (!query.empty()) {
                        cues.push_back(query);
                    }
                    i = end + 1;
                    continue;
                }
            }
        }
        spoken += c;
        ++i;
    }
    return spoken;
}

/// "- ", "* ", "+ ", "• ", "1. ", "1) " (after any indentation): the bullet's length, else 0.
std::size_t bulletLength(std::string_view s) {
    if (s.size() >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') {
        return 2;
    }
    if (s.starts_with("\xE2\x80\xA2")) { // •
        return s.size() > 3 && s[3] == ' ' ? 4 : 3;
    }
    std::size_t digits = 0;
    while (digits < s.size() && digits < 4 && std::isdigit(static_cast<unsigned char>(s[digits])) != 0) {
        ++digits;
    }
    if (digits > 0 && digits + 1 < s.size() && (s[digits] == '.' || s[digits] == ')') && s[digits + 1] == ' ') {
        return digits + 2;
    }
    return 0;
}

std::size_t wordCount(std::string_view s) {
    return units(s).size();
}

bool mostlyThai(std::string_view s) {
    std::size_t thai = 0;
    std::size_t letters = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xE0 && i + 2 < s.size() && (static_cast<unsigned char>(s[i + 1]) == 0xB8 || static_cast<unsigned char>(s[i + 1]) == 0xB9)) {
            ++thai;
            ++letters;
            i += 3;
            continue;
        }
        if (std::isalpha(c) != 0) {
            ++letters;
        }
        ++i;
    }
    return letters > 0 && thai * 10 >= letters * 3;
}

} // namespace

std::vector<std::string> splitProse(std::string_view line) {
    std::vector<std::string> out;
    std::string current;
    const auto endsAt = [&](std::size_t i, std::size_t& length) {
        // ASCII terminators followed by a space / quote / end; CJK full stops end on their own.
        for (const std::string_view mark : {std::string_view("\xE3\x80\x82"), std::string_view("\xEF\xBC\x9F"), std::string_view("\xEF\xBC\x81")}) {
            if (line.substr(i).starts_with(mark)) {
                length = mark.size();
                return true;
            }
        }
        std::size_t k = i;
        if (line.substr(k).starts_with("\xE2\x80\xA6")) { // …
            k += 3;
        } else if (line[k] == '.' || line[k] == '?' || line[k] == '!') {
            while (k < line.size() && (line[k] == '.' || line[k] == '?' || line[k] == '!')) {
                ++k;
            }
        } else {
            return false;
        }
        while (k < line.size() && (line[k] == '"' || line[k] == '\'' || line[k] == ')')) {
            ++k;
        }
        if (k < line.size() && line[k] != ' ' && line[k] != '\t') {
            return false; // "3.5", "e.g.x", a URL
        }
        length = k - i;
        return true;
    };
    for (std::size_t i = 0; i < line.size();) {
        std::size_t length = 0;
        if (endsAt(i, length)) {
            current.append(line.substr(i, length));
            const std::string sentence(trim(current));
            if (!sentence.empty()) {
                if (!out.empty() && wordCount(sentence) < 3) {
                    out.back() += " " + sentence;
                } else {
                    out.push_back(sentence);
                }
            }
            current.clear();
            i += length;
            continue;
        }
        current += line[i];
        ++i;
    }
    const std::string rest(trim(current));
    if (!rest.empty()) {
        if (!out.empty() && wordCount(rest) < 3) {
            out.back() += " " + rest;
        } else {
            out.push_back(rest);
        }
    }
    return out;
}

std::expected<Script, Error> parseScript(std::string_view input, const ScriptOptions& options) {
    if (input.size() > options.maxBytes) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the script is larger than " + std::to_string(options.maxBytes / 1024) +
                                                                     " KiB: paste a part of it"));
    }
    std::string text = text::sanitizeUtf8(input);
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    Script script;
    std::string section;
    std::size_t lineNumber = 0;
    std::size_t start = 0;
    std::vector<std::string> pendingCues; // cue lines before the first beat
    bool inFence = false;
    std::size_t thaiLines = 0;
    std::size_t spokenLines = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string_view raw = std::string_view(text).substr(start, end - start);
        start = end + 1;
        ++lineNumber;
        std::string_view line = trim(raw);
        if (line.starts_with("```")) {
            inFence = !inFence;
            continue;
        }
        if (inFence || line.empty() || line == "---" || line == "***") {
            if (end == text.size()) {
                break;
            }
            continue;
        }
        while (line.starts_with('>')) {
            line = trim(line.substr(1));
        }
        if (line.starts_with('#')) {
            std::size_t level = 0;
            while (level < line.size() && line[level] == '#') {
                ++level;
            }
            const std::string heading(trim(stripDecoration(line.substr(level))));
            if (!heading.empty()) {
                if (script.title.empty() && script.beats.empty() && level == 1) {
                    script.title = heading;
                }
                section = heading;
            }
            continue;
        }
        bool outline = false;
        if (const std::size_t bullet = bulletLength(line)) {
            outline = true;
            line = trim(line.substr(bullet));
        }
        std::string decorated = stripDecoration(line);
        std::vector<std::string> cues;
        std::string spoken = extractCues(decorated, cues);
        std::string speaker;
        std::string_view spokenView = trim(spoken);
        if (const std::size_t label = speakerLabel(spokenView, speaker)) {
            spokenView = trim(spokenView.substr(label));
        }
        if (units(spokenView).empty()) {
            // A cue-only line belongs to the beat before it (or the first beat).
            if (!cues.empty()) {
                if (script.beats.empty()) {
                    pendingCues.insert(pendingCues.end(), cues.begin(), cues.end());
                } else {
                    auto& broll = script.beats.back().broll;
                    broll.insert(broll.end(), cues.begin(), cues.end());
                }
            }
            continue;
        }
        ++spokenLines;
        const bool thai = mostlyThai(spokenView);
        thaiLines += thai ? 1 : 0;
        std::vector<std::string> pieces;
        const bool split = options.split == ScriptSplit::Sentences ||
                           (options.split == ScriptSplit::Auto && !outline && !thai);
        if (split) {
            pieces = splitProse(spokenView);
        } else {
            pieces.emplace_back(spokenView);
        }
        for (std::size_t k = 0; k < pieces.size(); ++k) {
            ScriptBeat beat;
            beat.index = script.beats.size();
            beat.text = pieces[k];
            beat.section = section;
            beat.speaker = speaker;
            beat.outline = outline;
            beat.line = lineNumber;
            if (k == 0) {
                beat.broll = std::move(cues);
                cues.clear();
                if (script.beats.empty() && !pendingCues.empty()) {
                    beat.broll.insert(beat.broll.begin(), pendingCues.begin(), pendingCues.end());
                    pendingCues.clear();
                }
            }
            script.beats.push_back(std::move(beat));
            if (script.beats.size() > options.maxBeats) {
                return std::unexpected(Error(ErrorCode::InvalidArgument, "the script has more than " + std::to_string(options.maxBeats) +
                                                                             " beats: make a rough cut of one part at a time"));
            }
        }
        if (end == text.size()) {
            break;
        }
    }
    if (!pendingCues.empty()) {
        script.warnings.push_back("visual cues without a line to go with them were ignored");
    }
    if (spokenLines > 0 && thaiLines * 2 > spokenLines) {
        script.language = "th";
    }
    return script;
}

} // namespace wabcut::ai::roughcut
