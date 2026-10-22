#pragma once

// Premiere's Clip > Group / Ungroup (0.2 parity round).
//
// Grouped clips share a ClipGroupId (Clip::group). Unlike a link (A/V sync, LinkGroupId) a group is only an editing
// convenience, as in Premiere: clicking any member selects the whole group (and the clips linked to its members), so
// it moves, nudges, copies and deletes together; Alt-click picks one member alone. Clips on any tracks and of any
// kind can be grouped. A razor cut keeps both pieces in the group; a paste makes the pasted copies a new group.

#include <wabcut/model/EditOps.h>

#include <span>
#include <vector>

namespace wabcut::edit {

/// Clip > Group (Ctrl+G): puts the clips (at least two of the sequence, none on a locked track) into one new group;
/// they leave the groups they were in (a group of clips that are all named becomes the new one). Errors: NotFound (a
/// clip), InvalidArgument (fewer than two clips, a locked track); nothing changes then.
EditOutcome groupClips(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips);

/// Clip > Ungroup (Ctrl+Shift+G): dissolves every group a named clip belongs to (all its members leave it, as in
/// Premiere). Clips that are not grouped are ignored; no group named: unchanged.
EditOutcome ungroupClips(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips);

/// The clips plus every other member of their groups and the clips linked to any of those (what a click selects):
/// input order first, then the others by track order and time.
std::vector<ClipId> withGroupedClips(const Sequence& sequence, std::span<const ClipId> clips);

/// True when one of the clips is grouped.
bool anyGrouped(const Sequence& sequence, std::span<const ClipId> clips);

/// A ripple edit (B, Ctrl-drag) of several edges at once - a clip group's outer edge, where every member whose edge it
/// is trims: each named clip (with FollowLinks, its linked partners too) moves its `edge` by the same `delta`, clamped
/// so every one of them can (one frame minimum, the media there is), the material after it shifts on its track, and
/// the other sync-locked, unlocked tracks ripple at the first clip's edit point, as rippleTrim does for one clip. One
/// ChangeSet. Errors: NotFound (a clip), InvalidArgument (no clip, a locked track, two of the edges on one track).
EditOutcome rippleTrimEdges(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips, Edge edge,
                            Ticks delta, LinkMode links = LinkMode::FollowLinks);

} // namespace wabcut::edit
