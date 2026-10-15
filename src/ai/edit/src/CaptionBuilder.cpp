#include <wabcut/ai/edit/CaptionBuilder.h>

#include <wabcut/ai/Text.h>
#include <wabcut/ai/WordBuilder.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>

namespace wabcut::ai::edit {

namespace {

bool isAsciiAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool endsSentence(std::string_view word) {
    for (const std::string_view end : {".", "?", "!", "\xE3\x80\x82", "\xEF\xBC\x9F", "\xEF\xBC\x81", "\xE2\x80\xA6"}) {
        if (word.ends_with(end)) {
            return true;
        }
    }
    return false;
}

bool endsClause(std::string_view word) {
    return endsSentence(word) || word.ends_with(",") || word.ends_with(";") || word.ends_with(":") ||
           word.ends_with("\xEF\xBC\x8C") || word.ends_with("\xE3\x80\x81");
}

/// Same as Captions.cpp: "HH:MM:SS<sep>mmm", the nearest millisecond, negative times clamped to 0.
std::string cueTime(Ticks t, char separator) {
    const auto ms = mulDiv(t.value, 1000, kTicksPerSecond, Rounding::Nearest).value_or(0);
    const std::int64_t total = ms < 0 ? 0 : ms;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld%c%03lld", static_cast<long long>(total / 3'600'000),
                  static_cast<long long>(total / 60'000 % 60), static_cast<long long>(total / 1000 % 60), separator,
                  static_cast<long long>(total % 1000));
    return buf;
}

std::string escapeVtt(std::string_view s) {
    std::string out;
    for (const char c : s) {
        out += c == '&' ? std::string("&amp;") : c == '<' ? std::string("&lt;") : c == '>' ? std::string("&gt;") : std::string(1, c);
    }
    return out;
}

/// Cue text without blank lines or carriage returns (a blank line would end the cue in both formats).
std::string cueText(std::string_view text) {
    std::string out;
    std::string_view rest = text;
    while (true) {
        const std::size_t nl = rest.find('\n');
        std::string line(text::trimAsciiSpace(rest.substr(0, nl)));
        std::erase(line, '\r');
        if (!line.empty()) {
            out += (out.empty() ? "" : "\n") + line;
        }
        if (nl == std::string_view::npos) {
            break;
        }
        rest = rest.substr(nl + 1);
    }
    return out;
}

} // namespace

std::size_t displayLength(std::string_view s) {
    std::size_t length = 0;
    for (std::size_t i = 0; i < s.size();) {
        const int n = text::utf8SequenceAt(s, i);
        const std::size_t step = n > 0 ? static_cast<std::size_t>(n) : 1;
        if (!text::startsWithCombiningMark(s.substr(i, step))) {
            ++length;
        }
        i += step;
    }
    return length;
}

std::string joinWords(std::span<const std::string> words, std::string_view language) {
    const bool spaces = wordSplitFor(language) == WordSplit::Whitespace;
    std::string out;
    for (const std::string& word : words) {
        if (!out.empty() && !word.empty() && !text::isTrailingPunctuation(word) &&
            (spaces || (isAsciiAlnum(out.back()) && isAsciiAlnum(word.front())))) {
            out += ' ';
        }
        out += word;
    }
    return out;
}

std::optional<std::vector<std::string>> breakLines(std::span<const std::string> words, std::string_view language,
                                                   int maxCharsPerLine, int maxLines) {
    const std::size_t n = words.size();
    if (n == 0) {
        return std::vector<std::string>{};
    }
    const auto max = static_cast<std::size_t>(std::max(1, maxCharsPerLine));
    // len[i][j]: display length of words i..j-1 on one line, built from each word's length and whether joinWords
    // puts a space before it.
    const auto lineOf = [&](std::size_t i, std::size_t j) { return joinWords(words.subspan(i, j - i), language); };
    const bool spaces = wordSplitFor(language) == WordSplit::Whitespace;
    std::vector<std::size_t> wordLength(n);
    std::vector<std::size_t> spaceBefore(n, 0);
    for (std::size_t k = 0; k < n; ++k) {
        wordLength[k] = displayLength(words[k]);
        if (k > 0 && !words[k].empty() && !words[k - 1].empty() && !text::isTrailingPunctuation(words[k]) &&
            (spaces || (isAsciiAlnum(words[k - 1].back()) && isAsciiAlnum(words[k].front())))) {
            spaceBefore[k] = 1;
        }
    }
    std::vector<std::vector<std::size_t>> len(n + 1, std::vector<std::size_t>(n + 1, 0));
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j <= n; ++j) {
            len[i][j] = len[i][j - 1] + wordLength[j - 1] + (j - 1 > i ? spaceBefore[j - 1] : 0);
        }
    }
    // Fewest lines first: a single over-long word may stand on a line of its own.
    for (int lines = 1; lines <= std::max(1, maxLines); ++lines) {
        // best[k][i]: the smallest possible longest line for words i..n-1 in k lines, and the first break.
        struct Cell {
            std::size_t longest = static_cast<std::size_t>(-1);
            std::size_t next = 0;
            int score = 0; ///< tie-break: breaks after punctuation, shorter first lines
        };
        std::vector<std::vector<Cell>> best(static_cast<std::size_t>(lines) + 1, std::vector<Cell>(n + 1));
        best[0][n] = Cell{0, n, 0};
        for (int k = 1; k <= lines; ++k) {
            for (std::size_t i = n; i-- > 0;) {
                Cell& cell = best[static_cast<std::size_t>(k)][i];
                for (std::size_t j = i + 1; j <= n; ++j) {
                    const std::size_t here = len[i][j];
                    if (here > max && j > i + 1) {
                        break;
                    }
                    const Cell& rest = best[static_cast<std::size_t>(k) - 1][j];
                    if (rest.longest == static_cast<std::size_t>(-1)) {
                        continue;
                    }
                    const std::size_t longest = std::max(here, rest.longest);
                    int score = rest.score;
                    if (j < n) {
                        score += endsClause(words[j - 1]) ? 4 : 0;
                        score += here <= len[j][n] ? 1 : 0; // a shorter (or equal) first line
                    }
                    if (longest < cell.longest || (longest == cell.longest && score > cell.score)) {
                        cell = Cell{longest, j, score};
                    }
                }
            }
        }
        const Cell& start = best[static_cast<std::size_t>(lines)][0];
        if (start.longest == static_cast<std::size_t>(-1)) {
            continue;
        }
        // Every line must fit unless it is a single word.
        std::vector<std::string> out;
        bool fits = true;
        std::size_t i = 0;
        for (int k = lines; k > 0 && i < n; --k) {
            const std::size_t j = best[static_cast<std::size_t>(k)][i].next;
            if (len[i][j] > max && j > i + 1) {
                fits = false;
            }
            out.push_back(lineOf(i, j));
            i = j;
        }
        if (fits && i == n) {
            return out;
        }
    }
    return std::nullopt;
}

