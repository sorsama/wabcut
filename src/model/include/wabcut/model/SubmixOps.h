#pragma once

// p02 audio: submix tracks, track output assignment and sends (Premiere's Audio Track Mixer routing; docs/audio.md
// "Submixes and sends").
//
//   * A submix track (Sequence::submixTracks) is a bus: audio tracks are assigned to it (Track::output) or send to it
//     (Track::sends); its own effects, fader, panner, mute and solo then apply to the sum, and it outputs to the Mix
//     or to another submix. It holds no clips.
//   * Every audio track and submix outputs to exactly one place: the Mix (TrackId{}) or a submix. A submix never
//     outputs back into itself, directly or through other submixes (tracks only feed buses, so sends cannot loop).
//   * A track has at most kMaxTrackSends sends, each to a submix, pre- or post-fader, with its own level, pan and mute.
//
// Pure state-to-state operations (one undo step each through the commands).

#include <wabcut/model/AudioChannels.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wabcut {

/// Why the sequence's audio routing is not valid, or nullopt: outputs and sends name submix tracks of the sequence, a
/// track has at most kMaxTrackSends sends with levels (-144..+24 dB) and pans (-1..1) in range, and no submix outputs
/// back into itself, directly or through others.
std::optional<std::string> audioRoutingProblem(const Sequence& sequence);

/// The submix tracks in processing order (indices into Sequence::submixTracks): every submix after each submix that
/// outputs into it, otherwise in list order. nullopt when the routing has a cycle.
std::optional<std::vector<std::size_t>> submixProcessingOrder(const Sequence& sequence);

/// Whether the sequence routes anything through a submix: it has submix tracks and some audio track outputs or sends to
/// one. Without that the mix is the plain sum of the tracks into the Mix (and players take their simpler path).
bool usesSubmixRouting(const Sequence& sequence);

} // namespace wabcut

namespace wabcut::edit {

/// Adds a submix track (Premiere's Sequence > Add Tracks > Audio Submix Tracks) of `format` (mono, stereo, 5.1 or
/// adaptive) after the last submix, outputting to the Mix. Reported in EditResult::createdTracks. Errors: more than
/// kMaxSubmixTracks.
EditOutcome addSubmixTrack(const ProjectState& state, SequenceId sequence, ChannelFormat format, std::string name = {});

/// Deletes a submix track. Tracks and submixes assigned to it output to the Mix instead, and the sends to it are removed.
/// The submix must be unlocked.
EditOutcome removeSubmixTrack(const ProjectState& state, SequenceId sequence, TrackId submix);

/// Premiere's track output assignment: an audio track or submix outputs to `output`, a submix track of the sequence, or
/// to the Mix (TrackId{}). Errors: a video track, a missing submix, a cycle (a submix into itself or a submix that feeds
/// it), a locked track.
EditOutcome setTrackOutput(const ProjectState& state, SequenceId sequence, TrackId track, TrackId output);

/// Adds a send from an audio track to `send.submix` (at most kMaxTrackSends per track; Premiere's sends A to E).
EditOutcome addTrackSend(const ProjectState& state, SequenceId sequence, TrackId track, TrackSend send);
/// Replaces send `index` of an audio track (its submix, pre/post-fader, mute, level, pan).
EditOutcome setTrackSend(const ProjectState& state, SequenceId sequence, TrackId track, std::size_t index, TrackSend send);
/// Removes send `index` of an audio track.
EditOutcome removeTrackSend(const ProjectState& state, SequenceId sequence, TrackId track, std::size_t index);

} // namespace wabcut::edit
