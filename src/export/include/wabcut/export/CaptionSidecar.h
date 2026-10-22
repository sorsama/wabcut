#pragma once

// Caption sidecars (I.4): an export with CaptionExport::Srt / ::Vtt writes the sequence's caption track next to the
// output file ("<output>.srt" / ".vtt"), cue times relative to the start of the exported range (cues outside it
// are left out, cues across its edges are cut to it). Words the soundtrack bleeps or mutes are replaced by the
// settings' redactedWord (playback/video/CaptionRedaction.h, Codex wave-4 P1-3).

#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <expected>
#include <filesystem>
#include <string>

namespace wabcut::exporter {

/// The caption track `captions` exports from `sequence` (its `track`, else the first enabled one); null: none.
const CaptionTrack* captionTrackToExport(const Sequence& sequence, const CaptionExportSettings& captions);

/// SubRip (`format` Srt) or WebVTT (Vtt) text of `track`'s cues within `range`, shifted to start at 0. Lines keep
/// their breaks; times are rounded to milliseconds. Cue text is plain text (Codex wave-3 P2-10): line ends are
/// normalised and blank lines dropped (one would end the cue); WebVTT escapes & < > (so no "-->" either); SubRip, which
/// has no escapes, gets an invisible zero-width space inside "<x", "</", "{\" and "-->" so players read no tag, no
/// override and no timing line. A cue with no visible text is left out.
std::string captionSidecarText(const CaptionTrack& track, TimeRange range, CaptionExport format);

/// `track` (of `sequence` in `project`) as caption files carry it (P.2, captions.export and the new sidecar formats):
/// the words the soundtrack bleeps or mutes replaced by `redactedWord`, as in sidecars and burn-in.
CaptionTrack captionTrackForFile(const ProjectState& project, const Sequence& sequence, const CaptionTrack& track,
                                 std::string_view redactedWord = kRedactedWordToken,
                                 const playback::CaptionWordTimings* words = nullptr);

/// Writes the sidecar of an export of `sequence` (of `project`, whose redactions apply) to "<output>" with the
/// format's extension. Returns the file written, or an empty path when there is nothing to write (no caption track,
/// or a mode without a sidecar). Errors: IoError.
std::expected<std::filesystem::path, Error> writeCaptionSidecar(const ProjectState& project, const Sequence& sequence,
                                                                const CaptionExportSettings& captions, TimeRange range,
                                                                const std::filesystem::path& output);

} // namespace wabcut::exporter
