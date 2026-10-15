#pragma once

// Builds words from recogniser tokens, language-aware (docs/ai.md: Thai has no spaces, never split on whitespace).
//
// Whisper tokens are byte-level BPE pieces: one token may end in the middle of a multi-byte UTF-8 character and
// the next token completes it. Bytes are therefore joined first into units of complete characters, whose times
// span every token that contributed bytes, and only then grouped into words:
//   * space-delimited scripts (English, ...): a unit that starts with whitespace starts a new word;
//   * scripts written without spaces (Thai, Lao, Khmer, Myanmar, Chinese, Japanese, Tibetan): every unit stays
//     a word of its own. Proper word segmentation needs a dictionary segmenter (ICU BreakIterator);
//     TODO(ai): use one when the project gains ICU, as a refinement of these units, never by whitespace;
// and in both cases punctuation-only units join the word before them.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

/// One recogniser token: raw bytes (possibly an incomplete UTF-8 sequence) and its times in centiseconds
/// (whisper's 10 ms unit) from the start of the audio it was given.
struct RecognizedToken {
    std::string bytes;
    std::int64_t t0 = 0;
    std::int64_t t1 = 0;
    float probability = 0.0f;
};

struct RecognizedSegment {
    std::int64_t t0 = 0; ///< centiseconds
    std::int64_t t1 = 0;
    std::vector<RecognizedToken> tokens; ///< text tokens only (no special or timestamp tokens)
    bool speakerTurnNext = false;
};

struct BuiltWord {
    std::string text;    ///< valid UTF-8, trimmed
    std::int64_t t0 = 0; ///< centiseconds, t0 <= t1
    std::int64_t t1 = 0;
    float confidence = 0.0f;
};

enum class WordSplit {
    Whitespace, ///< words are separated by spaces
    PerUnit,    ///< no spaces between words: each complete-character unit is a word (see the header comment)
};

/// How `language` (ISO 639-1, e.g. "en", "th"; "" or "auto" = unknown, treated as Whitespace) separates words.
WordSplit wordSplitFor(std::string_view language);

/// Words of one segment, in order. Never splits a UTF-8 character; bytes that never form a valid character
/// become U+FFFD. Word times are clamped into the segment's [t0, t1] and kept non-decreasing.
std::vector<BuiltWord> buildWords(const RecognizedSegment& segment, WordSplit split);

/// The segment's text: its tokens' bytes joined (UTF-8 sanitised) and trimmed.
std::string segmentText(const RecognizedSegment& segment);

} // namespace wabcut::ai