namespace {

/// Lines for `words`: the model's line breaking over their joined text, breaking between words and at the breaker's
/// opportunities (CaptionText.h).
std::optional<std::vector<std::string>> breakWithBreaker(std::span<const std::string> words, std::string_view language,
                                                         const CaptionOptions& options) {
    const std::string text = joinWords(words, language);
    std::vector<std::size_t> starts;
    for (std::size_t k = 1; k < words.size(); ++k) {
        const std::string upTo = joinWords(words.subspan(0, k + 1), language);
        starts.push_back(upTo.size() - words[k].size());
    }
    return breakCaptionLines(text, language, options.maxCharsPerLine, options.maxLines, *options.breaker, starts);
}

std::optional<std::vector<std::string>> layoutLines(std::span<const std::string> words, std::string_view language,
                                                    const CaptionOptions& options) {
    return options.breaker ? breakWithBreaker(words, language, options)
                           : breakLines(words, language, options.maxCharsPerLine, options.maxLines);
}

} // namespace

std::string captionText(std::span<const std::string> words, std::string_view language, const CaptionOptions& options) {
    std::string text;
    for (const std::string& line : layoutLines(words, language, options).value_or(
             std::vector<std::string>{joinWords(words, language)})) {
        text += (text.empty() ? "" : "\n") + line;
    }
    return text;
}

