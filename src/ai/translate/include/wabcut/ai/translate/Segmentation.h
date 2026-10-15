#pragma once

// Re-segmenting translated captions (AI-12): each source cue's translation keeps the cue's timing when it reads at the
// target language's speed and fits its lines; otherwise, in order:
//   1. extend into the neighbouring gaps (at most 0.5 s past the end and 0.25 s before the start, keeping two frames
//      between cues, never longer than 7 s);
//   2. merge with a neighbour of the same speaker less than a second away when the two read in time together and fit
//      the lines, or move words across the boundary so both read in time ("rebalance");
//   3. break the text into lines (LineBreaker: Thai between words), and split what does not fit into its lines into
//      several cues, each timed by its share of the text;
//   4. extend cues shorter than 20 frames into the following gap.
// What still reads too fast is kept and flagged; nothing is dropped.

#include <wabcut/ai/translate/Languages.h>
#include <wabcut/model/CaptionText.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::translate {

struct TimingRules {
    Ticks minDuration{kTicksPerSecond * 5 / 6}; ///< 20 frames at 24 fps
    Ticks maxDuration{kTicksPerSecond * 7};
    Ticks minGap{kTicksPerSecond / 12};         ///< two frames at 24 fps (set from the sequence's rate)
    Ticks maxExtend{kTicksPerSecond / 2};       ///< past the end of the source cue
    Ticks maxLead{kTicksPerSecond / 4};         ///< before the start of the source cue
    Ticks mergeGap{kTicksPerSecond};            ///< neighbours closer than this may be merged or rebalanced
};

/// One translated cue in, in time order.
struct TranslatedCue {
    Ticks start{};
    Ticks duration{};
    std::string text;            ///< the translation (newlines and runs of spaces are flattened)
    SpeakerId speaker;
    std::size_t source = 0;      ///< the source cue's index
};

struct TimedCue {
    Ticks start{};
    Ticks duration{};
    std::string text;            ///< lines separated by '\n'
    SpeakerId speaker;
    std::vector<std::size_t> sources;
    double charsPerSecond = 0.0;
    bool tooFast = false;        ///< still reads faster than the language allows
    bool overflow = false;       ///< a piece longer than a line that could not be broken

    Ticks end() const { return start + duration; }
};

struct SegmentationReport {
    std::size_t kept = 0;        ///< cues with the source timing and text unchanged in shape
    std::size_t extended = 0;
    std::size_t merged = 0;
    std::size_t rebalanced = 0;
    std::size_t split = 0;       ///< extra cues made by splitting
    std::size_t tooFast = 0;
    double maxCharsPerSecond = 0.0;
};

/// The display length the reading speed counts (captionDisplayLength without spaces at line breaks).
std::size_t readingLength(std::string_view text);

std::vector<TimedCue> resegment(std::span<const TranslatedCue> cues, std::string_view language, const LanguageRules& rules,
                                const TimingRules& timing, const LineBreaker& breaker, SegmentationReport* report = nullptr);

} // namespace wabcut::ai::translate
