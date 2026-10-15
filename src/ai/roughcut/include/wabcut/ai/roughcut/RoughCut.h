#pragma once

// Rough cut from a script (AI-11, docs/ai.md "Creator superpowers"): paste a script, an outline or bullet points and get
// a first cut in script order, made of the best take of every beat, with the alternates kept.
//
//   planRoughCut()      every beat of the script is looked for across the speech of the media in scope:
//                         1. retrieval: runs of 1..4 consecutive sentences of every source are scored against the beat
//                            by a weighted bag of words / word pairs (cluster bigrams and trigrams for Thai), with IDF
//                            weights from the whole corpus, so a 60-minute transcript is searched in milliseconds;
//                         2. alignment: the best windows are aligned word by word (TextMatch.h, fuzzy and Thai-aware),
//                            which finds the exact words that say the beat inside them; outline beats (bullets) and
//                            weak matches also count the text embedder's similarity (AI-8's encoder) when the host has
//                            one;
//                         3. takes: every distinct match good enough is a take of the beat; each is judged
//                            (Speech.h: fillers, false starts, hesitations, complete sentence, level / clipping / noise,
//                            pace) and the best is chosen ("best", or "cleanest" / "first" / "last" by preference).
//                       Deterministic: equal inputs give equal plans (ties break on source, then time).
//   assembleRoughCut()  a new sequence in script order: each chosen take as linked V/A clips trimmed to its words with
//                       small handles (never into a neighbouring word), frame-aligned, a marker per script section, and,
//                       when given, B-roll suggestions on their own track ("B-roll (suggested)") over the beats they
//                       illustrate. The sequence records the plan (Sequence::derivedFrom, generator "roughcut/1"): every
//                       beat's chosen take and alternates as media ranges, so a take can be swapped later
//                       (roughcut.swapTake) even after the project was saved and reopened.

#include <wabcut/ai/roughcut/Script.h>
#include <wabcut/ai/roughcut/Speech.h>

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::roughcut {

inline constexpr std::string_view kRoughCutGenerator = "roughcut/1";

enum class TakePreference {
    Best,     ///< the highest combined score
    Cleanest, ///< fewest fillers, false starts and hesitations, then the score
    First,    ///< the earliest good take (source order, then time)
    Last,     ///< the latest good take (often the keeper)
};
std::string_view toString(TakePreference preference);
std::optional<TakePreference> takePreferenceFromString(std::string_view text);

struct RoughCutOptions {
    double minMatch = 0.45;              ///< alignment quality below which a match is not a take of the beat
    double alternateMargin = 0.3;        ///< alternates within this much match quality of the best match are kept
    std::size_t maxAlternates = 4;       ///< takes kept per beat besides the chosen one
    std::size_t candidatesPerBeat = 12;  ///< retrieved windows aligned per beat
    std::size_t maxSentencesPerWindow = 4;
    double embedWeight = 0.35;           ///< of the embedding similarity in an outline beat's match, when available
    TakePreference preference = TakePreference::Best;
    Ticks handle = Ticks{kTicksPerSecond * 15 / 100}; ///< media kept around a take's words (each side, at most half the pause)
    CorpusOptions corpus;
};

/// One take of a beat.
struct BeatTake {
    WordSpan span;              ///< the words that say the beat
    MediaRange media;           ///< what the clip plays: the words plus handles
    double match = 0.0;         ///< 0..1 alignment quality (with the embedding share for outline beats)
    TakeQuality quality;
    std::string text;           ///< as said
    bool byMeaning = false;     ///< the embedding similarity carried the match (outline beats)
};

struct BeatPlan {
    ScriptBeat beat;
    std::vector<BeatTake> takes; ///< the chosen take first, then the alternates (best first)
    double confidence = 0.0;     ///< 0..1: how sure the choice is (match quality, and the margin over the next take)
    std::string note;            ///< "not found in the media", "partial match", ...
    bool found() const { return !takes.empty(); }
};

struct RoughCutPlan {
    Script script;
    std::vector<BeatPlan> beats;
    std::size_t sources = 0;      ///< media searched
    std::size_t words = 0;        ///< words searched
    double milliseconds = 0.0;    ///< planning time
    std::vector<std::string> warnings;
    std::size_t found() const;
};

/// What else the planner may use (all optional).
struct RoughCutSignals {
    /// A text embedder (AI-8's text encoder): an L2-normalised vector of a text, or nullopt.
    std::function<std::optional<std::vector<float>>(const std::string& text)> embed;
    /// Take audio (level, clipping, noise floor); null = not measured.
    const TakeAudioProbe* audio = nullptr;
};

/// Plans the rough cut of `script` over the media sources of `corpus` (Sequence sources are ignored).
RoughCutPlan planRoughCut(const SpeechCorpus& corpus, const Script& script, const RoughCutSignals& inputs = {},
                          const RoughCutOptions& options = {});

/// The take of a beat a preference picks among takes already ranked by score (index into `takes`).
std::size_t preferredTake(const std::vector<BeatTake>& takes, TakePreference preference);

/// A B-roll suggestion for a beat.
struct BrollSuggestion {
    std::size_t beat = 0;
    MediaId media;
    TimeRange source;           ///< what to show (source time)
    std::string query;          ///< what was searched
    double score = 0.0;         ///< the search's similarity
};

struct AssemblyOptions {
    std::string name;                 ///< the new sequence's name ("" = "Rough cut: <title>")
    std::optional<SequenceId> settingsFrom; ///< copy frame size and rate from this sequence; else the first take's video
    bool sectionMarkers = true;       ///< a marker at each script section
    std::vector<BrollSuggestion> broll; ///< placed on a "B-roll (suggested)" track (video only)
};

struct AssembledCut {
    ProjectState state;
    SequenceId sequence;
    std::vector<std::optional<TimeRange>> beatRanges; ///< per beat: where its take landed on the timeline
    std::size_t clips = 0;           ///< clips placed (V and A count separately)
    std::size_t brollPlaced = 0;
    std::vector<std::string> warnings;
};

/// Builds the rough-cut sequence (appended to `state`). Errors: InvalidArgument when no beat has a take.
std::expected<AssembledCut, Error> assembleRoughCut(const ProjectState& state, const RoughCutPlan& plan, const AssemblyOptions& options = {});

// --- The plan record (Sequence::derivedFrom) ----------------------------------------------------------------------

/// A beat of the record: its text and its takes as media ranges (the chosen one first).
struct RecordedTake {
    MediaRange media;
    double score = 0.0;
    bool operator==(const RecordedTake&) const = default;
};
struct RecordedBeat {
    std::string text;
    std::vector<RecordedTake> takes; ///< takes[chosen] is on the timeline
    std::size_t chosen = 0;
    bool operator==(const RecordedBeat&) const = default;
};
struct RoughCutRecord {
    std::string title;
    std::vector<RecordedBeat> beats;
    bool operator==(const RoughCutRecord&) const = default;
};

/// The record of a plan (text cut and alternates dropped as needed to stay within SequenceDerivation::kMaxParamsBytes).
RoughCutRecord recordOf(const RoughCutPlan& plan);
std::string recordToJson(const RoughCutRecord& record);
std::expected<RoughCutRecord, Error> recordFromJson(std::string_view json);
/// The record of a rough-cut sequence, or an error saying why there is none.
std::expected<RoughCutRecord, Error> recordOfSequence(const Sequence& sequence);

} // namespace wabcut::ai::roughcut
