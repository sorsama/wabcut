#pragma once

// The languages a recording can be spoken in, as the on-device speech recogniser (whisper.cpp) takes them (card 073):
// an ISO 639-1 code (three letters for Cantonese), its English and own name, whether it is written right to left, and
// how much audio the recogniser was trained on. The Transcript tab's language picker, transcript.transcribe's
// "language" and the CLI's --language all use this one list. Pure.
//
// The tiers come from the amount of training audio per language in the Whisper paper (Radford et al., "Robust Speech
// Recognition via Large-Scale Weak Supervision", 2022, Appendix E: https://cdn.openai.com/papers/whisper.pdf): Strong
// languages had roughly 650 hours or more (and Whisper's word error rate there is low from the Base model up), Fair
// ones 100 to 600 hours, Limited ones less. They advise which model to pick; they are not a promise of accuracy.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wabcut::ai {

enum class SpokenTier : unsigned char { Strong, Fair, Limited };

struct SpokenLanguage {
    std::string_view code;       ///< "he", "id", "uk", ...: what whisper takes
    std::string_view name;       ///< English name
    std::string_view nativeName; ///< in its own script ("" when unknown)
    bool rtl;                    ///< written right to left (Arabic, Hebrew, Persian, Urdu, Yiddish, Pashto, Sindhi)
    SpokenTier tier;
};

/// Every language the recogniser knows, in the order of its own table (English first, then by amount of training audio).
std::span<const SpokenLanguage> spokenLanguages();

/// The language for `code` ("he", "HE", "he-IL", "iw" for Hebrew, "nb" for Norwegian...), or nullptr. "auto" is not a
/// language: see isSpokenLanguageChoice().
const SpokenLanguage* findSpokenLanguage(std::string_view code);

/// Whether `code` is "auto" or a language of the list: what transcript.transcribe's "language" may be.
bool isSpokenLanguageChoice(std::string_view code);

/// The code whisper is given for `code` (normalised: "he-IL" -> "he", "iw" -> "he"); "auto" stays "auto"; "" when it is
/// neither.
std::string spokenLanguageCode(std::string_view code);

/// Whether text in `code` runs right to left (false for "auto" and unknown codes).
bool isRightToLeftLanguage(std::string_view code);

/// A one-sentence note on how well a Whisper model handles `code`: "" for "auto" and unknown codes and for Strong
/// languages with any model but "tiny"; otherwise which model size to use. `modelId` is a model library id ("base",
/// "small", "large-v3-turbo-q5_0", ... or "" for the automatic one).
std::string spokenLanguageAdvice(std::string_view code, std::string_view modelId);

/// Whether the model (a ModelStore id) can translate speech to English: false for the English-only models (".en") and
/// "tiny" is allowed. Whisper's large-v3-turbo was trained for transcription and translates poorly: it can, but see
/// translationAdvice().
bool modelCanTranslate(std::string_view modelId);

/// 0.7 (card 111): a note on transcribing a recording that switches language (auto-detect per stretch) with `modelId`:
/// why to pick a larger or multilingual model, and that it takes longer; never empty.
std::string mixedLanguagesAdvice(std::string_view modelId);

/// A note on translating to English with `modelId`: "" for a model that does it well (small, medium, large-v3), else why
/// to pick another.
std::string translationAdvice(std::string_view modelId);

} // namespace wabcut::ai
