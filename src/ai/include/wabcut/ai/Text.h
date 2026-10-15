#pragma once

// Small UTF-8 helpers for recognised text (no ICU).

#include <cstddef>
#include <string>
#include <string_view>

namespace wabcut::ai::text {

/// True if `s` is well-formed UTF-8 (no overlong forms, surrogates or code points above U+10FFFF).
bool isValidUtf8(std::string_view s);

/// Length of the well-formed UTF-8 sequence that starts at s[i]; 0 if the bytes there are malformed; -1 if they
/// are a valid start that the end of `s` cuts off (the rest may follow in more bytes).
int utf8SequenceAt(std::string_view s, std::size_t i);

/// Length of the longest prefix of `s` made of complete, well-formed UTF-8 sequences, ignoring an incomplete
/// sequence at the end (which may continue in the next token). Malformed bytes in the middle end the prefix.
std::size_t completeUtf8Prefix(std::string_view s);

/// `s` with every malformed byte replaced by U+FFFD.
std::string sanitizeUtf8(std::string_view s);

/// `s` without leading and trailing ASCII whitespace.
std::string_view trimAsciiSpace(std::string_view s);

/// True if `s` starts with a combining mark that cannot begin a word: combining diacritics (U+0300..036F),
/// Thai/Lao above and below vowels and tone marks, Khmer/Myanmar/Devanagari dependent signs, kana voicing
/// marks. A token that starts with one continues the previous word, whatever the language.
bool startsWithCombiningMark(std::string_view s);

/// True if `s` (valid UTF-8) is non-empty and consists only of punctuation that belongs to the word before it:
/// ASCII . , ! ? ; : ) ] } % ' " and the CJK / Thai / general punctuation that follows a word
/// (、 。 ， ！ ？ ： ； 」 』 ） … ฯ ๆ).
bool isTrailingPunctuation(std::string_view s);

} // namespace wabcut::ai::text
