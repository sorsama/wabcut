#include <wabcut/ai/Transcript.h>

#include <wabcut/ai/Text.h>

namespace wabcut::ai {

std::expected<void, Error> Transcript::validate() const {
    auto bad = [](std::string message) { return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)}); };
    for (std::size_t i = 0; i < words.size(); ++i) {
        const TranscriptWord& w = words[i];
        if (w.t1 < w.t0) {
            return bad("word " + std::to_string(i) + " ends before it starts");
        }
        if (i > 0 && w.t0 < words[i - 1].t0) {
            return bad("word " + std::to_string(i) + " starts before the word before it");
        }
        if (!text::isValidUtf8(w.text)) {
            return bad("word " + std::to_string(i) + " is not valid UTF-8");
        }
        if (!(w.confidence >= 0.0f && w.confidence <= 1.0f)) {
            return bad("word " + std::to_string(i) + " has a confidence outside 0..1");
        }
    }
    std::size_t nextWord = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const TranscriptSegment& s = segments[i];
        if (s.t1 < s.t0) {
            return bad("segment " + std::to_string(i) + " ends before it starts");
        }
        if (i > 0 && s.t0 < segments[i - 1].t0) {
            return bad("segment " + std::to_string(i) + " starts before the segment before it");
        }
        if (s.firstWord != nextWord || s.wordCount > words.size() - s.firstWord) {
            return bad("segment " + std::to_string(i) + " does not cover the next words in order");
        }
        if (!text::isValidUtf8(s.text)) {
            return bad("segment " + std::to_string(i) + " is not valid UTF-8");
        }
        nextWord = s.firstWord + s.wordCount;
    }
    if (!segments.empty() && nextWord != words.size()) {
        return bad("words after the last segment");
    }
    if (!text::isValidUtf8(language) || !text::isValidUtf8(source.path)) {
        return bad("language or source path is not valid UTF-8");
    }
    return {};
}

} // namespace wabcut::ai
