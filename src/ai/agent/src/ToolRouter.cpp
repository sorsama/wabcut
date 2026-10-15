#include <wabcut/ai/agent/ToolRouter.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <regex>
#include <set>
#include <utility>

namespace wabcut::ai::agent {

namespace {

/// Words that say nothing about which tool a request needs.
const std::set<std::string, std::less<>>& stopWords() {
    static const std::set<std::string, std::less<>> words{
        "a",    "an",   "the",  "to",   "of",    "in",    "on",    "at",   "and",  "or",    "with",  "for",
        "from", "by",   "it",   "its",  "this",  "that",  "these", "those", "is",  "are",   "be",    "as",
        "my",   "me",   "i",    "we",   "our",   "you",   "your",  "please", "can", "could", "would", "should",
        "make", "do",   "so",   "into", "onto",  "then",  "than",  "there", "here", "every", "each",  "all",
        "one",  "two",  "three", "four", "five", "first", "last",  "second", "seconds", "frame", "frames",
        "s",    "sec",  "about", "just", "only",  "also",  "but",   "not",  "no",   "any",   "some",  "up",
        "out",  "over", "after", "before", "between", "around", "both", "them", "they", "he", "she", "his",
        "her",  "if",   "when", "where", "what", "which", "who",   "how",  "want", "need", "like",  "let",
        "get",  "put",  "use",  "using", "via",  "now",   "again", "more", "less", "very", "too",   "bit",
        "little", "thing", "things", "stuff", "everything", "something"};
    return words;
}

/// The editing vocabulary: what people say, mapped to the words the catalog uses.
const std::map<std::string, std::vector<std::string>, std::less<>>& synonyms() {
    static const std::map<std::string, std::vector<std::string>, std::less<>> map{
        {"shorten", {"trim"}},
        {"shorter", {"trim"}},
        {"tighten", {"trim", "ripple"}},
        {"lengthen", {"trim"}},
        {"longer", {"trim"}},
        {"extend", {"trim"}},
        {"remove", {"delete", "ripple", "remove", "clear"}},
        {"delete", {"delete", "remove", "ripple", "clear"}},
        {"drop", {"delete", "ripple"}},
        {"erase", {"delete", "clear"}},
        {"gap", {"ripple", "gap", "close"}},
        {"gaps", {"ripple", "gap", "close"}},
        {"close", {"ripple", "gap"}},
        {"split", {"edit", "add", "split"}},
        {"cut", {"edit", "add", "split", "trim"}},
        {"razor", {"edit", "add", "split"}},
        {"slice", {"edit", "add", "split"}},
        {"bookmark", {"marker"}},
        {"chapter", {"marker", "chapter"}},
        {"chapters", {"marker", "chapter"}},
        {"note", {"marker", "comment"}},
        {"flag", {"marker"}},
        {"subtitle", {"captions", "caption", "cue"}},
        {"subtitles", {"captions", "caption", "cue"}},
        {"caption", {"captions", "cue"}},
        {"srt", {"captions"}},
        {"louder", {"volume", "gain", "track"}},
        {"quieter", {"volume", "gain", "track"}},
        {"softer", {"volume", "gain"}},
        {"volume", {"volume", "gain", "fader"}},
        {"gain", {"volume", "gain"}},
        {"db", {"volume", "gain"}},
        {"decibel", {"volume", "gain"}},
        {"decibels", {"volume", "gain"}},
        {"mute", {"mute", "state", "track"}},
        {"unmute", {"mute", "state", "track"}},
        {"solo", {"solo", "state", "track"}},
        {"lock", {"lock", "state", "track"}},
        {"unlock", {"lock", "state", "track"}},
        {"fade", {"fade"}},
        {"duck", {"duck", "auto"}},
        {"ducking", {"duck", "auto"}},
        {"loudness", {"normalize", "loudness"}},
        {"lufs", {"normalize", "loudness"}},
        {"normalise", {"normalize"}},
        // Looks are effects (clip.addEffect "wabcut.color.basic", "wabcut.vignette", ...).
        {"colour", {"color", "effect"}},
        {"color", {"color", "effect"}},
        {"grade", {"color", "grade", "effect"}},
        {"warm", {"color", "temperature", "effect"}},
        {"warmer", {"color", "temperature", "effect"}},
        {"cool", {"color", "temperature", "effect"}},
        {"cooler", {"color", "temperature", "effect"}},
        {"white", {"color", "saturation", "effect"}},
        {"black", {"color", "saturation", "effect"}},
        {"monochrome", {"color", "saturation", "effect"}},
        {"desaturate", {"color", "saturation", "effect"}},
        {"brightness", {"color", "exposure", "effect"}},
        {"brighten", {"color", "exposure", "effect"}},
        {"brighter", {"color", "exposure", "effect"}},
        {"darken", {"color", "exposure", "effect", "vignette"}},
        {"darker", {"color", "exposure", "effect"}},
        {"exposure", {"color", "exposure", "effect"}},
        {"stop", {"exposure", "effect"}},
        {"contrast", {"color", "contrast", "effect"}},
        {"saturation", {"color", "saturation", "effect"}},
        {"vignette", {"vignette", "effect"}},
        {"edges", {"vignette", "effect"}},
        {"blur", {"blur", "effect"}},
        {"sharpen", {"sharpen", "effect"}},
        {"crop", {"crop", "effect"}},
        {"effect", {"effect"}},
        {"label", {"label", "color"}},
        {"camera", {"multicam", "angle"}},
        {"cam", {"multicam", "angle"}},
        {"angle", {"multicam", "angle", "switch"}},
        {"angles", {"multicam", "angle", "switch"}},
        {"folder", {"bin"}},
        {"folders", {"bin"}},
        {"organize", {"bin", "move"}},
        {"organise", {"bin", "move"}},
        {"library", {"media", "bin"}},
        {"rating", {"media", "attributes"}},
        {"comment", {"comment", "marker"}},
        {"description", {"media", "attributes", "description"}},
        {"poster", {"media", "attributes", "poster"}},
        {"subclip", {"subclip", "media"}},
        {"speed", {"speed", "duration"}},
        {"slow", {"speed", "duration"}},
        {"slower", {"speed", "duration"}},
        {"faster", {"speed", "duration"}},
        {"fast", {"speed", "duration"}},
        {"slowmo", {"speed"}},
        {"transition", {"transition", "dissolve"}},
        {"dissolve", {"transition", "dissolve"}},
        {"crossfade", {"transition", "audio"}},
        {"title", {"title", "text"}},
        {"text", {"title", "text"}},
        {"silence", {"silences", "remove"}},
        {"silences", {"silences", "remove"}},
        {"pause", {"silences", "transcript"}},
        {"pauses", {"silences", "transcript"}},
        {"filler", {"transcript", "cleanup"}},
        {"um", {"transcript", "cleanup"}},
        {"uh", {"transcript", "cleanup"}},
        {"said", {"transcript"}},
        {"says", {"transcript"}},
        {"word", {"transcript"}},
        {"words", {"transcript"}},
        {"move", {"move", "update"}},
        {"shift", {"move", "shift", "update"}},
        {"nudge", {"move", "shift"}},
        {"resync", {"shift", "captions"}},
        {"disable", {"enable"}},
        {"hide", {"enable"}},
        {"reverse", {"speed", "reverse"}},
        {"freeze", {"hold", "frame"}},
        {"rename", {"rename", "update", "name", "attributes"}},
        {"named", {"name"}},
        {"called", {"name"}},
        {"name", {"rename", "name"}},
        {"fix", {"update", "edit", "correct"}},
        {"typo", {"update", "edit", "text"}},
        {"correct", {"update", "edit"}},
        {"change", {"update", "set"}},
        {"lower", {"volume", "gain"}},
        {"turn", {"volume"}},
        {"reduce", {"volume", "gain"}},
        {"bigger", {"size", "scale", "style"}},
        {"larger", {"size", "scale", "style"}},
        {"smaller", {"size", "scale", "style"}},
        {"font", {"font", "style"}},
        {"size", {"size", "style"}},
        {"clip", {"clip"}},
        {"clips", {"clip"}},
        {"track", {"track"}},
        {"tracks", {"track"}},
        {"marker", {"marker"}},
        {"markers", {"marker"}},
        {"how", {"summary"}},
        {"long", {"summary"}},
        {"list", {"list", "summary"}},
        // G.2: gaming highlights ("make a highlight reel of my best moments", "a TikTok of the clutch").
        {"highlight", {"highlight", "find"}},
        {"reel", {"reel", "highlight"}},
        {"montage", {"reel", "highlight"}},
        {"moment", {"highlight", "find"}},
        {"clutch", {"highlight"}},
        {"tiktok", {"short"}},
        {"vertical", {"short"}},
    };
    return map;
}

/// Request words that name a command family (the part of a command name before the dot): the request is about
/// that family's objects, so its commands rank higher ("delete the marker" -> marker.remove, not edit.clear).
const std::map<std::string, std::string, std::less<>>& domains() {
    static const std::map<std::string, std::string, std::less<>> map{
        {"marker", "marker"},       {"bookmark", "marker"},     {"chapter", "marker"},     {"caption", "captions"},
        {"subtitle", "captions"},   {"srt", "captions"},        {"bin", "bin"},            {"folder", "bin"},
        {"media", "media"},         {"subclip", "media"},       {"library", "media"},      {"multicam", "multicam"},
        {"angle", "multicam"},      {"camera", "multicam"},     {"cam", "multicam"},       {"transition", "transition"},
        {"dissolve", "transition"}, {"title", "title"},         {"mask", "mask"},          {"transcript", "transcript"},
        {"redaction", "redaction"}, {"keyframe", "keyframes"},
        {"highlight", "highlights"}, {"reel", "highlights"},      {"montage", "highlights"}, {"moment", "highlights"},
    };
    return map;
}

/// Lowercase words of `text`; camelCase and dotted names split ("timeline.rippleDelete" -> timeline ripple delete).
std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    auto flush = [&] {
        if (!current.empty()) {
            out.push_back(std::move(current));
            current.clear();
        }
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (std::isalnum(c)) {
            const bool upper = std::isupper(c) != 0;
            if (upper && !current.empty() && std::islower(static_cast<unsigned char>(current.back()))) {
                flush();
            }
            current.push_back(static_cast<char>(std::tolower(c)));
        } else {
            flush();
        }
    }
    flush();
    return out;
}

/// A crude English stem: "markers" -> "marker", "silences" -> "silence", "trimming" -> "trim", "added" -> "add".
std::string stem(std::string w) {
    auto cut = [&](std::string_view suffix, std::size_t keep) {
        if (w.size() >= suffix.size() + keep && w.ends_with(suffix)) {
            w.erase(w.size() - suffix.size());
            return true;
        }
        return false;
    };
    if (cut("ing", 3) || cut("ed", 3)) {
        // trimming -> trimm -> trim; not add, call, fill
        const char last = w.back();
        if (w.size() > 3 && last == w[w.size() - 2] && std::string_view("mptgn").find(last) != std::string_view::npos) {
            w.pop_back();
        }
        return w;
    }
    if (cut("ies", 3)) {
        w += 'y';
    } else if (!w.ends_with("ss")) {
        cut("s", 3);
    }
    return w;
}

struct Doc {
    std::set<std::string> strong; ///< name, title, alias words
    std::set<std::string> weak;   ///< description words
    std::string family;           ///< "marker" for marker.remove
};

} // namespace

