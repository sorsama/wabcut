// Audio description (0.7, card 175): the gaps in the dialogue, the request that asks a vision model for a line per gap, and
// the fit of a line to its gap. See AudioDescription.h.

#include <wabcut/ai/dub/AudioDescription.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace wabcut::ai::dub {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/// The words of `text`, split at blanks.
std::vector<std::string> wordsOf(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (isSpace(c)) {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
    return out;
}

std::string join(const std::vector<std::string>& words, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to && i < words.size(); ++i) {
        if (!out.empty()) {
            out += ' ';
        }
        out += words[i];
    }
    return out;
}

bool endsSentence(const std::string& word) {
    if (word.empty()) {
        return false;
    }
    std::size_t end = word.size();
    while (end > 0 && (word[end - 1] == '"' || word[end - 1] == '\'' || word[end - 1] == ')')) {
        --end;
    }
    return end > 0 && (word[end - 1] == '.' || word[end - 1] == '!' || word[end - 1] == '?');
}

/// A word that cannot end a sentence ("and", "the"...): a cut right after it reads as broken.
bool dangling(const std::string& word) {
    std::string lower;
    for (const char c : word) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static const char* const kWords[] = {"a", "an", "the", "and", "or", "but", "with", "of", "to", "in", "on", "at", "as", "by", "for", "from", "while", "that", "his", "her", "their", "its"};
    return std::ranges::any_of(kWords, [&](const char* w) { return lower == w; });
}

std::string trimmedOfEnds(std::string text) {
    while (!text.empty() && (isSpace(text.back()) || text.back() == ',' || text.back() == ';' || text.back() == ':' || text.back() == '-')) {
        text.pop_back();
    }
    return text;
}

} // namespace

int wordCount(std::string_view text) { return static_cast<int>(wordsOf(text).size()); }

int wordBudget(Ticks length, const AdOptions& options) {
    const double seconds = std::max(0.0, length.toSeconds());
    const int fit = static_cast<int>(std::floor(seconds * std::max(0.1, options.wordsPerSecond)));
    return std::clamp(fit, 1, std::max(1, options.maxWords));
}

