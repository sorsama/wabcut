#pragma once

// Caption languages for translation and dubbing (AI-12, docs/ai.md "Going global"): how fast a subtitle may be read,
// how long its lines may be and what the language is called. The figures are the Netflix Timed Text Style Guides'
// adult limits where those were checked (English 20 characters per second and 42 per line, Thai 17 and 35, Japanese
// 4 and 13, checked 2026-10-12; Thai tone marks and above/below vowels do not count, as captionDisplayLength), and
// conservative defaults elsewhere (17 and 42; CJK 9 and 16).

#include <span>
#include <string>
#include <string_view>

namespace wabcut::ai::translate {

struct LanguageRules {
    std::string_view code;       ///< ISO 639-1 (or "zh-Hant")
    std::string_view name;       ///< English name
    std::string_view nativeName; ///< the language's own name, for track names and chips
    double charsPerSecond = 17.0;
    int maxCharsPerLine = 42;
    int maxLines = 2;
    bool checked = false;        ///< the limits were checked against the style guide (else a default)
};

/// "th-TH" -> "th", "EN" -> "en", "zh_Hant" -> "zh-Hant"; "" stays "".
std::string normalizeLanguage(std::string_view code);

/// The rules of a language (normalized), or defaults (17 cps, 42 per line, 2 lines) named after the code.
LanguageRules rulesFor(std::string_view code);

/// The languages with a name and rules, for pickers.
std::span<const LanguageRules> knownLanguages();

/// Whether `code` is a plausible language tag: 2-3 letters, optionally "-" and a 2-4 letter or 2-3 digit subtag.
bool isLanguageTag(std::string_view code);

} // namespace wabcut::ai::translate
