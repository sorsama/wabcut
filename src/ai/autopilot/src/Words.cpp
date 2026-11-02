// Creator autopilot: words for matching, the transcript's lines, file names and YouTube times (Autopilot.h).

#include <wabcut/ai/autopilot/Autopilot.h>

#include <wabcut/ai/shorts/Shorts.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace wabcut::ai::autopilot {

namespace {

bool endsWith(std::string_view s, std::string_view tail) { return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail; }

bool isAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isVowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }

/// "runn" -> "run", "stopp" -> "stop" (a doubled consonant left by -ing / -ed), but "fall", "kiss" keep theirs.
std::string undouble(std::string s) {
    if (s.size() >= 3 && s[s.size() - 1] == s[s.size() - 2] && !isVowel(s.back()) && s.back() != 'l' && s.back() != 's' &&
        s.back() != 'z') {
        s.pop_back();
    }
    return s;
}

/// File types and words names are full of that say nothing about what a shot shows.
const std::set<std::string, std::less<>>& noiseWords() {
    static const std::set<std::string, std::less<>> words{
        "mp4", "mov", "mkv", "avi", "mxf", "webm", "m4v", "jpg", "jpeg", "png", "tif", "tiff", "gif", "bmp", "heic", "webp",
        "wav", "mp3", "aac", "flac", "clip", "video", "footage", "shot", "take", "final", "copy", "edit", "export", "render",
        "img", "dsc", "dji", "gopr", "gopro", "mvi", "pxl", "vid", "screen", "recording", "untitled", "new", "raw", "proxy",
        "4k", "hd", "uhd", "fps", "sequence", "v1", "v2"};
    return words;
}

} // namespace

