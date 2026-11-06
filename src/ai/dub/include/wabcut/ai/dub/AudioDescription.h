#pragma once

// Audio description (0.7, card 175; docs/ai.md "Audio description"): for blind and low-vision viewers a second, spoken
// layer of what is on screen, said in the gaps between the dialogue.
//
//   1. findGaps     the gaps in the speech of a sequence (its transcript, in timeline order) long enough to say something
//                   in: the usable stretch, how many words fit at a speaking pace, and the dialogue just before and after;
//   2. describeGaps a vision model (a frame or two of each gap, the dialogue around it) writes one short line per gap that
//                   fits it; "NONE" from the model leaves a gap silent. Lines are text only: nothing is voiced here;
//   3. the lines become a caption track "Audio Description" (command audioDescription.createTrack) the person reads and
//      edits in the Captions tab like any other, exports as a script for a narrator (captions.export), or voices with
//      dub.create (the Dub dialog: the voice, cost and consent; the dialogue and music are ducked under each line).
//
// The model sees pictures of the sequence only through the ordinary guarded chat path (consent, spend cap, offline mode and
// the project's no-cloud lock apply; a model on this computer works too). Nothing in this file touches the network.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/Chat.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <expected>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::dub {

struct AdOptions {
    /// The shortest stretch of silence worth a line, after the margins are taken off.
    Ticks minGap{2 * kTicksPerSecond};
    /// Words closer together than this are one stretch of speech (a breath is not a gap).
    Ticks joinGap{kTicksPerSecond / 2};
    /// Kept clear of the speech on each side of a line.
    Ticks margin{kTicksPerSecond * 3 / 10};
    /// A silence longer than this gets several lines.
    Ticks maxGap{15 * kTicksPerSecond};
    /// How fast a describer reads: words per second (about 150 words a minute).
    double wordsPerSecond = 2.5;
    /// Most words in a line, whatever the gap.
    int maxWords = 40;
    /// Words of dialogue shown around a gap.
    std::size_t contextWords = 12;
};

/// One gap in the speech where a description fits.
struct AdGap {
    TimeRange range;      ///< the usable stretch on the timeline
    int maxWords = 0;     ///< what fits in it at the speaking pace
    std::string before;   ///< the last words said before it ("" at the start)
    std::string after;    ///< the first words said after it ("" at the end)
};

/// A line of description in its gap.
struct AdLine {
    Ticks start{};
    Ticks duration{};
    std::string text;
    int maxWords = 0;
    bool operator==(const AdLine&) const = default;
};

/// The speech of a sequence as ranges on the timeline: words closer than options.joinGap are joined. Sorted.
std::vector<TimeRange> speechRanges(const ai::edit::SequenceTranscript& transcript, const AdOptions& options = {});

/// The gaps between `speech` (sorted ranges) in [0, duration): the silence before the first range and after the last
/// count too. `words`, when given, supplies the dialogue around each gap.
std::vector<AdGap> findGaps(std::span<const TimeRange> speech, Ticks duration, const AdOptions& options = {},
                            const ai::edit::SequenceTranscript* words = nullptr);
/// findGaps over a sequence's transcript.
std::vector<AdGap> findGaps(const ai::edit::SequenceTranscript& transcript, Ticks duration, const AdOptions& options = {});

/// How many words fit in `length` at the speaking pace (at least 1, at most options.maxWords).
int wordBudget(Ticks length, const AdOptions& options = {});

/// `text` as one line of at most `maxWords` words: blanks and line breaks folded, quotes and bullets dropped, cut at a
/// sentence end when one fits (else at a word) and ended with a full stop. "NONE" (any case) and an empty text give "".
std::string fitDescription(std::string_view text, int maxWords);

/// The words of `text`.
int wordCount(std::string_view text);

/// What the model is told, and the picture(s) and dialogue of one gap.
struct DescribeOptions {
    std::string language = "en"; ///< ISO 639-1: the language the lines are written in
    std::string style;           ///< extra guidance from the person ("British spelling", "name the characters Ana and Ben")
    AdOptions timing;
    int maxOutputTokens = 200;
};

/// The system prompt (the rules of audio description) for `options`.
std::string describeSystemPrompt(const DescribeOptions& options);

/// The request for one gap: the pictures (`frames`, inline images) and the facts about the gap, `previous` the lines
/// already written (so the descriptions do not repeat each other).
ai::ChatRequest describeRequest(const AdGap& gap, std::vector<ai::ImagePart> frames, const DescribeOptions& options,
                                std::span<const std::string> previous);

/// Frames of a gap, in time order (one at the middle, or a few across it); an empty answer skips the gap.
using FrameSource = std::function<std::vector<ai::ImagePart>(const AdGap& gap)>;

struct DescribeResult {
    std::vector<AdLine> lines;          ///< in time order, one per described gap
    std::vector<std::string> warnings;  ///< gaps left silent and why ("00:12 no picture", "model said nothing to describe")
    ai::Usage usage;
    std::size_t gaps = 0;               ///< how many gaps were offered
};

/// Describes `gaps` with `model` (a chat model that takes images), one request each. `progress(done / total)` returns
/// false to stop (the result so far is returned). The first model error ends the run with that error (a consent or a
/// spend cap stop is not retried gap after gap).
std::expected<DescribeResult, ai::AiError> describeGaps(std::span<const AdGap> gaps, ai::IChatModel& model,
                                                        const FrameSource& frames, const DescribeOptions& options,
                                                        const std::function<bool(double)>& progress, std::stop_token stop);

/// Adds audioDescription.findGaps and audioDescription.createTrack (registered with the dub commands).
void registerAudioDescriptionCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::dub
