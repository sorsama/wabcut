#pragma once

// Automatic multicam switching (AI-13, docs/ai.md "Podcast and interview editing"): from a speaker timeline
// (Speakers.h) to a list of angle cuts, each with the rule that made it and a sentence a person can check ("Cut to
// Cam B: Speaker 2 starts talking"). The rules, every one tunable and grouped in presets (Podcast calm, Interview,
// Energetic):
//   - the active speaker's close-up, cut `lead` before they start (a J-cut feel) and held at least `minHold`;
//   - the wide shot for crosstalk (two or more at once for `overlapMin`), for laughter, and after `silenceWide` of
//     silence; optionally an opening wide shot;
//   - variety: a close-up held `varietyAfter` cuts to the wide shot for `varietyLength`, then back;
//   - reaction shots (optional): a listener's strong reaction (laughter, a quick "wow") cuts to their close-up;
//   - never mid-word: a cut moves to the nearest gap between words within `maxSnap` (the transcript's words).
// Times are the multicam source's; the command maps them onto each multicam clip.

#include <wabcut/ai/multicam/Speakers.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::multicam {

struct SwitchOptions {
    std::string preset = "interview";
    Ticks minHold{kTicksPerSecond * 2};
    Ticks lead{kTicksPerSecond / 4};
    bool wideOnOverlap = true;
    Ticks overlapMin{kTicksPerSecond * 6 / 10};
    bool wideOnLaughter = true;
    Ticks silenceWide{kTicksPerSecond * 3};
    Ticks varietyAfter{kTicksPerSecond * 15}; ///< 0 = no variety cuts
    Ticks varietyLength{kTicksPerSecond * 3};
    bool reactions = false;
    double reactionStrength = 0.6;
    Ticks reactionHold{kTicksPerSecond * 12 / 10};
    bool openWide = true;
    Ticks openLength{kTicksPerSecond * 3};
    bool avoidMidWord = true;
    Ticks maxSnap{kTicksPerSecond * 4 / 10};
};

/// "calm" (Podcast calm), "interview", "energetic"; nullopt for an unknown name.
std::optional<SwitchOptions> presetOptions(std::string_view name);
std::vector<std::string> presetNames();

enum class CutReason { Opening, Speaker, Overlap, Laughter, Silence, Variety, Reaction, Back };
std::string_view toString(CutReason reason);

struct PlannedCut {
    Ticks at{};                         ///< source time: the new angle starts here
    std::size_t angle = 0;              ///< 0-based
    CutReason reason = CutReason::Speaker;
    std::optional<std::size_t> speaker; ///< whom the cut is about
    std::string explanation;
    double confidence = 0.0;
    Ticks moved{};                      ///< how far it moved to a word gap (signed)
};

struct SwitchStats {
    std::size_t cuts = 0;
    std::size_t speakerCuts = 0;
    std::size_t wideCuts = 0;
    std::size_t varietyCuts = 0;
    std::size_t reactionCuts = 0;
    std::size_t snapped = 0;
    std::size_t midWord = 0; ///< cuts that could not avoid a word (none within maxSnap)
    double averageShot = 0.0;
    double shortestShot = 0.0;
};

struct SwitchPlan {
    std::vector<PlannedCut> cuts; ///< the first is at span.start: the opening angle
    SwitchStats stats;
    std::vector<std::string> warnings;
};

/// Plans the cuts over `span` (source time). `words`: the transcript's word ranges (source time, any order), for
/// the gaps between words. `angleNames[i]`: angle i's name for the explanations.
SwitchPlan planSwitches(const SpeakerTimeline& timeline, std::span<const TimeRange> words, std::span<const std::string> angleNames,
                        TimeRange span, const SwitchOptions& options);

} // namespace wabcut::ai::multicam
