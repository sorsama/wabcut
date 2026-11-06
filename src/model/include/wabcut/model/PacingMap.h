#pragma once

// 0.7 (card 180): the pacing heat strip's analysis - where an edit may lose a viewer. Nothing is sent anywhere and nothing
// is guessed by a model: every stretch is graded from signals measured on this machine,
//   * shot length  the time since the picture last changed (the top visible clip changed; a cut through the same media is
//                  not a change), however long the current shot has run;
//   * stillness    how little the picture moves inside a long shot (the media's motion curve, MediaActivity);
//   * silence      how long the sound stays below a quiet level (the media's loudness curve, MediaActivity);
//   * speech       how slowly the words come (words a minute around the moment, from the sequence's transcript, when it
//                  has one).
// Each signal grades 0..1 (0 = fine); the score of a moment is the noisy-or of the weighted grades, so two weak reasons add
// up and one strong reason is enough. Stretches of equal level (1 easing off, 2 slow, 3 drags) are merged; each keeps
// the reasons that made it and the numbers behind them, for the tooltip. Pure functions of their inputs: the same edit
// and the same measurements always give the same strip. The host (PacingBridge) measures the media off the GUI thread.

#include <wabcut/model/GamingOps.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wabcut {

/// Why a moment drags (bit flags).
enum PacingReason : std::uint8_t {
    kPacingLongShot = 1,   ///< no cut for a long time
    kPacingStill = 2,      ///< and little moving in the picture
    kPacingSilence = 4,    ///< the sound has been quiet for a while
    kPacingSlowSpeech = 8, ///< the words come slowly
};

struct PacingOptions {
    double stepSeconds = 1.0;          ///< one cell of the strip
    double longShotSeconds = 8.0;      ///< a shot starts to count after this long ...
    double veryLongShotSeconds = 24.0; ///< ... and counts in full at this
    double stillAfterSeconds = 3.0;    ///< stillness counts once the shot has run this long ...
    double stillFullSeconds = 10.0;    ///< ... in full at this
    double stillLevel = 0.12;          ///< motion (0..1, normalised) at which the picture is as good as still
    double silenceLevel = 0.06;        ///< loudness (0..1, normalised) under which the sound is silent
    double silenceSeconds = 2.0;       ///< silence starts to count after this long ...
    double verySilentSeconds = 6.0;    ///< ... in full at this
    double slowWordsPerMinute = 100.0; ///< speech slower than this counts (a lively delivery is 140 to 180)
    double wordWindowSeconds = 12.0;   ///< the span the words a minute are counted over, centred on the moment
    double minStretchSeconds = 3.0;    ///< shorter stretches (below level 3) are left off the strip
};

/// Why options are out of range, or nullopt.
std::optional<std::string> pacingOptionsProblem(const PacingOptions& options);

/// What the analysis reads.
struct PacingInputs {
    Ticks start;                       ///< the span analysed: [start, end)
    Ticks end;
    std::vector<Ticks> cuts;           ///< where the picture changes, sorted (not the span's own ends)
    std::vector<TimeRange> picture;    ///< where a clip shows, sorted and not overlapping: outside it the frame is empty
    ActivityCurve audio;               ///< loudness per sample, 0..1 (empty: not measured)
    ActivityCurve motion;              ///< picture motion per sample, 0..1 (empty: not measured)
    std::vector<TimeRange> unmeasured; ///< stretches whose sound and motion are not known (media not measured yet)
    std::vector<Ticks> words;          ///< the start of each spoken word on the timeline, sorted (empty: no transcript)
};

/// One cell of the strip.
struct PacingCell {
    Ticks start;
    float score = 0.0F;          ///< 0..1
    std::uint8_t reasons = 0;    ///< PacingReason bits whose weighted grade is 0.2 or more
    float shotSeconds = 0.0F;    ///< the whole shot the cell is in (0: no picture)
    float silentSeconds = 0.0F;  ///< the silent run the cell is in (0: not silent or not measured)
    float wordsPerMinute = -1.0F; ///< around the cell; -1: no transcript, 0: nobody speaks there
    float motion = -1.0F;        ///< the picture's motion there; -1: not measured
};

/// A merged run of cells of one level.
struct PacingStretch {
    TimeRange range;
    int level = 0;               ///< 1 easing off (score >= 0.3), 2 slow (>= 0.5), 3 drags (>= 0.7)
    double peak = 0.0;           ///< the highest score in it
    std::uint8_t reasons = 0;    ///< the union of the cells' reasons
    double longestShot = 0.0;    ///< seconds: the longest shot it touches
    double longestSilence = 0.0; ///< seconds
    double slowestWpm = -1.0;    ///< the lowest words a minute (> 0) among its cells; -1: none
    double meanMotion = -1.0;    ///< the mean motion; -1: not measured
};

/// The score thresholds of the levels.
inline constexpr double kPacingLevel1 = 0.3;
inline constexpr double kPacingLevel2 = 0.5;
inline constexpr double kPacingLevel3 = 0.7;
inline constexpr int pacingLevelOf(double score) {
    return score >= kPacingLevel3 ? 3 : score >= kPacingLevel2 ? 2 : score >= kPacingLevel1 ? 1 : 0;
}

/// The cells of the analysis, one per `stepSeconds` from `start` (at most 20,000: a longer span gets wider cells).
std::vector<PacingCell> pacingCells(const PacingInputs& inputs, const PacingOptions& options = {});

/// The stretches of `cells` (level 1 and up), in time order, merged by level.
std::vector<PacingStretch> pacingStretches(const std::vector<PacingCell>& cells, Ticks end, const PacingOptions& options = {});

/// The cuts and the picture ranges of a sequence's visible video (the top clip at each moment among the enabled clips of
/// the tracks that show: eye on, soloed ones alone), and the span [0, content end). A cut is where the top clip changes to
/// another clip, or the picture begins or ends; a cut through the same media (a through edit) is none. Audio, motion and
/// words are the host's to add.
PacingInputs pacingInputsOf(const Sequence& sequence);

/// Adds the measured curves to `inputs` (the sequence's dead-time curves from `activity`, findDeadTime's audio and motion
/// curves) and the unmeasured stretches (clips of the listened tracks whose media `activity` has nothing for). Returns
/// those media, for the host to measure.
std::vector<MediaId> addPacingActivity(PacingInputs& inputs, const ProjectState& state, SequenceId sequence,
                                       const ActivityLookup& activity);

} // namespace wabcut
