#pragma once

// The optional cloud re-rank of the shorts finder (AI-9): the user's chat model (bring your own key) reads the best
// local candidates as transcript text only (no file names, no media, no pictures) and scores and titles them as a
// shorts editor would. The model comes from AiPlatform::chat, whose every call enforces the no-cloud locks (offline,
// the project's), consent and the monthly spend cap, and is recorded in the request log; callers also skip it outright
// for a no-cloud project. The local score stays the fallback for any failure.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/shorts/Shorts.h>

#include <expected>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::shorts {

struct RerankOptions {
    std::size_t maxCandidates = 10;   ///< the best local ones sent
    std::size_t maxCharsPerShort = 1500;
    std::string query;                ///< what the user asked the shorts to be about
    std::string language;             ///< the transcript's (titles are written in it)
    double weight = 0.5;              ///< of the cloud score in the final one
    int maxOutputTokens = 1500;
};

/// What is sent: the system prompt, the candidates' transcript text, a JSON response format.
ChatRequest rerankRequest(std::span<const ShortCandidate> shorts, const RerankOptions& options);

struct RerankOutcome {
    std::string model;
    std::size_t scored = 0;
};

/// Re-ranks `shorts` in place (cloudScore, cloudReason, title; sorted by the blended score). Errors: the model's
/// (ConsentRequired, Offline, SpendCapExceeded, ...) and ProviderError for an answer that is not the asked JSON; the
/// shorts are unchanged then.
std::expected<RerankOutcome, AiError> rerankShorts(IChatModel& model, std::vector<ShortCandidate>& shorts, const RerankOptions& options,
                                                   std::stop_token stop = {});

} // namespace wabcut::ai::shorts
