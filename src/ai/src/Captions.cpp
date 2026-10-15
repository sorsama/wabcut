#include <wabcut/ai/Captions.h>

#include <wabcut/ai/Text.h>
#include <wabcut/ai/WordBuilder.h>

#include <cstdio>

namespace wabcut::ai {

namespace {

/// "HH:MM:SS<sep>mmm", rounded to the nearest millisecond; negative times clamp to 0.
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
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

bool isAsciiAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/// No blank lines inside a cue (a blank line ends it in both formats).
std::string singleBlock(std::string s) {
    for (char& c : s) {
        if (c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return s;
}

} // namespace

std::string segmentWordsText(const Transcript& transcript, const TranscriptSegment& segment) {
    if (segment.wordCount == 0) {
        return std::string(text::trimAsciiSpace(segment.text));
    }
    const bool spaces = wordSplitFor(transcript.language) == WordSplit::Whitespace;
    std::string out;
    for (std::size_t i = segment.firstWord; i < segment.firstWord + segment.wordCount && i < transcript.words.size(); ++i) {
        const std::string& word = transcript.words[i].text;
        if (!out.empty() && !word.empty() && !text::isTrailingPunctuation(word) &&
            (spaces || (isAsciiAlnum(out.back()) && isAsciiAlnum(word.front())))) {
            out += ' '; // languages without spaces still space the Latin words written inside them
        }
        out += word;
    }
    return out;
}

std::string toSrt(const Transcript& transcript) {
    std::string out;
    int index = 0;
    for (const TranscriptSegment& segment : transcript.segments) {
        const std::string line = singleBlock(segmentWordsText(transcript, segment));
        if (line.empty()) {
            continue;
        }
        out += std::to_string(++index) + "\n" + cueTime(segment.t0, ',') + " --> " + cueTime(segment.t1, ',') + "\n" +
               line + "\n\n";
    }
    return out;
}

std::string toVtt(const Transcript& transcript) {
    std::string out = "WEBVTT\n\n";
    for (const TranscriptSegment& segment : transcript.segments) {
        const std::string line = singleBlock(segmentWordsText(transcript, segment));
        if (line.empty()) {
            continue;
        }
        out += cueTime(segment.t0, '.') + " --> " + cueTime(segment.t1, '.') + "\n" + escapeVtt(line) + "\n\n";
    }
    return out;
}

} // namespace wabcut::ai
