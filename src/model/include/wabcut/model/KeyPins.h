#pragma once

// 0.7 (card 120): keyframes that follow edits.
//
// Clip keyframes are in media time (Parameters.h), so trimming or slipping a clip leaves them on the picture: a fade-in
// keyed at the head of a clip is left behind, hidden, when the head is trimmed. A keyframe can be PINNED to the clip's
// start or end (Keyframe::pin, Effect Controls > right click a key > Pin to Clip Start / End): when an edit moves that end
// of the clip - a trim, a slip, a roll, a speed change, a ripple trim - the key moves with it and keeps its value.
//
// The follow-up is one rule in one place, like the clip lock: EditSession runs followPinnedKeyframes() on every state it
// is about to commit or show as a preview, so no edit operation has to know about pins. A pin is honoured only while the
// key sits exactly on the end of the clip it belongs to (a razor cut gives both halves one curve, and the key stays with
// the half that still owns its end). Undo and redo restore states as they were; they do not run it.

#include <wabcut/model/FinishingOps.h>

#include <optional>

namespace wabcut::edit {

/// Pins key `index` of a clip parameter to the clip's start or end (the key moves to that end's curve time, keeping its
/// value; a key already there is replaced), or unpins it (KeyPin::None; the key stays where it is). Errors: the clip or
/// parameter does not exist, the key index is out of range, the clip's track is locked, or the parameter is not a curve.
EditOutcome setKeyframePin(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                           std::size_t index, KeyPin pin);

/// `after` with the keys that `before` -> `after` left behind moved to the ends they are pinned to: for every clip in both
/// whose start or end moved in curve time, the pinned keys that sat on its old start / end go to the new one (a moved key
/// replaces the key it lands on). nullopt: nothing to move (the common case: no clip has a pinned key).
std::optional<ProjectState> followPinnedKeyframes(const ProjectState& before, const ProjectState& after);

} // namespace wabcut::edit