std::string fitDescription(std::string_view text, int maxWords) {
    // Quotes and list bullets a model likes to add are not part of a line to be spoken.
    std::string cleaned;
    for (const char c : text) {
        if (c == '"' || c == '*' || c == '`') {
            continue;
        }
        cleaned += isSpace(c) ? ' ' : c;
    }
    std::vector<std::string> words = wordsOf(cleaned);
    while (!words.empty() && (words.front() == "-" || words.front() == "\xE2\x80\xA2")) { // a bullet
        words.erase(words.begin());
    }
    if (words.empty()) {
        return {};
    }
    if (words.size() == 1) {
        std::string only;
        for (const char c : words[0]) {
            only += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (only == "NONE" || only == "NONE." || only == "N/A") {
            return {};
        }
    }
    const std::size_t limit = static_cast<std::size_t>(std::max(1, maxWords));
    if (words.size() > limit) {
        // The last sentence end that leaves a few words, else the limit itself.
        std::size_t cut = limit;
        for (std::size_t at = std::min(limit, words.size()); at >= 3; --at) {
            if (endsSentence(words[at - 1])) {
                cut = at;
                break;
            }
        }
        words.resize(cut);
        while (words.size() > 1 && dangling(words.back())) {
            words.pop_back();
        }
    }
    std::string out = trimmedOfEnds(join(words, 0, words.size()));
    if (out.empty()) {
        return {};
    }
    if (!endsSentence(out)) {
        out += '.';
    }
    return out;
}

std::vector<TimeRange> speechRanges(const ai::edit::SequenceTranscript& transcript, const AdOptions& options) {
    std::vector<TimeRange> out;
    for (const ai::edit::TimelineWord& word : transcript.words()) {
        if (word.timeline.isEmpty()) {
            continue;
        }
        if (!out.empty() && word.timeline.start - out.back().end() < options.joinGap) {
            const Ticks end = std::max(out.back().end(), word.timeline.end());
            out.back().duration = end - out.back().start;
        } else {
            out.push_back(word.timeline);
        }
    }
    return out;
}

namespace {

std::string wordsBefore(const ai::edit::SequenceTranscript& transcript, Ticks at, std::size_t count) {
    std::vector<std::string> words;
    for (const ai::edit::TimelineWord& word : transcript.words()) {
        if (word.timeline.end() <= at) {
            words.emplace_back(transcript.text(static_cast<std::size_t>(&word - transcript.words().data())));
        }
    }
    const std::size_t from = words.size() > count ? words.size() - count : 0;
    return join(words, from, words.size());
}

std::string wordsAfter(const ai::edit::SequenceTranscript& transcript, Ticks at, std::size_t count) {
    std::vector<std::string> words;
    for (const ai::edit::TimelineWord& word : transcript.words()) {
        if (word.timeline.start >= at) {
            words.emplace_back(transcript.text(static_cast<std::size_t>(&word - transcript.words().data())));
            if (words.size() >= count) {
                break;
            }
        }
    }
    return join(words, 0, words.size());
}

} // namespace

std::vector<AdGap> findGaps(std::span<const TimeRange> speech, Ticks duration, const AdOptions& options,
                            const ai::edit::SequenceTranscript* words) {
    std::vector<AdGap> out;
    // The silences, then each trimmed by the margins and cut into pieces no longer than maxGap.
    std::vector<TimeRange> silences;
    Ticks cursor{};
    for (const TimeRange& range : speech) {
        if (range.start > cursor) {
            silences.push_back(TimeRange::fromStartEnd(cursor, range.start));
        }
        cursor = std::max(cursor, range.end());
    }
    if (duration > cursor) {
        silences.push_back(TimeRange::fromStartEnd(cursor, duration));
    }
    for (const TimeRange& silence : silences) {
        // The margin is kept from speech, not from the start or the end of the sequence.
        const bool speechBefore = silence.start.value > 0;
        const bool speechAfter = silence.end() < duration;
        Ticks from = silence.start + (speechBefore ? options.margin : Ticks{});
        const Ticks to = silence.end() - (speechAfter ? options.margin : Ticks{});
        if (to - from < options.minGap) {
            continue;
        }
        const std::int64_t total = (to - from).value;
        const std::int64_t pieces = std::max<std::int64_t>(1, (total + options.maxGap.value - 1) / options.maxGap.value);
        const std::int64_t each = total / pieces;
        for (std::int64_t i = 0; i < pieces; ++i) {
            const Ticks start = from + Ticks{each * i};
            const Ticks length = i + 1 == pieces ? to - start : Ticks{each};
            if (length < options.minGap) {
                continue;
            }
            AdGap gap;
            gap.range = TimeRange{start, length};
            gap.maxWords = wordBudget(length, options);
            if (words) {
                gap.before = wordsBefore(*words, silence.start, options.contextWords);
                gap.after = wordsAfter(*words, silence.end(), options.contextWords);
            }
            out.push_back(std::move(gap));
        }
    }
    return out;
}

std::vector<AdGap> findGaps(const ai::edit::SequenceTranscript& transcript, Ticks duration, const AdOptions& options) {
    const std::vector<TimeRange> speech = speechRanges(transcript, options);
    return findGaps(speech, duration, options, &transcript);
}

std::string describeSystemPrompt(const DescribeOptions& options) {
    std::string prompt =
        "You write audio description for blind and low-vision viewers: one short line, spoken between the dialogue of a "
        "video, saying what a viewer who cannot see the picture would miss.\n"
        "Rules:\n"
        "- Describe only what is visible in the picture now: who is there, what they do, where it is, what changes, any "
        "text on screen. Say what is seen, not what it may mean.\n"
        "- Present tense, plain words, third person. No camera terms (no \"the camera pans\" or \"close-up\").\n"
        "- Never repeat or paraphrase the dialogue; the viewer hears it.\n"
        "- Do not name people unless their name appears on screen or in the notes below; say \"a man\", \"a woman\".\n"
        "- At most the number of words you are given, usually one or two short sentences. Shorter is better.\n"
        "- If nothing visible is worth saying, answer exactly: NONE\n"
        "- Answer with the line only: no quotes, no labels, no explanation.\n";
    prompt += "Write in the language with ISO 639-1 code \"" + options.language + "\".\n";
    if (!options.style.empty()) {
        prompt += "Notes from the person who makes the video: " + options.style + "\n";
    }
    return prompt;
}

ai::ChatRequest describeRequest(const AdGap& gap, std::vector<ai::ImagePart> frames, const DescribeOptions& options,
                                std::span<const std::string> previous) {
    ai::ChatRequest request;
    request.system = describeSystemPrompt(options);
    request.maxOutputTokens = options.maxOutputTokens;
    request.temperature = 0.3;
    ai::ChatMessage message;
    message.role = ai::Role::User;
    for (ai::ImagePart& frame : frames) {
        message.parts.emplace_back(std::move(frame));
    }
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%.1f", gap.range.duration.toSeconds());
    std::string facts = std::string("The silence is ") + seconds + " seconds long. Write at most " + std::to_string(gap.maxWords) +
                        " words.\n";
    if (frames.size() > 1) {
        facts += "The pictures are in time order across the silence.\n";
    }
    if (!gap.before.empty()) {
        facts += "Just before, someone says: \"" + gap.before + "\"\n";
    }
    if (!gap.after.empty()) {
        facts += "Just after, someone says: \"" + gap.after + "\"\n";
    }
    if (!previous.empty()) {
        facts += "Lines already written (do not repeat them):";
        const std::size_t from = previous.size() > 3 ? previous.size() - 3 : 0;
        for (std::size_t i = from; i < previous.size(); ++i) {
            facts += " \"" + previous[i] + "\"";
        }
        facts += "\n";
    }
    message.parts.emplace_back(ai::TextPart{std::move(facts)});
    request.messages.push_back(std::move(message));
    return request;
}

std::expected<DescribeResult, ai::AiError> describeGaps(std::span<const AdGap> gaps, ai::IChatModel& model,
                                                        const FrameSource& frames, const DescribeOptions& options,
                                                        const std::function<bool(double)>& progress, std::stop_token stop) {
    DescribeResult result;
    result.gaps = gaps.size();
    std::vector<std::string> written;
    const auto when = [](const AdGap& gap) {
        const auto whole = static_cast<long long>(gap.range.start.toSeconds());
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%lld:%02lld", whole / 60, whole % 60);
        return std::string(buffer);
    };
    for (std::size_t i = 0; i < gaps.size(); ++i) {
        if (stop.stop_requested() || (progress && !progress(static_cast<double>(i) / static_cast<double>(gaps.size())))) {
            break;
        }
        const AdGap& gap = gaps[i];
        std::vector<ai::ImagePart> pictures = frames ? frames(gap) : std::vector<ai::ImagePart>{};
        if (pictures.empty()) {
            result.warnings.push_back(when(gap) + ": no picture to describe");
            continue;
        }
        const ai::ChatRequest request = describeRequest(gap, std::move(pictures), options, written);
        auto response = model.chat(request, {}, stop);
        if (!response) {
            return std::unexpected(std::move(response.error()));
        }
        result.usage.inputTokens += response->usage.inputTokens;
        result.usage.outputTokens += response->usage.outputTokens;
        std::string text = fitDescription(response->message.text(), gap.maxWords);
        if (text.empty()) {
            result.warnings.push_back(when(gap) + ": nothing worth describing");
            continue;
        }
        written.push_back(text);
        result.lines.push_back(AdLine{gap.range.start, gap.range.duration, std::move(text), gap.maxWords});
    }
    if (progress) {
        progress(1.0);
    }
    return result;
}

} // namespace wabcut::ai::dub
