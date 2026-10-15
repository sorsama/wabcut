#pragma once

// Caption translation (AI-12, docs/ai.md "Going global"). A caption track becomes one new track per target language,
// cue timing kept and re-segmented to the target's reading speed (Segmentation.h):
//   - the source text is the cue text with the corrections layer applied (transcript.correctWord) and every word inside
//     an audio redaction as "[bleep]", before anything is translated;
//   - "[bleep]" never reaches a translator as content: the text between bleeps is translated, the bleeps are kept in
//     place (an LLM sees them as markers and its answer must keep exactly as many; otherwise the pieces between them
//     are translated one by one), so a translation can never put the redacted word back;
//   - a translator is the user's cloud chat model (LlmTranslator: guarded by consent, the spend cap and the no-cloud
//     lock through AiPlatform; caption text only, with the glossary and the tone) or a local model (the wabcut-embed
//     helper, OPUS-MT, see LocalTranslator.h);
//   - the glossary's terms are checked in the result; terms a translation missed are reported, not silently fixed.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/translate/Segmentation.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::edit {
class TranscriptProvider;
}

namespace wabcut::ai::translate {

inline constexpr std::string_view kBleep = "[bleep]";

struct GlossaryTerm {
    std::string source;
    std::string target; ///< the same as source: keep it as it is (a name, a brand)

    bool operator==(const GlossaryTerm&) const = default;
};

enum class Tone { Neutral, Casual, Formal, Playful };
std::string_view toString(Tone tone);
std::optional<Tone> toneFromString(std::string_view text);

/// The text of every cue of `track` as it should be translated: cue text with the corrections layer applied and the
/// redacted words as "[bleep]" (from the sequence transcript, when `transcripts` has the words under a cue; a cue
/// whose text is the transcript's becomes the corrected text, an edited cue gets each corrected or redacted word
/// replaced where it appears). Without transcripts, the cue text as it is.
/// The texts of `track` as they may leave the computer: corrections applied, redacted words as "[bleep]"; a cue over
/// redacted speech that the transcript does not account for (typed, imported, no transcript) is "[bleep]" as a whole.
struct SourceTexts {
    std::vector<std::string> texts;
    std::size_t corrected = 0; ///< words replaced by their correction
    std::size_t redacted = 0;  ///< "[bleep]" markers in the texts (redacted words, never translated)
};
SourceTexts sourceTexts(const ProjectState& state, SequenceId sequence, const CaptionTrack& track,
                        const edit::TranscriptProvider* transcripts);

/// The pieces of `text` around "[bleep]": "a [bleep] b" -> {"a", "b"} with bleeps = 1 between them. Empty pieces are
/// kept (a cue that starts with a bleep has an empty first piece).
struct BleepSplit {
    std::vector<std::string> pieces; ///< bleeps + 1 pieces
    std::size_t bleeps = 0;
};
BleepSplit splitBleeps(std::string_view text);
/// The pieces joined back with " [bleep] " (no space next to an empty piece or in an unspaced language).
std::string joinBleeps(const std::vector<std::string>& pieces, std::string_view language);
std::size_t countBleeps(std::string_view text);

struct TranslateOptions {
    std::string sourceLanguage;           ///< "" = the track's
    std::vector<GlossaryTerm> glossary;
    Tone tone = Tone::Neutral;
    std::string notes;                    ///< extra instructions for a model (who speaks, the audience), at most 500 bytes
    std::size_t batch = 40;               ///< cues per translator call
    bool markersKept = true;              ///< the translator keeps "[bleep]" markers (a model); false: the pieces
                                          ///< between bleeps are sent on their own (a local model)
    std::optional<double> charsPerSecond; ///< else the target language's
    TimingRules timing;
};

/// A translator for one language pair (the caller opens it: local or cloud).
using TranslatorOpener =
    std::function<std::expected<std::unique_ptr<ITranslator>, AiError>(std::string_view source, std::string_view target)>;

struct LanguageResult {
    std::string language;
    CaptionTrack track;                  ///< the new track (id invalid: added by the caller)
    SegmentationReport segmentation;
    std::vector<std::string> warnings;
    std::size_t characters = 0;          ///< source characters sent to the translator
    std::size_t retried = 0;             ///< cues re-translated piece by piece (lost or added bleeps)
    std::size_t untranslated = 0;        ///< cues whose translation failed (kept in the source language, flagged)
    std::vector<std::size_t> missedTerms; ///< indices into the glossary
};

/// Translates `source` (its cues and `texts`, from sourceTexts) into `language`: the track named
/// "<source name> (<native language name>)" in the target language, the source style (Thai, Lao, Khmer, Burmese:
/// the "Thai-friendly" preset's font and line spacing).
std::expected<LanguageResult, AiError> translateTrack(const CaptionTrack& source, const std::vector<std::string>& texts,
                                                      std::string_view language, ITranslator& translator,
                                                      const TranslateOptions& options, const LineBreaker& breaker,
                                                      const std::function<bool(std::size_t done, std::size_t total)>& progress,
                                                      std::stop_token stop);

/// The context line a model gets with every batch: what the text is, the tone, the glossary, the line limits.
std::string translationContext(std::string_view sourceLanguage, std::string_view targetLanguage, const TranslateOptions& options);

/// ITranslator over the user's chat model: JSON in, JSON out, every id back, bleeps kept as markers. The model is
/// guarded by the platform (consent, spend cap, no-cloud lock, request log) when it comes from AiPlatform::chat.
class LlmTranslator final : public ITranslator {
public:
    explicit LlmTranslator(std::unique_ptr<IChatModel> model, int maxOutputTokens = 8192);
    std::expected<TranslationResult, AiError> translate(const TranslationRequest& request, std::stop_token stop) override;
    const ModelSpec& spec() const { return m_model->spec(); }

private:
    std::unique_ptr<IChatModel> m_model;
    int m_maxOutputTokens;
};

/// The chat request LlmTranslator sends for `request` (exposed for tests and the cost preview).
ChatRequest translationChatRequest(const TranslationRequest& request, int maxOutputTokens);

} // namespace wabcut::ai::translate
