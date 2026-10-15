#pragma once

// Caption text (C.7): display length and line breaking, with a seam for real line-break rules.
//
// Where a line may break is language business: spaces in most scripts, but Thai, Lao, Khmer, Burmese, Chinese and
// Japanese are written without spaces between words. The model stays pure C++, so it asks a LineBreaker: the app
// installs one over ICU or Qt's line-break iterator (QTextBoundaryFinder::Line), which knows Thai words; the model's
// own simpleLineBreaker() only breaks at spaces and after hyphens. Transcript-built captions also pass their word
// boundaries, so they break between words even with the simple breaker.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

class LineBreaker {
public:
    virtual ~LineBreaker() = default;
    /// Byte offsets into `text` (UTF-8) where a new line may start, ascending, inside (0, text.size()). Spaces at a
    /// break are dropped from both lines.
    virtual std::vector<std::size_t> breakOpportunities(std::string_view text, std::string_view language) const = 0;
};

/// Breaks after runs of spaces and after hyphens and dashes; never inside unspaced text.
const LineBreaker& simpleLineBreaker();

/// Display width of UTF-8 text: code points, except combining marks (Thai vowels above/below and tone marks, ...)
/// and zero-width characters, which take no cell. Invalid bytes count one each.
std::size_t captionDisplayLength(std::string_view text);

/// Whether `language` (ISO 639-1) writes words without spaces between them (th, lo, km, my, zh, ja).
bool isUnspacedLanguage(std::string_view language);

/// Wraps `text` (its newlines become spaces, or nothing in an unspaced language) into at most `maxLines` lines of at
/// most `maxCharsPerLine` display characters, breaking only at `breaker`'s opportunities and the `extra` byte offsets:
/// the fewest lines that fit, then the most balanced (the longest line as short as possible), preferring breaks
/// after punctuation and a first line no longer than the rest. A single unbreakable piece longer than a line may
/// stand on a line of its own. nullopt when the text does not fit.
std::optional<std::vector<std::string>> breakCaptionLines(std::string_view text, std::string_view language,
                                                          int maxCharsPerLine, int maxLines, const LineBreaker& breaker,
                                                          std::span<const std::size_t> extra = {});

/// The lines joined with '\n' (`text` flattened onto one line when breakCaptionLines finds no fit).
std::string wrapCaptionText(std::string_view text, std::string_view language, int maxCharsPerLine, int maxLines,
                            const LineBreaker& breaker, std::span<const std::size_t> extra = {});

/// `text` on one line: newlines become a space (nothing in an unspaced language), runs of spaces one space, trimmed.
std::string flattenCaptionText(std::string_view text, std::string_view language);

/// Valid UTF-8 without NUL or control characters other than '\n' and '\t'.
bool isValidCaptionText(std::string_view text);

} // namespace wabcut
