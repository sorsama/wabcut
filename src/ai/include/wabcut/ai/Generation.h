#pragma once

// Generative media helpers (AI-10, docs/ai.md "Generative media"): what a generation request will produce and cost
// on a model, before anything is sent. Pure functions over the registry's GenerationSpec and prices; the guarded
// call itself is AiPlatform::generate() (Provider.h).

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Capability.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/SpendGuard.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

/// The length a video model will actually generate for a wanted duration: the shortest allowed duration that is at
/// least `wanted`, else the longest one. Seconds; 0 for models without documented durations.
int generatedSeconds(const GenerationSpec& spec, std::optional<Ticks> wanted);

/// The price tier of a request: the resolution (video) or size (image), plus "+audio" for video with sound when
/// the model has tiers for it. Matches PriceRule::tier.
std::string generationTier(const ModelSpec& model, const MediaGenerationRequest& request);

/// Checks a request against what the model accepts (kind, conditioning images, aspect ratio, resolution, prompt
/// length, files that exist). InvalidRequest names the first problem; Unsupported for a kind or conditioning the
/// model lacks.
std::expected<void, AiError> checkGenerationRequest(const ModelSpec& model, const MediaGenerationRequest& request);

/// 0.2 sound effects: the length a request is estimated and billed at - the one asked for (within the model's limits),
/// or the model's longest when it picks the length itself.
double soundEffectSeconds(const ModelSpec& model, const MediaGenerationRequest& request);

/// Pre-run cost: generated seconds (rounded up to an allowed duration) x the tier's per-second price, or images x
/// the per-image price, times `count`. Generation prices are fixed per output, so expected == ceiling.
CostEstimate estimateGenerationCost(const ModelSpec& model, const MediaGenerationRequest& request,
                                    std::chrono::year_month_day date);

/// The usage a successful job is billed for (what the estimate assumed), for settling the reservation.
Usage generationUsage(const ModelSpec& model, const MediaGenerationRequest& request);

/// SHA-256 of the UTF-8 prompt, 64 lowercase hex digits (provenance stores the hash, never the prompt).
std::string promptHash(std::string_view prompt);

/// What AiPlatform::generate() returns.
struct GeneratedMedia {
    std::vector<std::filesystem::path> files; ///< downloaded outputs, in output order
    GenerationTicket ticket;
    std::string provider;
    std::string model;
    Usage usage;
    MicroUsd cost = 0;
    std::string createdAt; ///< ISO-8601 UTC, when the job finished
};

/// Progress callbacks of a generation job (job thread).
struct GenerationEvents {
    std::function<void(const GenerationTicket&)> onSubmitted;
    std::function<void(const GenerationStatus&)> onStatus;
};

} // namespace wabcut::ai
