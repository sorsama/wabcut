#pragma once

// Hybrid ranking: what the frames show (visual similarity) and what is said (transcript match) in one list, by
// Reciprocal Rank Fusion. Each candidate scores weight / (k + rank) in its own list (rank 1 = best), so neither
// list's raw scale (cosine vs bm25) dominates and a stretch found by both ranks above one found by either. A
// transcript phrase that overlaps a visual hit of the same file joins it: their scores add and the hit spans both.

#include <wabcut/ai/search/SearchIndex.h>
#include <wabcut/ai/search/TextQuery.h>

#include <optional>
#include <span>
#include <vector>

namespace wabcut::ai::search {

struct FusionOptions {
    double k = 60.0; ///< the RRF constant (the value of the original paper)
    double visualWeight = 1.0;
    double transcriptWeight = 1.0;
};

struct FusedHit {
    std::uint64_t fingerprint = 0;
    TimeRange range;                  ///< source range
    Ticks thumbnail;                  ///< the matched keyframe, else the first matched word
    double score = 0.0;               ///< fused score; higher is better
    std::optional<float> similarity;  ///< visual cosine, when the frames matched
    std::optional<double> relevance;  ///< transcript relevance, when the words matched
    std::vector<std::string> words;   ///< the matched phrase
    std::vector<TimeRange> wordTimes;
    std::vector<std::size_t> matched; ///< indices into `words` of the words the query matched
    bool spaced = true;               ///< the phrase's words are written with spaces between them
};

/// `visual` and `text` best first. Result sorted by score, best first.
std::vector<FusedHit> fuse(std::span<const VisualMatch> visual, std::span<const TextMatch> text, const TextQuery& query,
                           const FusionOptions& options = {});

} // namespace wabcut::ai::search
