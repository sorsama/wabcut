#pragma once

// Pre-export check (0.7): one list of what is likely to go wrong with an export, worked out from the project and the
// export settings alone (nothing is rendered or decoded), so it is cheap enough to run before every export. Each row
// is a CheckIssue: a kind, how serious it is, where on the timeline it is (the app jumps there), numbers and names
// for the app to put in a sentence in the user's language, and the fix Wabcut can apply, if there is one.
//
// What it looks at:
//   * original media that is offline or missing (the export would stop on it: ExportJob's own test, missingOriginals);
//   * gaps in the picture inside the exported range (black frames) and flash frames (video clips of one or two frames);
//   * clips whose frame rate differs from the sequence's, and variable frame rate files;
//   * a Warp Stabilizer / Rolling Shutter Repair that was never analysed (it exports unstabilised);
//   * effects that take long to render (a fixed list of the expensive ones);
//   * an export whose shape or size does not suit the sequence (black bars, a crop, a stretch, a scale up);
//   * captions: the export is set to show them but the sequence has none in range, or the sequence has captions that
//     the file will not show;
//   * loudness and clipped audio, when a measurement of the mix is supplied (CheckContext::loudness: measuring renders
//     the whole mix, so the app does it on request, off the GUI thread).
//
// Not looked at (they need the text layout of the renderer): title and caption text past the title-safe area.

#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::exporter {

/// How serious a row is. Problem: the export fails or the file is wrong. Warning: probably not what you want. Note:
/// good to know.
enum class CheckSeverity : std::uint8_t { Problem, Warning, Note };

/// What a row is about.
enum class CheckKind : std::uint8_t {
    InvalidSettings,       ///< the export settings do not resolve (subject: why)
    OfflineMedia,          ///< original media that is offline or missing (subject: its name)
    Gap,                   ///< no picture for `length` from `at`
    FlashFrame,            ///< a video clip of one or two frames (subject: its name)
    MixedFrameRate,        ///< clips at another frame rate (subject: "24 fps", count: clips) or variable frame rate
    UnanalysedStabilizer,  ///< a stabilizer that was never analysed (subject: the clip)
    SlowEffects,           ///< clips with effects that render slowly (subject: the effects' names, count: clips)
    SizeMismatch,          ///< the export's shape or size does not suit the sequence (subject: what happens)
    CaptionsNotInFile,     ///< the sequence has captions (count) that the file will not show
    CaptionsMissing,       ///< the export shows captions but the sequence has none in the range
    LoudnessOffTarget,     ///< measured loudness is away from the platform's target (value, other: target)
    AudioClips,            ///< the true peak is at or above the ceiling (value: dBTP)
    DynamicMetadataDropped, ///< 0.7: clips carry HDR10+ or Dolby Vision metadata that the HDR export does not keep (subject: "hdr10plus" or "dolbyVision", count: clips)
};

/// The fix Wabcut can apply to a row, by id. The app does the work (it owns the edit and the export's choices).
enum class CheckFix : std::uint8_t {
    None,
    Relink,            ///< open Relink Media
    CloseGap,          ///< Sequence > Close Gap at this gap (clip: the clip after it)
    TurnOnLoudness,    ///< switch Loudness Normalization on for this export (to the platform's target)
    MatchSequenceSize, ///< export at the sequence's own size
    TurnOnCaptions,    ///< burn the captions into the picture
};

struct CheckIssue {
    CheckKind kind = CheckKind::OfflineMedia;
    CheckSeverity severity = CheckSeverity::Warning;
    CheckFix fix = CheckFix::None;
    /// Where on the timeline it is (sequence time); unset: the row has no place.
    std::optional<Ticks> at;
    /// How long (a gap, a flash frame's clip).
    Ticks length{};
    ClipId clip;   ///< the clip it is about (CloseGap: the one after the gap)
    TrackId track; ///< its track
    std::string subject;   ///< a name, a rate, an effect list: what the sentence is about
    std::int64_t count = 1; ///< how many things this row stands for
    double value = 0.0;     ///< a measured number (LUFS, dBTP)
    double other = 0.0;     ///< a second number (the target)
    /// The last row of a kind that has more rows than kMaxCheckRowsPerKind: "and `count` more", no place, no fix.
    bool more = false;
};

/// "problem", "warning", "note".
std::string_view checkSeverityName(CheckSeverity severity);
/// "offlineMedia", "gap", ...: stable ids for commands and tests.
std::string_view checkKindName(CheckKind kind);
/// "none", "relink", "closeGap", "turnOnLoudness", "matchSequenceSize", "turnOnCaptions".
std::string_view checkFixName(CheckFix fix);

/// The row in one plain English sentence, for hosts that do not translate (the export.check command). `sequence` gives
/// the frame rate that frame counts are worked out in.
std::string checkIssueMessage(const CheckIssue& issue, const Sequence& sequence);

/// What the mix measured to, when the app measured it (CheckContext::loudness).
struct LoudnessReading {
    double integratedLufs = 0.0; ///< -inf for silence
    double truePeakDbtp = 0.0;
};

/// What the check cannot know from the project and the settings.
struct CheckContext {
    /// The project file, for media paths relative to it.
    std::filesystem::path projectFile;
    /// The loudness of the mix over the exported range, once measured.
    std::optional<LoudnessReading> loudness;
    /// The loudness the export's platform plays at (-14 LUFS for the streaming sites), or unset when it has none.
    std::optional<double> platformLufs;
    /// A short name for that platform ("YouTube"), for the sentence.
    std::string platformName;
};

/// The rows for exporting `sequence` of `state` with `settings`, most serious first, then in timeline order. Never
/// fails: settings that do not resolve give one InvalidSettings row.
std::vector<CheckIssue> preExportCheck(const ProjectState& state, const Sequence& sequence, const ExportSettings& settings,
                                       const CheckContext& context = {});

/// The loudness and true peak of the mix an export of `sequence` with `settings` would write, over the exported range
/// (the export's own mix: plug-ins, channel layout and sample rate included), measured without a sound card, faster than
/// real time. Reads the original media; `state` is the project as the export would take it. Errors: NotFound (media
/// missing), InvalidArgument (the settings do not resolve), Cancelled.
std::expected<LoudnessReading, Error> measureExportLoudness(const ProjectState& state, const Sequence& sequence,
                                                            const ExportSettings& settings,
                                                            const std::filesystem::path& projectFile,
                                                            const std::stop_token& stop);

/// The most rows of one kind (a project with 300 gaps lists the first ones and counts the rest in the last row).
inline constexpr std::size_t kMaxCheckRowsPerKind = 12;

} // namespace wabcut::exporter