CaptionTrack buildCaptions(const SequenceTranscript& st, const CaptionOptions& options, std::string name) {
    CaptionTrack track;
    track.name = std::move(name);
    std::map<std::string, std::size_t> languages;
    for (std::size_t i = 0; i < st.size(); ++i) {
        ++languages[std::string(st.language(i))];
    }
    for (const auto& [language, count] : languages) {
        if (track.language.empty() || count > languages[track.language]) {
            track.language = language;
        }
    }
    struct Pending {
        std::size_t first = 0;
        std::vector<std::string> words;
        std::vector<SpeakerId> speakers;
        Ticks start;
        Ticks end;
    };
    std::vector<Pending> cues;
    const std::size_t capacity =
        static_cast<std::size_t>(std::max(1, options.maxCharsPerLine)) * static_cast<std::size_t>(std::max(1, options.maxLines));
    for (std::size_t i = 0; i < st.size(); ++i) {
        const TimelineWord& w = st[i];
        const std::string word(st.text(i));
        bool fresh = cues.empty();
        if (!fresh) {
            Pending& cur = cues.back();
            const TimelineWord& prev = st[i - 1];
            std::vector<std::string> trial = cur.words;
            trial.push_back(word);
            const bool speakerChange =
                options.breakOnSpeakerChange &&
                ((w.speakerId.isValid() && prev.speakerId.isValid()) ? w.speakerId != prev.speakerId
                                                                     : (w.speaker && prev.speaker && *w.speaker != *prev.speaker));
            const bool pause = w.timeline.start - prev.timeline.end() > options.maxPause;
            const bool tooLong = w.timeline.end() - cur.start > options.maxDuration;
            const bool sentence = options.splitOnPunctuation && endsSentence(cur.words.back()) &&
                                  displayLength(joinWords(cur.words, track.language)) * 2 >= capacity;
            const std::size_t trialLength = displayLength(joinWords(trial, track.language));
            const bool fits = trialLength <= static_cast<std::size_t>(options.maxCharsPerLine) ||
                              (trialLength <= capacity + static_cast<std::size_t>(options.maxLines) &&
                               layoutLines(trial, track.language, options).has_value());
            fresh = speakerChange || pause || tooLong || sentence || !fits;
        }
        if (fresh) {
            cues.push_back(Pending{i, {}, {}, w.timeline.start, w.timeline.end()});
        }
        Pending& cur = cues.back();
        cur.words.push_back(word);
        cur.speakers.push_back(w.speakerId);
        cur.end = std::max(cur.end, w.timeline.end());
    }
    for (std::size_t k = 0; k < cues.size(); ++k) {
        const Pending& p = cues[k];
        const Ticks next = k + 1 < cues.size() ? cues[k + 1].start : Ticks::max();
        Ticks end = std::max(p.end, std::min(p.start + options.minDuration, next));
        end = std::min(end, next);
        if (options.minGap.value > 0 && next != Ticks::max() && end > next - options.minGap && next - options.minGap > p.start) {
            end = next - options.minGap;
        }
        if (end <= p.start) {
            continue;
        }
        std::string text = captionText(p.words, track.language, options);
        if (!track.cues.empty() && track.cues.back().end() > p.start) {
            continue; // overlapping speech on two tracks: the earlier cue wins
        }
        // The speaker of most of the cue's words (the first of them on a tie).
        SpeakerId speaker;
        std::size_t best = 0;
        for (const SpeakerId s : p.speakers) {
            const auto n = static_cast<std::size_t>(std::count(p.speakers.begin(), p.speakers.end(), s));
            if (s.isValid() && n > best) {
                best = n;
                speaker = s;
            }
        }
        track.cues.push_back(CaptionCue{p.start, end - p.start, std::move(text), speaker});
    }
    return track;
}

std::string toSrt(const CaptionTrack& captions) {
    std::string out;
    int index = 0;
    for (const CaptionCue& cue : captions.cues) {
        const std::string text = cueText(cue.text);
        if (text.empty()) {
            continue;
        }
        out += std::to_string(++index) + "\n" + cueTime(cue.start, ',') + " --> " + cueTime(cue.end(), ',') + "\n" +
               text + "\n\n";
    }
    return out;
}

std::string toVtt(const CaptionTrack& captions) {
    std::string out = "WEBVTT\n\n";
    for (const CaptionCue& cue : captions.cues) {
        const std::string text = cueText(cue.text);
        if (text.empty()) {
            continue;
        }
        out += cueTime(cue.start, '.') + " --> " + cueTime(cue.end(), '.') + "\n" + escapeVtt(text) + "\n\n";
    }
    return out;
}

} // namespace wabcut::ai::edit
