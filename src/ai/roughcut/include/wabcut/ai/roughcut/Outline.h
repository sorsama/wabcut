#pragma once

// Loose outlines into beats with the user's own model (AI-11, optional, bring your own key). An outline bullet such
// as "- why we raised prices" rarely matches what was said word for word. interpretOutline() sends the outline and,
// for each bullet, the few transcript sentences the local index ranks closest to it (text only: no media, file names
// or pictures) and asks the model which words of the transcript say each bullet, in order. Its answer becomes ordinary
// beats that the local planner aligns, judges and assembles as usual, so nothing the model says reaches the timeline
// unchecked.
//
// The model comes from AiPlatform::chat (guarded: offline mode and the project's no-cloud lock, consent, the monthly
// spend cap, the request log, checked again before each request). Callers skip it entirely for a no-cloud project, and
// the rough cut works without it.

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Capability.h>
#include <wabcut/ai/roughcut/Script.h>
#include <wabcut/ai/roughcut/Speech.h>

#include <expected>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::roughcut {

struct OutlineOptions {
    std::size_t sentencesPerBullet = 8; ///< transcript sentences sent per bullet (the closest by the local index)
    std::size_t maxChars = 24'000;      ///< transcript text sent at most
    std::size_t maxBeats = 200;         ///< beats accepted from the answer
    int maxOutputTokens = 3000;
};

/// The closest single sentences of the media sources to each text, best first (the planner's retrieval index).
std::vector<std::vector<WordSpan>> closestSentences(const SpeechCorpus& corpus, std::span<const std::string> texts, std::size_t perText);

/// What is sent: the system prompt, the bullets and the numbered candidate sentences, a JSON response format.
/// `sentences` receives the spans the numbers refer to (S1 = sentences[0]).
ChatRequest outlineRequest(const Script& outline, const SpeechCorpus& corpus, const OutlineOptions& options,
                           std::vector<WordSpan>* sentences = nullptr);

struct OutlineOutcome {
    Script script;              ///< the outline as beats (bullets the model did not place stay as they were)
    std::string model;
    std::size_t bullets = 0;    ///< bullets interpreted
    std::size_t beats = 0;      ///< beats made
    std::size_t charsSent = 0;  ///< transcript characters sent
};

/// Interprets the outline. Errors: the model's (ConsentRequired, Offline, SpendCapExceeded, ...) and ProviderError for
/// an answer that is not the asked JSON.
std::expected<OutlineOutcome, AiError> interpretOutline(IChatModel& model, const Script& outline, const SpeechCorpus& corpus,
                                                        const OutlineOptions& options = {}, std::stop_token stop = {});

} // namespace wabcut::ai::roughcut
