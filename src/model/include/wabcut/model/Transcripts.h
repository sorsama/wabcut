#pragma once

// The project's side of transcripts (C.7): speakers with names and colours, and per media audio stream the edits
// applied on top of whatever transcript the analysis cache holds. The cached transcript stays raw, so transcribing
// again never loses them:
//   * labels: which project speaker each diarization label of that transcript is;
//   * speakerSpans: speakers assigned by hand over source-time ranges (they win over the labels);
//   * corrections: word fixes anchored by source time and the text they replaced, so a correction still finds its
//     words after re-transcription moved them a little (and says when the words under it changed: "conflict").
// Saved with the project and edited like everything else (one undo step per command). ai::edit applies them when it
// maps transcripts onto a sequence (TranscriptEditing.h there).

#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wabcut {

struct Speaker {
    SpeakerId id;
    std::string name;            ///< UTF-8, 1..kMaxSpeakerNameBytes bytes
    std::uint32_t color = 0;     ///< 0xRRGGBB

    bool operator==(const Speaker&) const = default;
};

/// A diarization label (ai::TranscriptWord::speakerId) of one transcript and the project speaker it is.
struct SpeakerLabel {
    std::int32_t label = 0;
    SpeakerId speaker;

    bool operator==(const SpeakerLabel&) const = default;
};

/// A speaker assigned to the words whose middle lies in `source` (source time of the media).
struct SpeakerSpan {
    TimeRange source;
    SpeakerId speaker;

    bool operator==(const SpeakerSpan&) const = default;
};

/// A fix to the words whose middle lies in `source`: they read `text` instead. `original` is what they read when the
/// correction was made. Empty `text` removes the words.
struct WordCorrection {
    TimeRange source;
    std::string original;
    std::string text;

    bool operator==(const WordCorrection&) const = default;
};

/// The edits of one media item's audio stream transcript.
struct TranscriptEdits {
    MediaId media;
    std::int32_t audioStream = 0;            ///< index into MediaInfo::audio, like StreamRef::index
    std::vector<SpeakerLabel> labels;        ///< sorted by label, unique
    std::vector<SpeakerSpan> speakerSpans;   ///< sorted by start, non-overlapping, non-empty
    std::vector<WordCorrection> corrections; ///< sorted by start, non-overlapping, non-empty

    bool empty() const { return labels.empty() && speakerSpans.empty() && corrections.empty(); }
    /// The project speaker of a diarization label, or an invalid id.
    SpeakerId speakerOfLabel(std::int32_t label) const;
    /// The speaker assigned at source time `t` (by a span), or an invalid id.
    SpeakerId speakerAt(Ticks t) const;

    bool operator==(const TranscriptEdits&) const = default;
};

/// Default colour of the n-th speaker (a palette that cycles).
std::uint32_t defaultSpeakerColor(std::size_t n);

/// Why `edits` is malformed (order, overlaps, sizes, text that is not valid UTF-8), or nullopt.
std::optional<std::string> transcriptEditsProblem(const TranscriptEdits& edits);

// Bounds for untrusted project files and requests.
inline constexpr std::size_t kMaxSpeakers = 1024;              ///< per project
inline constexpr std::size_t kMaxSpeakerNameBytes = 128;
inline constexpr std::size_t kMaxTranscriptEdits = 4096;       ///< media streams with edits, per project
inline constexpr std::size_t kMaxTranscriptEditEntries = 100'000; ///< labels, spans or corrections per stream
inline constexpr std::size_t kMaxCorrectionTextBytes = 4096;

} // namespace wabcut
