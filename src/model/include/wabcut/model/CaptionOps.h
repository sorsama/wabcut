#pragma once

// Caption workshop operations (C.7): caption tracks and cues, styles and presets, speakers and transcript edits.
// Same contract as EditOps.h: pure functions from a state to a new state that fail with an Error and change nothing;
// every result passes validate(). Sequence edits return EditOutcome (one ChangeSet, captionsChanged); project edits
// (presets, speakers, transcript edits) return the new ProjectState.
//
// Cue rules (captionTrackProblem): cues sorted by start, positive durations, starts >= 0, valid UTF-8 text of at most
// kMaxCaptionTextBytes, no overlaps unless the track's style allows them, at most kMaxCaptionCues per track.

#include <wabcut/model/CaptionText.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wabcut::edit {

/// Why `track` breaks the cue rules (or its style or name is invalid), or nullopt.
std::optional<std::string> captionTrackProblem(const CaptionTrack& track);

// ---------------------------------------------------------------------------------------------------
// Tracks and cues (one sequence; NotFound for a missing sequence, track or cue index)

/// A new empty caption track (its id in EditResult::createdCaptionTrack).
EditOutcome addCaptionTrack(const ProjectState& state, SequenceId sequence, std::string name, std::string language,
                            CaptionStyle style = {});
/// Adds a cue where its start belongs. Refused when it overlaps another (unless the style allows it).
EditOutcome addCaptionCue(const ProjectState& state, SequenceId sequence, CaptionTrackId track, CaptionCue cue);

struct CaptionCueChange {
    std::optional<std::string> text;
    std::optional<Ticks> start;
    std::optional<Ticks> end;
    std::optional<SpeakerId> speaker; ///< an invalid id clears it
};
/// Changes a cue's text, times or speaker (the cue keeps its place in start order or moves to it).
EditOutcome updateCaptionCue(const ProjectState& state, SequenceId sequence, CaptionTrackId track, std::size_t index,
                             const CaptionCueChange& change);
EditOutcome removeCaptionCue(const ProjectState& state, SequenceId sequence, CaptionTrackId track, std::size_t index);
/// Cuts a cue at timeline time `at` (strictly inside it) into two with the same speaker. The text splits at byte
/// `textOffset` when given (a UTF-8 boundary), else at the line-break opportunity (`breaker`) whose share of the text
/// is closest to the share of time before `at`; both halves are re-wrapped like the cue (as many lines at most).
/// InvalidArgument when the text has nowhere to split.
EditOutcome splitCaptionCue(const ProjectState& state, SequenceId sequence, CaptionTrackId track, std::size_t index,
                            Ticks at, std::optional<std::size_t> textOffset, const LineBreaker& breaker);
/// Joins cues index .. index + count - 1 (count >= 2) into one from the first start to the last end, their texts
/// flattened and joined (a space between them, none in an unspaced language) and wrapped to `maxCharsPerLine` x
/// `maxLines`; it keeps the first cue's speaker.
EditOutcome mergeCaptionCues(const ProjectState& state, SequenceId sequence, CaptionTrackId track, std::size_t index,
                             std::size_t count, const LineBreaker& breaker, int maxCharsPerLine = 42, int maxLines = 2);
/// Moves the cues that start inside `range` by `delta`. Refused when one would start before 0 or overlap a cue that
/// does not move (unless the style allows overlaps). EditResult::appliedDelta is `delta`.
EditOutcome shiftCaptionCues(const ProjectState& state, SequenceId sequence, CaptionTrackId track, TimeRange range,
                             Ticks delta);
EditOutcome setCaptionStyle(const ProjectState& state, SequenceId sequence, CaptionTrackId track, CaptionStyle style);
/// Renames, relabels the language of, or shows/hides a caption track.
struct CaptionTrackChange {
    std::optional<std::string> name;
    std::optional<std::string> language;
    std::optional<bool> enabled;
};
EditOutcome setCaptionTrackInfo(const ProjectState& state, SequenceId sequence, CaptionTrackId track,
                                const CaptionTrackChange& change);

// ---------------------------------------------------------------------------------------------------
// Presets

/// The style of a preset: the project's (ProjectState::captionPresets) or a built-in one, by name (case-insensitive).
std::optional<CaptionStyle> captionPresetStyle(const ProjectState& state, std::string_view name);
/// Adds a project preset, or replaces the one with that name. Built-in names are refused. 0.2: replacing one restyles
/// every caption track that follows it (CaptionTrack::trackStyle, Premiere's Track Style), in every sequence.
std::expected<ProjectState, Error> saveCaptionPreset(const ProjectState& state, CaptionPreset preset);
/// Removes a project preset; tracks that followed it keep their look and follow nothing.
std::expected<ProjectState, Error> removeCaptionPreset(const ProjectState& state, std::string_view name);
/// 0.2, Premiere's Track Style menu: the track takes the preset's style and follows it (`name` "" = follows nothing; the
/// look stays). Errors: NotFound (track, preset).
EditOutcome setCaptionTrackStyle(const ProjectState& state, SequenceId sequence, CaptionTrackId track, std::string_view name);
/// Premiere's Push to Track Style: the track's style becomes its project preset's, and every track that follows that
/// preset takes it. Errors: InvalidArgument (the track follows no preset, or a built-in one), NotFound.
std::expected<ProjectState, Error> pushCaptionTrackStyle(const ProjectState& state, SequenceId sequence, CaptionTrackId track);

// ---------------------------------------------------------------------------------------------------
// Speakers and transcript edits (Transcripts.h)

std::expected<Created<SpeakerId>, Error> addSpeaker(const ProjectState& state, std::string name,
                                                    std::optional<std::uint32_t> color = std::nullopt);
/// Gives every diarization label of `media`'s `audioStream` transcript in `labels` that has no speaker yet a new one
/// ("Speaker N", the next palette colour). Unchanged state when all have one.
std::expected<ProjectState, Error> registerSpeakerLabels(const ProjectState& state, MediaId media,
                                                         std::int32_t audioStream, std::span<const std::int32_t> labels);
std::expected<ProjectState, Error> renameSpeaker(const ProjectState& state, SpeakerId speaker,
                                                 std::optional<std::string> name, std::optional<std::uint32_t> color);
/// Every reference to `from` (labels, assigned spans, caption cues of every sequence) becomes `into`, and `from` goes.
std::expected<ProjectState, Error> mergeSpeakers(const ProjectState& state, SpeakerId from, SpeakerId into);
/// `speaker` speaks the words of `media`'s `audioStream` transcript whose middle lies in `source` (replacing what was
/// assigned there; an invalid speaker clears the assignment, so the labels decide again).
std::expected<ProjectState, Error> assignSpeaker(const ProjectState& state, MediaId media, std::int32_t audioStream,
                                                 TimeRange source, SpeakerId speaker);
/// Adds a correction (replacing the corrections it overlaps).
std::expected<ProjectState, Error> correctTranscript(const ProjectState& state, MediaId media,
                                                     std::int32_t audioStream, WordCorrection correction);

} // namespace wabcut::edit
