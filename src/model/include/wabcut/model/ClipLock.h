#pragma once

// 0.7 (card 064): locking individual clips (Clip::locked), without locking their whole track.
//
// A locked clip is frozen: it cannot be moved, trimmed, split, retimed, deleted, overwritten, pushed by a ripple or
// changed in any other way until it is unlocked. It can still be selected, copied and played. Clip::locked is
// independent of Track::locked (a locked track freezes every clip on it and refuses new material).
//
// Every edit is checked in one place: EditSession refuses a state in which a clip that was locked has changed or gone
// (lockedClipProblem). The selection-based edit operations also refuse a locked clip they are asked to change up front
// (SequenceEditor's resolveClips / edgeParticipants), with the same wording, so a direct caller gets the reason without
// a session. Locking and unlocking is the one edit that may touch a locked clip, and it touches nothing but the flag.

#include <wabcut/model/EditOps.h>

#include <optional>
#include <span>
#include <string>

namespace wabcut::edit {

/// Locks (or unlocks) exactly the named clips (no linked partners: the caller adds them). Clips that already have the
/// state are left alone; no clip to change gives an unchanged outcome. NotFound for a clip that is not in the sequence.
/// Works on clips of locked tracks too.
EditOutcome setClipsLocked(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips, bool locked);

/// The first locked clip of `before` that `after` changes: removed, moved to another track or time, trimmed, retimed,
/// edited (effects, keyframes, speed, name, links...) or replaced. Only the `locked` flag itself may differ (unlocking).
/// nullopt: no locked clip is touched. Sequences that `after` no longer has are not checked (deleting a sequence is a
/// project edit, not a clip edit). The message names the clip ("clip 'Lower third' is locked").
std::optional<Error> lockedClipProblem(const ProjectState& before, const ProjectState& after);

/// "clip 'Name' is locked" (the wording of every refusal); "clip N is locked" with the clip's id when it has no name.
std::string lockedClipMessage(const Clip& clip);
/// The error is a lockedClipMessage (the drag readout shows these as they are).
bool isLockedClipError(const Error& error);

} // namespace wabcut::edit
