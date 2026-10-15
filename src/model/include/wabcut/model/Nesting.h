#pragma once

// Nested sequences and multicam (C.8).
//
// A clip may play a sequence instead of media (Clip::sequence): Premiere's nested sequence, Final Cut's compound
// clip, Resolve's compound clip. Its sourceIn, speed, direction and time remap map the timeline into the nested
// sequence's time exactly as they map a media clip into media time (Clip::sourceTimeAt, integer ticks), so trims,
// slips, speed changes and remaps of a nested clip work like those of any clip. What it plays:
//   * a video clip (stream Video 0): the nested sequence's composited picture - its video tracks from V1 up, each
//     with its track effects, then the nested sequence's master video effects; then the nested clip's own intrinsic
//     Motion/Opacity and effect stack, as for any clip;
//   * an audio clip (stream Audio 0): the nested sequence's mix - every audio track with its fader, pan and track
//     effects summed to the nested master (stereo), its master audio effects and master fader; then the nested
//     clip's Volume/Panner and effects. The nested sequence's tracks are not exposed one by one (Premiere's default
//     nest: one stereo clip carries the mix).
// Past the nested sequence's duration a nested clip shows and plays nothing (the nested sequence may be shortened
// after its clips were cut); trims cannot extend it past that duration.
//
// A multicam source is a sequence with Sequence::multicam set: its angles are video tracks and/or audio tracks of
// that sequence, each synced by an offset. A multicam clip is a clip of that sequence with Clip::angles: a video
// clip shows exactly one angle's video track (alone: no other track, no master effects of the source), an audio
// clip mixes the audio tracks of the angles it has (faders and track effects of those tracks, no master stage).
// Angle cuts are ordinary razor cuts plus switchMulticamAngle, so trims, ripple, roll and undo work on them.
//
// A sequence never contains itself, directly or through other sequences (validate() reports a cycle; every op that
// would make one refuses). Leaf media inside nested sequences is resolved by resolveLeaves() (Resolve.h), which
// render and audio use; source-anchored redactions apply to that leaf media by media time, whatever nests it.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

/// Bound on an unknown clip source kept from a newer project (ClipContent::unknownSource): files are untrusted.
inline constexpr std::size_t kMaxUnknownSourceBytes = 64 * 1024;

std::string_view toString(MulticamSync sync);
std::optional<MulticamSync> multicamSyncFromString(std::string_view name);

/// Whether a clip of `sequence` can play `stream`: Video 0 when it has video tracks, Audio 0 when it has audio
/// tracks (a multicam source: when an angle has tracks of that kind).
bool sequenceHasStream(const Sequence& sequence, StreamRef stream);

/// How much source a clip can use: its media stream's extent (MediaItem::sourceExtent), or the nested sequence's
/// duration; nullopt when unbounded or unknown (also for a missing source and an unknown source kind).
std::optional<Ticks> clipSourceExtent(const ProjectState& state, const Clip& clip);

/// The sequences `sequence` plays directly (its nested and multicam clips), sorted, without duplicates.
std::vector<SequenceId> nestedSequencesOf(const Sequence& sequence);

/// Whether `outer` plays `inner`, directly or through other sequences (`outer == inner` counts).
bool sequenceContains(const ProjectState& state, SequenceId outer, SequenceId inner);

/// A cycle of sequences that contain each other ("A -> B -> A"), or nullopt. validate() reports it.
std::optional<std::string> nestingCycle(const ProjectState& state);

/// Nested and multicam clip instances one sequence may reach through every level (each clip of a nested sequence
/// counts once per clip that plays that sequence): far beyond any real project, and what keeps a crafted file of
/// doubling nests (each level playing the next twice) from expanding 2^depth layers (review wave 5 E5-P2-9).
inline constexpr std::uint64_t kMaxNestedClipPaths = 10'000'000;

