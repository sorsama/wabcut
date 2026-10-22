#pragma once

// Fade handles (0.2 parity round, Premiere 25.x/26 timeline fade handles): a clip's fade in / fade out is a one-sided
// transition inside the clip at that edge, so it renders, exports and edits like any transition (Effect Controls,
// the transition's edges on the timeline, Delete):
//   video  Cross Dissolve from / to transparency, i.e. from / to black over an empty track below (Premiere's fade)
//   audio  Constant Power, a fade from / to silence
// Dragging a fade handle sets the length; a length of 0 removes the fade.

#include <wabcut/model/EditOps.h>

#include <optional>

namespace wabcut::edit {

/// The length of the clip's fade at `edge` (its one-sided transition there), or nullopt.
std::optional<Ticks> clipFade(const Sequence& sequence, ClipId clip, Edge edge);

/// Sets the clip's fade at `edge` (Head = fade in, Tail = fade out) to `duration`, clamped to the clip's length:
/// adds the one-sided transition (Cross Dissolve on video, Constant Power on audio), changes its length, or removes it
/// when `duration` is 0. Errors: NotFound (the clip), InvalidArgument (a locked track, a negative duration, a two-sided
/// transition on that edge already).
EditOutcome setClipFade(const ProjectState& state, SequenceId sequence, ClipId clip, Edge edge, Ticks duration);

} // namespace wabcut::edit
