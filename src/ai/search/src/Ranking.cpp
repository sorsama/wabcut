#include <wabcut/ai/search/Ranking.h>

#include <algorithm>

namespace wabcut::ai::search {

std::vector<FusedHit> fuse(std::span<const VisualMatch> visual, std::span<const TextMatch> text, const TextQuery& query,
                           const FusionOptions& options) {
    std::vector<FusedHit> hits;
    hits.reserve(visual.size() + text.size());
    for (std::size_t i = 0; i < visual.size(); ++i) {
        const VisualMatch& v = visual[i];
        FusedHit hit;
        hit.fingerprint = v.fingerprint;
        hit.range = v.range;
        hit.thumbnail = v.time;
        hit.similarity = v.similarity;
        hit.score = options.visualWeight / (options.k + static_cast<double>(i + 1));
        hits.push_back(std::move(hit));
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const TextMatch& t = text[i];
        const double contribution = options.transcriptWeight / (options.k + static_cast<double>(i + 1));
        const auto matched = matchedWords(t.words, query);
        // The best visual hit of the same file that this phrase overlaps and that has no phrase yet.
        FusedHit* join = nullptr;
        for (FusedHit& hit : hits) {
            if (hit.fingerprint == t.fingerprint && hit.similarity && !hit.relevance && hit.range.overlaps(t.range) &&
                (join == nullptr || hit.score > join->score)) {
                join = &hit;
            }
        }
        if (join != nullptr) {
            join->score += contribution;
            join->relevance = t.relevance;
            join->range = TimeRange::fromStartEnd(std::min(join->range.start, t.range.start), std::max(join->range.end(), t.range.end()));
            join->words = t.words;
            join->wordTimes = t.wordTimes;
            join->matched = matched;
            join->spaced = t.spaced;
            continue;
        }
        FusedHit hit;
        hit.fingerprint = t.fingerprint;
        hit.range = t.range;
        hit.score = contribution;
        hit.relevance = t.relevance;
        hit.words = t.words;
        hit.wordTimes = t.wordTimes;
        hit.matched = matched;
        hit.spaced = t.spaced;
        hit.thumbnail = !matched.empty() && matched.front() < t.wordTimes.size() ? t.wordTimes[matched.front()].start : t.range.start;
        hits.push_back(std::move(hit));
    }
    std::ranges::stable_sort(hits, [](const FusedHit& a, const FusedHit& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.fingerprint != b.fingerprint) {
            return a.fingerprint < b.fingerprint;
        }
        return a.range.start < b.range.start;
    });
    return hits;
}

} // namespace wabcut::ai::search
