#include <wabcut/ai/VadChunker.h>

#include <wabcut/ai/Timestamps.h>

#include <algorithm>
#include <limits>

namespace wabcut::ai {

namespace {

/// Cuts `r` into the fewest equal parts of at most `max` samples.
void appendSplit(std::vector<SampleRange>& out, SampleRange r, std::int64_t max) {
    const std::int64_t parts = (r.length() + max - 1) / max;
    for (std::int64_t i = 0; i < parts; ++i) {
        const std::int64_t b = r.begin + r.length() * i / parts;
        const std::int64_t e = r.begin + r.length() * (i + 1) / parts;
        out.push_back({b, e});
    }
}

} // namespace

std::vector<SampleRange> planChunks(std::span<const SampleRange> speech, std::int64_t totalSamples,
                                    const ChunkOptions& options) {
    const std::int64_t maxChunk = std::max<std::int64_t>(options.maxChunkSamples, 1600);
    std::vector<SampleRange> regions;
    for (const SampleRange& s : speech) {
        const std::int64_t b = std::clamp(std::min(s.begin, s.end) - options.padSamples, std::int64_t{0}, totalSamples);
        const std::int64_t e = std::clamp(std::max(s.begin, s.end) + options.padSamples, std::int64_t{0}, totalSamples);
        if (e > b) {
            regions.push_back({b, e});
        }
    }
    std::ranges::sort(regions, {}, &SampleRange::begin);

    // Merge overlapping regions and regions separated by short silences.
    std::vector<SampleRange> merged;
    for (const SampleRange& r : regions) {
        if (!merged.empty() && r.begin - merged.back().end <= options.mergeGapSamples) {
            merged.back().end = std::max(merged.back().end, r.end);
        } else {
            merged.push_back(r);
        }
    }

    // Pack: a region joins the current chunk while the chunk stays within the limit; very long speech is split.
    std::vector<SampleRange> chunks;
    for (const SampleRange& r : merged) {
        if (r.length() > maxChunk) {
            appendSplit(chunks, r, maxChunk);
            continue;
        }
        if (!chunks.empty() && r.end - chunks.back().begin <= maxChunk && r.begin - chunks.back().end <= options.maxGapSamples) {
            chunks.back().end = r.end;
        } else {
            chunks.push_back(r);
        }
    }
    return chunks;
}

std::vector<SampleRange> fixedChunks(std::int64_t totalSamples, const ChunkOptions& options) {
    std::vector<SampleRange> chunks;
    if (totalSamples > 0) {
        appendSplit(chunks, {0, totalSamples}, std::max<std::int64_t>(options.maxChunkSamples, 1600));
    }
    return chunks;
}

std::vector<SampleRange> speechFromCentiseconds(std::span<const std::pair<std::int64_t, std::int64_t>> segments,
                                                std::int64_t blockStart, std::int64_t blockSamples) {
    std::vector<SampleRange> out;
    for (const auto& [t0, t1] : segments) {
        const std::int64_t b = blockStart + std::clamp(centisecondsToSamples(std::max<std::int64_t>(t0, 0)), std::int64_t{0}, blockSamples);
        const std::int64_t e = blockStart + std::clamp(centisecondsToSamples(std::max<std::int64_t>(t1, 0)), std::int64_t{0}, blockSamples);
        if (e > b) {
            out.push_back({b, e});
        }
    }
    return out;
}

std::expected<void, Error> stitchChunks(std::span<const ChunkResult> chunks, Ticks origin, WordSplit split,
                                        Transcript& out) {
    auto overflow = [] { return std::unexpected(Error{ErrorCode::InvalidArgument, "transcript time out of range"}); };
    // Continue after what `out` already holds, so stitching chunk by chunk (or segment by segment) gives the
    // same result as stitching everything at once.
    Ticks floor{std::numeric_limits<std::int64_t>::min()};
    if (!out.words.empty()) {
        floor = out.words.back().t0;
    }
    if (!out.segments.empty()) {
        floor = std::max(floor, out.segments.back().t0);
    }
    for (const ChunkResult& chunk : chunks) {
        const std::int64_t chunkCs = chunk.chunk.length() / (kTranscriptionSampleRate / 100);
        const auto chunkBegin = transcriptionSamplesToTicks(chunk.chunk.begin);
        const auto chunkEnd = transcriptionSamplesToTicks(chunk.chunk.end);
        if (!chunkBegin || !chunkEnd) {
            return overflow();
        }
        const auto lo = checkedAdd(origin, *chunkBegin);
        const auto hi = checkedAdd(origin, *chunkEnd);
        if (!lo || !hi) {
            return overflow();
        }
        auto toSource = [&](std::int64_t cs) -> std::optional<Ticks> {
            const auto t = chunkTimeToSource(origin, chunk.chunk.begin, std::clamp<std::int64_t>(cs, 0, chunkCs));
            return t ? std::optional<Ticks>(std::clamp(*t, *lo, *hi)) : std::nullopt;
        };
        for (const RecognizedSegment& recognized : chunk.segments) {
            const std::vector<BuiltWord> built = buildWords(recognized, split);
            const std::string text = segmentText(recognized);
            if (built.empty() && text.empty()) {
                continue;
            }
            TranscriptSegment segment;
            const auto s0 = toSource(recognized.t0);
            const auto s1 = toSource(recognized.t1);
            if (!s0 || !s1) {
                return overflow();
            }
            segment.t0 = std::max(*s0, floor);
            segment.t1 = std::max(*s1, segment.t0);
            segment.firstWord = out.words.size();
            segment.text = text;
            for (const BuiltWord& b : built) {
                const auto w0 = toSource(b.t0);
                const auto w1 = toSource(b.t1);
                if (!w0 || !w1) {
                    return overflow();
                }
                TranscriptWord word;
                word.text = b.text;
                word.t0 = std::max(*w0, floor);
                word.t1 = std::max(*w1, word.t0);
                word.confidence = std::clamp(b.confidence, 0.0f, 1.0f);
                floor = word.t0;
                segment.t0 = std::min(segment.t0, word.t0);
                segment.t1 = std::max(segment.t1, word.t1);
                out.words.push_back(std::move(word));
            }
            segment.wordCount = out.words.size() - segment.firstWord;
            if (!out.segments.empty()) {
                segment.t0 = std::max(segment.t0, out.segments.back().t0);
            }
            floor = std::max(floor, segment.t0);
            out.segments.push_back(std::move(segment));
        }
    }
    return {};
}

} // namespace wabcut::ai