std::string stem(std::string_view token) {
    std::string s;
    s.reserve(token.size());
    for (const char c : token) {
        s.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    // Only plain ASCII words get English endings removed; other scripts are matched as they are.
    if (s.empty() || !std::ranges::all_of(s, [](char c) { return isAsciiAlpha(c) || c == '\''; })) {
        return s;
    }
    if (endsWith(s, "'s")) {
        s.resize(s.size() - 2);
    }
    std::erase(s, '\'');
    if (s.size() > 4 && endsWith(s, "ies")) {
        return s.substr(0, s.size() - 3) + "y";
    }
    if (s.size() > 4 && endsWith(s, "sses")) {
        return s.substr(0, s.size() - 2);
    }
    if (s.size() > 3 && endsWith(s, "es") &&
        (endsWith(s, "ches") || endsWith(s, "shes") || endsWith(s, "xes") || endsWith(s, "zes") || endsWith(s, "sses"))) {
        return s.substr(0, s.size() - 2);
    }
    if (s.size() > 3 && endsWith(s, "s") && !endsWith(s, "ss") && !endsWith(s, "us") && !endsWith(s, "is")) {
        s.pop_back();
    }
    if (s.size() > 5 && endsWith(s, "ing")) {
        return undouble(s.substr(0, s.size() - 3));
    }
    if (s.size() > 4 && endsWith(s, "ed") && !endsWith(s, "eed")) {
        return undouble(s.substr(0, s.size() - 2));
    }
    return s;
}

std::vector<std::string> keywordTokens(std::string_view text) {
    // A file name's extension says nothing ("drone.mp4", "IMG_2041.MOV").
    if (const std::size_t dot = text.rfind('.'); dot != std::string_view::npos && text.size() - dot >= 3 && text.size() - dot <= 6 &&
                                                 std::all_of(text.begin() + static_cast<std::ptrdiff_t>(dot) + 1, text.end(), [](char c) {
                                                     return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                                                 })) {
        text = text.substr(0, dot);
    }
    // Split names first: separators and digits become spaces, and so does a lower-to-upper case change.
    std::string spaced;
    spaced.reserve(text.size() + 8);
    char previous = ' ';
    for (const char c : text) {
        const bool separator = c == '_' || c == '-' || c == '.' || c == '/' || c == '\\' || c == '+' || c == '(' || c == ')' ||
                               c == '[' || c == ']' || (c >= '0' && c <= '9');
        if (separator) {
            spaced.push_back(' ');
        } else {
            if (c >= 'A' && c <= 'Z' && previous >= 'a' && previous <= 'z') {
                spaced.push_back(' ');
            }
            spaced.push_back(c);
        }
        previous = c;
    }
    std::vector<std::string> out;
    for (const std::string& token : shorts::TextVectors::tokens(spaced)) {
        std::string s = stem(token);
        if (s.size() < 2 || noiseWords().contains(s)) {
            continue;
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<Line> transcriptLines(const edit::SequenceTranscript& transcript, std::size_t keywords) {
    std::vector<Line> lines;
    const std::vector<shorts::Sentence> sentences = shorts::splitSentences(transcript);
    std::vector<std::vector<std::string>> tokens;
    std::map<std::string, std::size_t, std::less<>> documents;
    for (const shorts::Sentence& s : sentences) {
        std::vector<std::string> words = keywordTokens(s.text);
        std::set<std::string> unique(words.begin(), words.end());
        for (const std::string& w : unique) {
            ++documents[w];
        }
        tokens.push_back(std::move(words));
    }
    const double n = static_cast<double>(std::max<std::size_t>(1, sentences.size()));
    const auto idf = [&](const std::string& w) {
        const auto it = documents.find(w);
        const double df = it == documents.end() ? 1.0 : static_cast<double>(it->second);
        // Longer words are usually the more specific ones ("telescope" over "big").
        return std::log(1.0 + n / df) * (w.size() >= 4 ? 1.0 : 0.7);
    };
    for (std::size_t i = 0; i < sentences.size(); ++i) {
        Line line;
        line.range = sentences[i].range;
        line.text = sentences[i].text;
        std::vector<std::pair<std::string, double>> scored;
        std::set<std::string> seen;
        for (const std::string& w : tokens[i]) {
            if (seen.insert(w).second) {
                scored.emplace_back(w, idf(w));
            }
        }
        double sum = 0.0;
        for (const auto& [w, s] : scored) {
            sum += s;
        }
        line.weight = scored.empty() ? 0.0 : sum / std::sqrt(static_cast<double>(scored.size()));
        std::ranges::stable_sort(scored, [](const auto& a, const auto& b) { return a.second > b.second; });
        for (std::size_t k = 0; k < scored.size() && k < keywords; ++k) {
            line.keywords.push_back(scored[k].first);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string youtubeTime(Ticks t) {
    const auto total = static_cast<std::int64_t>(std::floor(std::max(0.0, t.toSeconds())));
    const std::int64_t h = total / 3600;
    const std::int64_t m = total / 60 % 60;
    const std::int64_t s = total % 60;
    const auto two = [](std::int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    return h > 0 ? std::to_string(h) + ":" + two(m) + ":" + two(s) : std::to_string(m) + ":" + two(s);
}

std::string chaptersText(std::span<const KitChapter> chapters) {
    std::string out;
    for (const KitChapter& c : chapters) {
        out += youtubeTime(c.start) + " " + c.title + "\n";
    }
    return out;
}

std::string hashtags(std::span<const std::string> tags, std::size_t count) {
    std::string out;
    std::size_t used = 0;
    for (const std::string& tag : tags) {
        if (used == count) {
            break;
        }
        std::string word;
        bool upper = true;
        for (const char c : tag) {
            if (c == ' ') {
                upper = true;
                continue;
            }
            if (c == '#' || c == ',') {
                continue;
            }
            word.push_back(upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
            upper = false;
        }
        if (word.empty()) {
            continue;
        }
        out += (out.empty() ? "#" : " #") + word;
        ++used;
    }
    return out;
}

std::string safeFileName(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        const bool bad = u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*';
        out.push_back(bad ? '-' : c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
        out.erase(out.begin());
    }
    if (out.size() > 120) {
        out.resize(120);
        // Do not leave half a UTF-8 character.
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
            out.pop_back();
        }
        if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80) != 0) {
            out.pop_back();
        }
    }
    std::string upper;
    for (const char c : out.substr(0, out.find('.'))) {
        upper.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    }
    static const std::set<std::string, std::less<>> devices{"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                                             "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                                             "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (devices.contains(upper)) {
        out = "_" + out;
    }
    return out.empty() ? std::string("Untitled") : out;
}

} // namespace wabcut::ai::autopilot