/// The first sequence whose nesting fans out past `limit` clip instances, described, or nullopt (acyclic or not:
/// a cycle counts as past the limit). Memoized: linear in the project's clips. validate() reports it.
std::optional<std::string> nestingFanOutProblem(const ProjectState& state, std::uint64_t limit = kMaxNestedClipPaths);

/// What is wrong with a multicam source (the angle list, its tracks and offsets), or nullopt.
std::optional<std::string> multicamProblem(const Sequence& sequence);

/// The angle whose track `track` is, or nullopt.
std::optional<std::size_t> multicamAngleOfTrack(const MulticamInfo& multicam, TrackId track);

/// What is wrong with the angles of a clip (Clip::angles) against its source, or nullopt: a multicam clip needs one
/// angle with a video track (video) or angles with audio tracks (audio); other clips have none.
std::optional<std::string> clipAnglesProblem(const ProjectState& state, const Clip& clip);

/// How a long analysis is stopped and followed: a stop asked for on `stop` ends it (an ErrorCode::Cancelled error),
/// `progress` hears 0..1 on the calling thread (null: not followed). multicam.create passes its command's checkpoints.
struct AnalysisControl {
    std::stop_token stop;
    std::function<void(double)> progress;
};

/// Finds the time offsets that sync media items by their audio (the audio engineer's waveform cross-correlation;
/// the host provides it, see command::MulticamServices). `angles[i]`'s result is where its audio stream 0 starts
/// relative to angles[0]'s (positive = later); createMulticam normalises them. Fails when it cannot sync them.
class AudioSyncAnalyzer {
public:
    virtual ~AudioSyncAnalyzer() = default;
    virtual std::expected<std::vector<Ticks>, Error> offsets(const ProjectState& state,
                                                             std::span<const MediaId> angles) const = 0;
    /// The same, stopped and followed through `control` (an analysis of hour-long angles takes seconds). The default
    /// runs offsets() above, which neither stops nor reports.
    virtual std::expected<std::vector<Ticks>, Error> offsets(const ProjectState& state, std::span<const MediaId> angles,
                                                             const AnalysisControl& control) const {
        (void)control;
        return offsets(state, angles);
    }
};

/// A deterministic AudioSyncAnalyzer for tests and scripted hosts: the offsets it was given, by media (media it was
/// not given sync at 0; none given fails, like an analysis that found no match).
class OffsetTableAudioSync final : public AudioSyncAnalyzer {
public:
    explicit OffsetTableAudioSync(std::vector<std::pair<MediaId, Ticks>> table) : m_table(std::move(table)) {}
    using AudioSyncAnalyzer::offsets;
    std::expected<std::vector<Ticks>, Error> offsets(const ProjectState& state,
                                                     std::span<const MediaId> angles) const override;

private:
    std::vector<std::pair<MediaId, Ticks>> m_table;
};