std::size_t toolBudgetFor(int contextTokens) {
    if (contextTokens <= 0 || contextTokens > 16384) {
        return 0;
    }
    if (contextTokens <= 6144) {
        return 8;
    }
    if (contextTokens <= 8192) {
        return 12;
    }
    return 16;
}

std::vector<RoutedTool> rankTools(const std::vector<ToolSpec>& tools, const command::CommandRegistry& registry,
                                  std::string_view prompt, const RouteOptions& options) {
    // The documents: each tool's words, stemmed.
    std::vector<Doc> docs(tools.size());
    std::map<std::string, int> strongDf; // tools a word occurs in the name or title of
    std::map<std::string, int> anyDf;    // tools a word occurs in at all
    for (std::size_t i = 0; i < tools.size(); ++i) {
        Doc& d = docs[i];
        const command::CommandSpec* spec = registry.find(tools[i].name);
        std::string strongText = tools[i].name;
        if (spec) {
            d.family = spec->name.substr(0, spec->name.find('.'));
            strongText += " " + spec->name + " " + spec->title;
            for (const std::string& alias : spec->aliases) {
                strongText += " " + alias;
            }
        }
        for (std::string& w : words(strongText)) {
            d.strong.insert(stem(std::move(w)));
        }
        // Weak: the description, and the parameters' names, descriptions and allowed values ("wabcut.color.basic").
        std::string weakText = tools[i].description;
        if (const auto props = tools[i].parameters.find("properties"); props != tools[i].parameters.end() && props->is_object()) {
            for (const auto& [name, prop] : props->items()) {
                weakText += " " + name + " " + prop.value("description", std::string());
                if (const auto values = prop.find("enum"); values != prop.end() && values->is_array()) {
                    for (const Json& value : *values) {
                        weakText += value.is_string() ? " " + value.get<std::string>() : std::string();
                    }
                }
            }
        }
        for (std::string& w : words(weakText)) {
            if (!stopWords().contains(w)) {
                d.weak.insert(stem(std::move(w)));
            }
        }
        for (const std::string& w : d.strong) {
            ++strongDf[w];
        }
        for (const std::string& w : d.weak) {
            ++anyDf[w];
        }
        for (const std::string& w : d.strong) {
            if (!d.weak.contains(w)) {
                ++anyDf[w];
            }
        }
    }
    // The query: the request's words (weight 1) and what the vocabulary maps them to (0.8 each).
    std::map<std::string, double> query;
    std::set<std::string> families;
    auto want = [&](const std::string& word, double weight) { query[word] = std::max(query[word], weight); };
    for (const std::string& w : words(prompt)) {
        if (stopWords().contains(w) || std::all_of(w.begin(), w.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            continue;
        }
        const std::string s = stem(w);
        want(s, 1.0);
        if (const auto family = domains().find(s); family != domains().end()) {
            families.insert(family->second);
        }
        auto it = synonyms().find(w);
        if (it == synonyms().end()) {
            it = synonyms().find(s);
        }
        if (it != synonyms().end()) {
            for (const std::string& target : it->second) {
                want(stem(target), 0.8);
            }
        }
    }
    // Two timecodes ("remove 00:00:02:00 to 00:00:03:00") are a range: lift and extract work on ranges.
    static const std::regex timecode(R"(\d{1,2}:\d{2}:\d{2}[:;]\d{2})");
    const std::string text(prompt);
    if (std::distance(std::sregex_iterator(text.begin(), text.end(), timecode), std::sregex_iterator()) >= 2) {
        for (const char* word : {"range", "extract", "lift"}) {
            want(word, 0.8);
        }
    }
    // Score: words in the tool's name and title count five times as much as words of its description and parameters,
    // each weighted by how rare it is there; a tool that matches several of the request's words (a noun and a verb:
    // "marker" and "remove") ranks above one that matches a single word strongly.
    const double n = static_cast<double>(std::max<std::size_t>(tools.size(), 1));
    std::vector<RoutedTool> scored;
    for (std::size_t i = 0; i < tools.size(); ++i) {
        double score = 0.0;
        int matched = 0;
        for (const auto& [word, weight] : query) {
            if (docs[i].strong.contains(word)) {
                score += 3.0 * weight * std::log(1.0 + n / strongDf[word]);
                ++matched;
            } else if (docs[i].weak.contains(word)) {
                score += 0.6 * weight * std::log(1.0 + n / anyDf[word]);
                ++matched;
            }
        }
        if (score > 0.0) {
            score *= 1.0 + 0.35 * (matched - 1);
            if (families.contains(docs[i].family)) {
                score *= 1.8;
            }
            scored.push_back({tools[i].name, score});
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const RoutedTool& a, const RoutedTool& b) { return a.score > b.score; });

    std::vector<RoutedTool> out;
    for (const std::string& name : options.always) {
        if (out.size() < options.maxTools &&
            std::ranges::any_of(tools, [&](const ToolSpec& t) { return t.name == name; })) {
            out.push_back({name, 0.0});
        }
    }
    for (RoutedTool& t : scored) {
        if (out.size() >= options.maxTools) {
            break;
        }
        if (std::ranges::none_of(out, [&](const RoutedTool& o) { return o.name == t.name; })) {
            out.push_back(std::move(t));
        }
    }
    return out;
}

std::vector<ToolSpec> routeTools(const std::vector<ToolSpec>& tools, const command::CommandRegistry& registry,
                                 std::string_view prompt, const RouteOptions& options) {
    const std::vector<RoutedTool> ranked = rankTools(tools, registry, prompt, options);
    std::vector<ToolSpec> out;
    for (const ToolSpec& t : tools) {
        if (std::ranges::any_of(ranked, [&](const RoutedTool& r) { return r.name == t.name; })) {
            out.push_back(t);
        }
    }
    return out;
}

} // namespace wabcut::ai::agent
