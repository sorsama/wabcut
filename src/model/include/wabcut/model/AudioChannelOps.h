#pragma once

// A.9: operations on audio channel maps and channel formats (AudioChannels.h): Premiere's Modify > Audio Channels on
// clips and media items, split to mono, and the master's 5.1 format. Pure state-to-state operations (one undo step each
// through the commands). Track formats and 5.1 panners are TrackStateChange fields (ProjectOps.h).

#include <wabcut/model/AudioChannels.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <optional>
#include <span>

namespace wabcut::edit {

/// The map `clip` plays by: its own, else its media item's, else null (the stream decoded to the track as always).
const AudioChannelMap* effectiveChannelMap(const Clip& clip, const MediaItem* media);

/// Sets the channel map of audio clips (nullopt clears it: the media item's map, or the stream as it is, applies again).
/// Errors: a video or nested clip, a locked track, an invalid map (channelMapProblem).
EditOutcome setClipChannels(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                            std::optional<AudioChannelMap> map);

/// Sets a media item's channel map (nullopt clears it); its clips without a map of their own follow it.
std::expected<ProjectState, Error> setMediaChannels(const ProjectState& state, MediaId media, std::optional<AudioChannelMap> map);

/// The master's channel format: Stereo or Surround51 (an error otherwise).
EditOutcome setMasterFormat(const ProjectState& state, SequenceId sequence, ChannelFormat format);

/// Premiere's split to mono for a clip in a sequence: the clip plays source channel sourceChannels[0] as a mono clip,
/// and a copy of it on targets[k - 1] plays sourceChannels[k], over the same time with the same source, level and
/// effects; all of them linked (joining the clip's link group, or a new one). The targets must be unlocked audio tracks
/// with nothing over the clip's range. EditResult::created lists the copies.
EditOutcome splitClipToMono(const ProjectState& state, SequenceId sequence, ClipId clip, std::span<const int> sourceChannels,
                            std::span<const TrackId> targets);

} // namespace wabcut::edit
