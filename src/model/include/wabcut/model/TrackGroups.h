#pragma once

// Track groups (0.7, card 124): a named set of video or audio tracks (Dialogue, SFX, Music) that has a header row in the
// timeline, folds its tracks into that row when collapsed, and mutes, solos, locks, targets and routes them together.
//
// A group is metadata of its sequence (Sequence::trackGroups, TrackGroup): its tracks keep their clips, links, effects and
// ids. The operations keep a group's tracks next to each other in their stack (creating a group gathers its tracks at the
// place of the first one; adding a track moves it next to the group; leaving moves a track from the middle to the end), a
// track belongs to at most one group, and a group whose last track goes is dissolved. Every operation is one undo step.
// Group-wide switches are the tracks' own (TrackStateChange): the group applies one change to all its tracks, so the
// mixer, the track headers and exports see ordinary tracks. A collapsed group is a view of the timeline: its tracks play,
// render and export as before.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Sequence.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace wabcut::edit {

/// Fields to change on a group; unset fields keep their value.
struct TrackGroupChange {
    std::optional<std::string> name;        ///< not empty, at most kMaxTrackGroupNameBytes bytes of UTF-8
    std::optional<std::int32_t> colorIndex; ///< 0..15 (the label palette), or -1 for none
    std::optional<bool> collapsed;
};

/// "Group N": the first number not used by a group of the sequence.
std::string defaultTrackGroupName(const Sequence& sequence);

/// Group Tracks: makes a group of `tracks` (at least one; video or audio tracks of one kind, none in a group already).
/// They are gathered at the place of the first of them, in their stack order. An empty `name` is "Group N"; `colorIndex`
/// is -1 or 0..15. EditResult::createdTrackGroup names the group. Errors: NotFound (a track), InvalidArgument (mixed kinds,
/// a submix track, a track in a group, a name or colour out of bounds, too many groups).
EditOutcome createTrackGroup(const ProjectState& state, SequenceId sequence, std::span<const TrackId> tracks,
                             std::string name = {}, std::int32_t colorIndex = -1);

/// Rename, label colour, collapse / expand. No change: unchanged.
EditOutcome setTrackGroup(const ProjectState& state, SequenceId sequence, TrackGroupId group, const TrackGroupChange& change);

/// Ungroup: the group goes, its tracks stay where they are.
EditOutcome ungroupTracks(const ProjectState& state, SequenceId sequence, TrackGroupId group);

/// Add to Group: the tracks (of the group's kind) join it and move next to its last track; a track in another group
/// leaves that one. Tracks already in the group stay.
EditOutcome addTracksToTrackGroup(const ProjectState& state, SequenceId sequence, TrackGroupId group,
                                  std::span<const TrackId> tracks);

/// Remove from Group: each track leaves its group (tracks in no group are ignored). A track in the middle of its group
/// moves to the group's end so the group stays whole; a group left without tracks is dissolved.
EditOutcome removeTracksFromTrackGroups(const ProjectState& state, SequenceId sequence, std::span<const TrackId> tracks);

/// Applies one track-header change (mute, solo, lock, target, output, sync lock, label colour, role...) to every track of
/// the group, in one step. Tracks that already have it stay as they are; nothing to change: unchanged. Errors are
/// setTrackState's (for the first track that refuses it).
EditOutcome setTrackGroupTracksState(const ProjectState& state, SequenceId sequence, TrackGroupId group,
                                     const TrackStateChange& change);

/// Output of an audio group: every track goes to `submix` (a submix track of the sequence), or to the Mix when `submix`
/// is invalid. Errors: InvalidArgument (a video group), NotFound, and setTrackOutput's (a routing loop).
EditOutcome routeTrackGroupToSubmix(const ProjectState& state, SequenceId sequence, TrackGroupId group, TrackId submix);

/// Route to New Submix: adds a stereo submix track named after the group and routes the group's tracks to it, so one
/// fader and one effect stack work on the whole group. EditResult::createdTracks names the submix.
EditOutcome routeTrackGroupToNewSubmix(const ProjectState& state, SequenceId sequence, TrackGroupId group);

/// Keeps the groups right after `moved` was put at a new place in its stack (moveTrack): a member that no longer touches
/// another member of its group leaves it; a track that now sits between two members of a group joins it. Group lists are
/// rebuilt in stack order, empty groups go. True when a group changed.
bool regroupAfterMove(Sequence& sequence, TrackId moved);

/// Drops from every group the tracks that are not (or no longer) tracks of the sequence, and empty groups; rebuilds each
/// list in stack order. For a merged sequence and a loaded file. True when something changed.
bool pruneTrackGroups(Sequence& sequence);

/// Why the groups of a sequence are not allowed, or nullopt: at most kMaxTrackGroups; each group has a name of 1..
/// kMaxTrackGroupNameBytes bytes of UTF-8, a colour of -1..15, a kind and at least one member; members are distinct tracks
/// of that kind and in stack order; no track is in two groups; ids are distinct and valid.
std::optional<std::string> trackGroupsProblem(const Sequence& sequence);

} // namespace wabcut::edit
