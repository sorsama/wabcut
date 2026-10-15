#pragma once

// Retake comparison (AI-11, docs/ai.md "Creator superpowers"): repeated attempts of the same sentence, within one
// recording or across several, grouped so the best one can be kept.
//
//   findTakes()   every sentence of the scope (a sequence's words as heard, and/or media transcripts) of at least
//                 `minWords` words is a possible attempt. Candidates come from an IDF-weighted feature index; each pair
//                 is verified by aligning the two sentences both ways (TextMatch.h, fuzzy and Thai-aware). Two
//                 sentences are attempts of the same line when their mutual alignment quality reaches
//                 `minSimilarity`, or when one is a cut-off start of the other (a false start: "So today we-" before
//                 "So today we're looking at pricing"). Within one recording, attempts further apart than `maxGap`
//                 are not retakes (a catch-phrase said again later); across recordings any distance counts. Groups
//                 are formed by merging the most similar pairs first, and a sentence joins a group only when it is
//                 similar to every attempt already in it (no chaining of loosely related lines).
//                 Each attempt is judged like a rough-cut take (Speech.h: completeness against the group's fullest
//                 attempt, fillers, false starts, hesitations, level / clipping / noise floor, pace) and the group
//                 ranks them best first, with each attempt's word-level difference from the best one.
//                 A media attempt that is the same words a sequence attempt plays is that attempt (it is on the
//                 timeline); attempts only in the media are alternates the timeline could use.
//   planUseTake() what "Use this take" does to the sequence: the first attempt of the group on the timeline is the
//                 slot. If the chosen attempt is on the timeline, every other attempt of the group on the timeline is
//                 removed (ripple, with its pause); otherwise the slot is replaced by the chosen attempt's media
//                 (its words plus handles, linked video and audio like the clip it replaces) and the other attempts on
//                 the timeline are removed. "Keep best takes" is that for every group, as one edit.
//   applyTakeEdits() runs the planned removals and replacements on a state, last first, so earlier times stay valid.

#include <wabcut/ai/edit/TextEdits.h>
#include <wabcut/ai/roughcut/Speech.h>

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::command {
class ExecutionControl;
class CommandServices;
class CommandRegistry;
} // namespace wabcut::command

namespace wabcut::ai::roughcut {

struct TakesOptions {
    double minSimilarity = 0.7;          ///< mutual alignment quality for two sentences to be attempts of one line
    Ticks maxGap = Ticks::fromSeconds(180); ///< within one recording: attempts further apart are not retakes
    bool acrossSources = true;           ///< attempts in different recordings count
    std::size_t minWords = 3;            ///< shorter sentences ("okay", "yeah, so") are never attempts
    std::size_t maxTakesPerGroup = 12;
    std::size_t maxGroups = 500;
    CorpusOptions corpus;
};

struct TakesSignals {
    const TakeAudioProbe* audio = nullptr;
    /// A text embedder (optional): paraphrased attempts ("we doubled the price" / "the price went up twice") group
    /// when their words half-match and their meaning is close.
    std::function<std::optional<std::vector<float>>(const std::string& text)> embed;
};

/// One word of a diff, for display.
struct DiffWord {
    enum class Kind { Equal, Inserted, Deleted };
    Kind kind = Kind::Equal;
    std::string text;
    bool operator==(const DiffWord&) const = default;
};

struct Take {
    WordSpan span;                     ///< its words in its source
    TimeRange range;                   ///< on its source's axis (timeline for sequence takes, media time otherwise)
    std::optional<MediaRange> media;   ///< the media it plays, when it is one stretch of one item
    bool onTimeline = false;           ///< a sequence take (heard on the sequence)
    std::string sourceName;            ///< the media or sequence name
    std::string text;
    double completeness = 0.0;         ///< 0..1: how much of the group's fullest attempt it says
    TakeQuality quality;
    std::vector<DiffWord> diff;        ///< from the best take of the group to this one (empty for the best)
};

struct TakeGroup {
    std::size_t id = 0;                ///< its index in the report
    std::string text;                  ///< what the line says (the best take's words)
    std::vector<Take> takes;           ///< best first
    std::size_t onTimeline = 0;        ///< takes heard on the sequence
    double confidence = 0.0;           ///< 0..1: how alike the attempts are (the weakest link of the group)
    /// The best take is not the one the sequence plays first (Keep best takes would change something).
    bool improvable() const;
};

struct TakesReport {
    std::vector<TakeGroup> groups;     ///< in order of their first appearance (timeline first, then media)
    std::size_t sentences = 0;         ///< attempts considered
    std::size_t sources = 0;
    double milliseconds = 0.0;
    std::vector<std::string> warnings;
};

/// Finds the groups of repeated attempts in `corpus`.
TakesReport findTakes(const SpeechCorpus& corpus, const TakesSignals& inputs = {}, const TakesOptions& options = {});

// --- Editing with takes ---------------------------------------------------------------------------------------------

struct TakeEditOptions {
    /// Around a replaced take: how much of the pause on each side is cut with it (never into a neighbouring word).
    Ticks replacePad = Ticks{kTicksPerSecond * 3 / 10};
    /// Around a removed take: the usual word padding, plus the pause after it (room tone kept).
    edit::CutOptions removeCut;
    /// Media kept around an inserted take's words (each side, at most half the pause to a neighbouring word).
    Ticks handle = Ticks{kTicksPerSecond * 15 / 100};
};

/// One change to a sequence: remove a stretch of its words, or replace it with a media range.
struct TakeEdit {
    std::size_t firstWord = 0;          ///< words of the sequence's SequenceTranscript
    std::size_t lastWord = 0;
    std::optional<MediaRange> with;     ///< replace with this (words + handles); nullopt = remove
    std::string label;                  ///< for the review list ("Use take 2 of 'So today we...'")
};

/// What "Use this take" changes for `group` and the chosen take index. Errors: InvalidArgument when no take of the
/// group is on the timeline, or the chosen take has no single media stretch to insert.
std::expected<std::vector<TakeEdit>, Error> planUseTake(const SpeechCorpus& corpus, const TakeGroup& group, std::size_t take,
                                                        const TakeEditOptions& options = {});

/// The media stretch a take would be inserted as: its words plus handles (TakeEditOptions::handle).
std::optional<MediaRange> insertRangeOf(const SpeechCorpus& corpus, const Take& take, Ticks handle);

/// Applies `edits` to `sequence` of `state` (the transcript `words` must be that sequence's in `state`), last first, as
/// catalog edits run inline (so locked tracks, links and sync locks behave as for any extract and insert). Sequence In/Out
/// marks are kept. Throws command::CommandFailure on the first edit that cannot be made.
ProjectState applyTakeEdits(const ProjectState& state, SequenceId sequence, const edit::SequenceTranscript& words,
                            std::vector<TakeEdit> edits, const TakeEditOptions& options, const command::CommandRegistry& registry,
                            command::ExecutionControl* control = nullptr, command::CommandServices* services = nullptr);

} // namespace wabcut::ai::roughcut