namespace edit {

/// Premiere's Nest: the clips (with `links`, their linked partners) leave `sequence` for a new sequence in the same
/// bin, with the same settings, and one nested clip per kind replaces them, linked: a video clip on the lowest
/// video track of the selection and an audio clip (the nested mix) on the lowest audio track, spanning the
/// selection from its first clip's start to its last clip's end. Inside, the clips keep their tracks (V2 stays V2),
/// their times relative to that start, their ids, effects, links among themselves (links to clips left outside are
/// dropped) and the transitions between two of them; a transition with a clip left outside goes. One undo step.
/// Refuses an empty selection, locked tracks, an unselected clip inside that span on a track the nested clips go to,
/// and a selection whose clips already play the sequence (impossible: it is new).
struct NestResult {
    EditResult edit;
    SequenceId nested;            ///< the new sequence
    std::vector<ClipId> clips;    ///< the nested clips (video first)
};
std::expected<NestResult, Error> nestClips(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                                           std::string name = {}, LinkMode links = LinkMode::FollowLinks);

/// Resolve's Decompose (Premiere has no Unnest): the nested clip (and its linked nested partners of the same
/// sequence over the same time) is replaced by copies of the nested sequence's clips inside the window it shows, on the clip's track
/// and the ones above it (nested V2 -> one above the clip's video track; audio likewise), trimmed to the window,
/// with new ids, their effects, links and the transitions between them. Only where that is exact; else it refuses
/// with the reason: a retimed or reversed nested clip, effects or non-default Motion/Opacity/Volume on it, master or
/// track effects, track faders or disabled/muted tracks inside, a multicam source (flattenMulticam), a missing
/// destination track, a clip in the way, a transition across the window's edge. One undo step.
EditOutcome unnestClip(const ProjectState& state, SequenceId sequence, ClipId clip);

/// What multicam.create builds.
struct MulticamSpec {
    std::string name;          ///< default: "<first angle> Multicam"
    BinId bin;                 ///< default: the first angle's bin
    std::vector<MediaId> angles;
    MulticamSync sync = MulticamSync::Timecode;
    /// Audio sync's analysis (required for MulticamSync::Audio), and how it is stopped and followed.
    const AudioSyncAnalyzer* audioSync = nullptr;
    AnalysisControl audioSyncControl;
    /// Offsets for MulticamSync::Manual (one per angle; default 0).
    std::vector<Ticks> offsets;
    std::vector<std::string> angleNames; ///< default: the media names
};
/// Premiere's Create Multi-Camera Source Sequence: a new sequence whose angle i holds media angles[i] - its video on
/// V(i+1) when it has video, each of its audio streams on its own audio track - placed at the angle's sync offset
/// (the earliest angle at 0). Settings come from the first angle with video. The offsets: Timecode lines up the
/// media's start timecodes (MediaInfo::startTimecode), InPoints / OutPoints their Source Monitor marks (the media's start
/// / end when unset), Markers their first clip markers, Audio the analyzer's result, Manual `offsets`. Fails on no
/// angles, more than kMaxMulticamAngles, a missing medium, a medium without the timecode / mark / marker its method
/// needs, an offset past kMaxMulticamOffset.
std::expected<Created<SequenceId>, Error> createMulticam(const ProjectState& state, const MulticamSpec& spec);

/// Which clips of a multicam edit an angle switch changes.
enum class AngleTarget : std::uint8_t { Video, Audio, Both };
/// Premiere's angle switch (the 1-9 keys while playing): the multicam clip under `at` on the video side (Video,
/// Both) and/or its linked audio multicam clips (Audio, Both) are cut at `at` (no cut at a clip's start) and the part
/// from `at` to that clip's end plays `angle` (an audio clip: that angle alone). Without `at` the whole clip
/// switches. `clip` is either clip of the pair. Cutting both sides links the right pieces to each other, like a razor
/// cut; cutting one side keeps its right piece in the link group with the uncut side, so later switches still find
/// it. Refuses a clip that is not a multicam clip, an angle out of range or without that kind of track, `at` outside
/// the clip, locked tracks.
EditOutcome switchMulticamAngle(const ProjectState& state, SequenceId sequence, ClipId clip, std::optional<Ticks> at,
                                std::size_t angle, AngleTarget target = AngleTarget::Both);

/// Premiere's Flatten: the multicam clip (and its linked multicam partners over the same time) is replaced by copies of the clips its
/// active angles play inside its window - video: the angle's video track; audio: each active angle's audio tracks,
/// on the clip's audio track and the ones below it - trimmed to the window, with new ids and new links. Exact only:
/// refuses retimed or reversed multicam clips, effects or non-default intrinsic parameters on them, a missing
/// destination track or a clip in the way.
EditOutcome flattenMulticam(const ProjectState& state, SequenceId sequence, ClipId clip);

/// Sets an angle's sync offset in a multicam source: its clips move by the difference (so every multicam clip shows
/// the angle shifted), the source's duration grows to its content, and its sync method becomes Manual. Refuses an
/// offset outside [0, kMaxMulticamOffset].
EditOutcome setMulticamSync(const ProjectState& state, SequenceId multicam, std::size_t angle, Ticks offset);

} // namespace edit

} // namespace wabcut
