#pragma once

// Transcript: words with source-time anchors, segments, language and provenance (docs/ai.md, "Semantic data
// model"). Pure data; JSON in TranscriptJson.h, captions in Captions.h.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::ai {

/// The audio a transcript was made from. Times in a Transcript are SOURCE times of this stream: relative to the
/// media origin, the same time base AudioDecoder and VideoDecoder use, so they survive any edit of the clips
/// that use the media (the timeline maps them through each clip's TimeMap).
struct TranscriptSource {
    std::uint64_t fingerprint = 0; ///< media::sourceFingerprint of the file; 0 = unknown
    int streamIndex = -1;          ///< container stream index of the audio stream
    std::string path;              ///< last known UTF-8 path (informational; identity is the fingerprint)

    bool operator==(const TranscriptSource&) const = default;
};

/// How a transcript was produced, so it stays reproducible after models change (docs/ai.md, principle 6).
struct Provenance {
    std::string engine;        ///< e.g. "whisper.cpp"
    std::string engineVersion; ///< e.g. "1.9.5"
    std::string modelId;       ///< manifest id, e.g. "large-v3-turbo-q5_0"
    std::string modelVersion;  ///< content identity of the weights (SHA-256 from the model manifest)
    std::map<std::string, std::string> params; ///< every setting that affects the result (language, beam, ...)
    std::string createdAt;     ///< ISO-8601 UTC, e.g. "2026-10-11T08:30:00Z"

    bool operator==(const Provenance&) const = default;
};

struct TranscriptWord {
    std::string text;              ///< UTF-8, no surrounding whitespace; punctuation stays on its word
    Ticks t0{};                    ///< start, source time
    Ticks t1{};                    ///< end, source time (t1 >= t0)
    float confidence = 0.0f;       ///< 0..1 (mean token probability)
    std::optional<int> speakerId;  ///< diarization label, when known
    bool edited = false;           ///< the user changed this word's text or timing
    bool approved = false;         ///< the user reviewed and accepted this word

    bool operator==(const TranscriptWord&) const = default;
};

/// A recognised phrase: a run of consecutive words.
struct TranscriptSegment {
    Ticks t0{};
    Ticks t1{};
    std::size_t firstWord = 0;     ///< index into Transcript::words
    std::size_t wordCount = 0;
    std::string text;              ///< the segment as recognised (UTF-8)
    std::optional<int> speakerId;

    bool operator==(const TranscriptSegment&) const = default;
};

struct Transcript {
    /// JSON schema version written by toJson (TranscriptJson.h); readers accept this version only.
    static constexpr int kSchemaVersion = 1;

    std::string language;          ///< ISO 639-1 code as detected or requested ("en", "th", ...)
    TranscriptSource source;
    Provenance provenance;
    std::vector<TranscriptWord> words;       ///< in time order
    std::vector<TranscriptSegment> segments; ///< in time order, covering consecutive word ranges

    /// Structural checks: t0 <= t1 everywhere, words and segments in time order, segment word ranges inside
    /// `words`, contiguous and non-overlapping, and every word's text valid UTF-8. InvalidArgument otherwise.
    std::expected<void, Error> validate() const;

    bool operator==(const Transcript&) const = default;
};

} // namespace wabcut::ai
