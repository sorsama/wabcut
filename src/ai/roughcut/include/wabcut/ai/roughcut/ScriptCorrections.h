#pragma once

// Correct the transcript from the script (0.6, P1 round #37; docs/captions.md "Correct from your script"): an essayist
// has the exact words, the speech recogniser misspells names and jargon. Every line of the script is found in the
// sequence's transcript (TextMatch.h alignment, Thai-aware), and where a stretch of what was said aligns with the line,
// the words the recogniser got wrong are replaced with the script's words:
//
//   * only substitutions: a run of transcript words that differs from a run of script words between two matching
//     stretches. A word the speaker skipped is never inserted, a word they added (a filler, an aside) never deleted;
//   * only where the transcript could be a mishearing of the script: the same number or abbreviation written another
//     way ("25" / "twenty-five", "Dr." / "Doctor"), or close in letters or in sound ("cooper nettie's" / "Kubernetes",
//     "Somchay" / "Somchai"). A different word ("great" where the script says "good") is what the speaker said: it is
//     left as said and listed as kept;
//   * casing and punctuation come from the script ("open ai" -> "OpenAI,"); a name or term the transcript wrote with
//     other capitals is given the script's ("kubernetes" -> "Kubernetes") when the script's capitals mean something
//     (inner capitals, or a capital that does not start a sentence); Thai cluster runs grow to whole transcript words
//     and the script text that covers them;
//   * a line that does not align well enough (quality below `minMatch`) is tried again in parts near where it was
//     almost found (a long aside in the middle, half of it skipped); a line not found at all is skipped and reported.
//     A line said several times (retakes) is corrected wherever it aligns about as well as its best take;
//   * every correction is a WordCorrection on the media's transcript edits (edit::correctTranscript), so it is saved
//     with the project, survives re-transcription and reaches captions on "Update from transcript". One that overlaps
//     a correction made before (by hand, or an earlier run) becomes one correction over both, keeping its words.
//
// transcript.correctFromScript (EditProject) runs it on a sequence as one undo step; dryRun lists the changes. The work
// is text only (an hour of speech and a 10,000-word script take about a second); the app runs it on a worker.

#include <wabcut/ai/roughcut/Script.h>
#include <wabcut/ai/roughcut/Speech.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::roughcut {

struct ScriptCorrectionOptions {
    double minMatch = 0.7;          ///< alignment quality (0..1) a stretch needs before its words are corrected
    double retakeMargin = 0.1;      ///< other takes of a line are corrected when within this much of its best one
    std::size_t candidates = 8;     ///< transcript sentences looked at per line before aligning
    bool fixCase = true;            ///< also give names and terms the script's capitals where only those differ
    /// Called after each line with (lines done, lines); it may throw to stop (the command's checkpoint).
    std::function<void(std::size_t done, std::size_t total)> progress;
};

/// One correction: sequence transcript words first..last read `after` instead of `before`.
struct ScriptCorrection {
    std::size_t beat = 0;           ///< index into Script::beats
    std::size_t first = 0;          ///< words of the sequence transcript, inclusive
    std::size_t last = 0;
    TimeRange timeline;             ///< the words on the timeline
    MediaId media;                  ///< the media item and audio stream the words come from
    int audioStream = 0;
    TimeRange source;               ///< their source time in it
    std::string before;             ///< as transcribed
    std::string after;              ///< from the script
    bool caseOnly = false;          ///< only the capitals change ("kubernetes" -> "Kubernetes")
};

/// A stretch where what was said differs from the script but does not sound like it ("great" where the script says
/// "good"): the speaker changed the words, so they are left as said.
struct ScriptDifference {
    std::size_t beat = 0;
    std::size_t first = 0;          ///< words of the sequence transcript, inclusive
    std::size_t last = 0;
    TimeRange timeline;
    std::string said;               ///< the transcript's words
    std::string script;             ///< the script's
};

struct ScriptCorrectionPlan {
    std::vector<ScriptCorrection> corrections; ///< in transcript order, never overlapping
    std::size_t beats = 0;                     ///< lines of the script
    std::size_t beatsMatched = 0;              ///< lines found well enough (with or without corrections)
    std::size_t beatsPartial = 0;              ///< of those, found only in parts (an aside, a skipped half)
    std::vector<std::size_t> skipped;          ///< lines not found well enough (indices into Script::beats)
    std::vector<double> skippedQuality;        ///< the best quality each skipped line reached (0 = not found)
    std::vector<ScriptDifference> kept;        ///< differences left as said, in transcript order
    std::size_t crossClip = 0;                 ///< corrections left out: their words cross a cut or come from a nest
    std::size_t redacted = 0;                  ///< corrections left out: their words are redacted
    std::vector<std::string> warnings;
};

/// Plans the corrections of `source` (a Sequence source of a SpeechCorpus: its words are the sequence's) from
/// `script`. `sequence` is that sequence in the corpus's state (for the words' clips and media).
ScriptCorrectionPlan planScriptCorrections(const SpeechSource& source, const Sequence& sequence, const Script& script,
                                           const ScriptCorrectionOptions& options = {});

/// True when `said` (transcript words) can be the recogniser's reading of `script`, so the script's spelling may
/// replace it: the same number or abbreviation written another way ("25" / "twenty-five", "Dr." / "Doctor"), or close
/// in letters or in sound ("cooper nettie's" / "Kubernetes", "web cut" / "Wabcut"); false for a different word ("great"
/// / "good", "this" / "that").
bool soundsAlike(std::string_view said, std::string_view script);

/// Adds transcript.correctFromScript (registerRoughCutCommands calls it).
void registerScriptCorrectionCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::roughcut
