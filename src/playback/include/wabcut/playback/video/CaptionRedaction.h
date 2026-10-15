#pragma once

// Redacted speech in captions (Codex wave-4 P1-3): a word the soundtrack bleeps or mutes (a source-anchored redaction
// with audio, model/Masks.h) is never shown as text either. Burned-in captions are masked where they are evaluated
// (evaluateCaption with the project: the program monitor and export draw the same thing), SRT/VTT sidecars through
// redactCaptionTrack (exporter::writeCaptionSidecar).

#include <wabcut/model/ProjectState.h>
#include <wabcut/model/RedactedSpeech.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::playback {

struct CaptionWordTimings;

/// What a redacted word becomes unless the caption settings say otherwise (CaptionSettings::redactedWord,
/// exporter::CaptionExportSettings::redactedWord).
inline constexpr std::string_view kRedactedWordToken = "[bleep]";

// Where the project's audio redactions bleep or mute a sequence's sound: one definition for every reader of caption text
// (model/RedactedSpeech.h, review wave 5 A5-P1-4 / S5-P1-2).
using ::wabcut::hasSpeechRedactions;
using ::wabcut::redactedSpeechRanges;

/// The text of `cue` (of caption track `track`) with the words spoken inside `ranges` replaced by `token`. Its words are
/// render::captionWordSpans() of the text; when `words` times every word of the cue (the karaoke timings: track id and
/// cue start), exactly the words overlapping a range are replaced. Without them which word was redacted is unknown, so
/// a cue overlapping a range becomes `token` as a whole. A cue outside every range keeps its text.
std::string redactedCueText(CaptionTrackId track, const CaptionCue& cue, std::span<const TimeRange> ranges,
                            const CaptionWordTimings* words, std::string_view token = kRedactedWordToken);

/// `track` with every cue's text through redactedCueText().
CaptionTrack redactCaptionTrack(const CaptionTrack& track, std::span<const TimeRange> ranges,
                                const CaptionWordTimings* words, std::string_view token = kRedactedWordToken);

} // namespace wabcut::playback
