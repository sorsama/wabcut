#pragma once

// Splits long audio into transcription chunks along voice-activity boundaries, and stitches chunk results back
// onto one timeline.
//
// Whisper decodes 30 s windows and carries text context between them; long media therefore goes through it in
// chunks of a few minutes, cut only in silence (Silero VAD segments from whisper.cpp >= 1.9). Long silences fall
// between chunks and are never transcribed (whisper tends to hallucinate text in long silence); shorter pauses
// stay inside a chunk, where whisper handles them (it can also run its own VAD there, see TranscriptionOptions).
// Every chunk result is in centiseconds from the chunk start; stitching maps it to source time (Timestamps.h).

#include <wabcut/ai/Transcript.h>
#include <wabcut/ai/WordBuilder.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace wabcut::ai {

/// Half-open range of 16 kHz samples.
struct SampleRange {
    std::int64_t begin = 0;
    std::int64_t end = 0;
    constexpr std::int64_t length() const { return end - begin; }
    bool operator==(const SampleRange&) const = default;
};

struct ChunkOptions {
    std::int64_t maxChunkSamples = 300LL * 16000; ///< upper bound of one chunk (5 min)
    std::int64_t padSamples = 16000 / 4;          ///< kept around every speech segment (250 ms)
    std::int64_t mergeGapSamples = 2LL * 16000;   ///< speech this close counts as one stretch (2 s)
    std::int64_t maxGapSamples = 10LL * 16000;    ///< a longer silence always ends a chunk (10 s)
};

/// Chunks covering every speech segment (padded, clamped to [0, totalSamples)), in order, non-overlapping, each
/// at most maxChunkSamples long. Speech closer than mergeGapSamples forms one stretch; stretches share a chunk
/// while it stays within maxChunkSamples and no silence inside it exceeds maxGapSamples; a stretch longer than
/// maxChunkSamples is cut into equal parts. Chunks start and end on speech (plus padding), so leading, trailing
/// and long silences are skipped. `speech` may be unsorted or overlap. Without speech the result is empty.
std::vector<SampleRange> planChunks(std::span<const SampleRange> speech, std::int64_t totalSamples,
                                    const ChunkOptions& options = {});

/// Chunks without VAD: [0, total) cut into equal parts of at most maxChunkSamples.
std::vector<SampleRange> fixedChunks(std::int64_t totalSamples, const ChunkOptions& options = {});

/// VAD segments in centiseconds from the start of a block that begins at sample `blockStart` -> sample ranges.
std::vector<SampleRange> speechFromCentiseconds(std::span<const std::pair<std::int64_t, std::int64_t>> segments,
                                                std::int64_t blockStart, std::int64_t blockSamples);

/// The recogniser output of one chunk.
struct ChunkResult {
    SampleRange chunk;
    std::vector<RecognizedSegment> segments; ///< times in centiseconds from chunk.begin
};

/// Appends chunk results (in chunk order) to `out` as source-time words and segments: words built per `split`,
/// times clamped into their chunk and kept non-decreasing across chunks, empty segments dropped. `origin` is the
/// source time of PCM sample 0. Errors: InvalidArgument on a time overflow (never for real media).
std::expected<void, Error> stitchChunks(std::span<const ChunkResult> chunks, Ticks origin, WordSplit split,
                                        Transcript& out);

} // namespace wabcut::ai
