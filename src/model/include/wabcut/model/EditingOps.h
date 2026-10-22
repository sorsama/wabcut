#pragma once

// Premiere's editing commands (P.2, E5) that Wabcut lacked: Replace Edit and Replace Footage, Make Offline, Make
// Subsequence, Join Through Edits, Go to Gap and Close Gap, Frame Hold Options, Merge Clips, Interpret Footage and
// Modify > Timecode. Pure functions like EditOps.h; one ChangeSet (one undo step) each.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::edit {

// ---------------------------------------------------------------------------------------------------
// Replace Edit (Sequence > Replace With Clip; Shift-drag from the Source Monitor, Alt-drag from the Project panel)

/// What a Replace Edit puts in a clip's place: an item and the source time shown at the clip's first frame (the Source
/// Monitor's In point; for "Match Frame", the source frame at the Source Monitor's playhead lined up with the sequence
/// playhead: replaceMatchFrameIn()).
struct ReplaceSource {
    MediaId media;
    Ticks sourceIn;
};

/// Replace Edit: each of `clips` (with its linked partners under FollowLinks) plays `source` instead - same place,
/// length, speed, direction, effects and attributes; its name becomes the item's. Partners keep their sync with the
/// clip (their source times move by the same amount). A video clip takes the item's video stream (its own index when
/// the item has it, else the first), an audio clip the audio stream with its index (else the first); a clip of a kind
/// the item lacks is left as it is. Errors: NotFound, InvalidArgument (no clip the item can play; a time-remapped or
/// frozen clip; the item's media does not cover what the clip shows - Premiere's insufficient media).
EditOutcome replaceClips(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                         const ReplaceSource& source, LinkMode links = LinkMode::FollowLinks);

/// "From Source Monitor, Match Frame": the source In that puts `sourcePlayhead` (the frame the Source Monitor shows) at
/// `sequencePlayhead` in `clip` (whose start it is relative to), at the clip's speed and direction.
std::optional<Ticks> replaceMatchFrameIn(const Clip& clip, Ticks sourcePlayhead, Ticks sequencePlayhead);

// ---------------------------------------------------------------------------------------------------
// Replace Footage, Make Offline (the Project panel)

/// Replace Footage: `media` plays `replacement`'s file from now on (its locator and probed info; editor::
/// describeMediaFile makes it). Every clip of the item keeps its place, source times, effects and markers; the item's
/// marks, markers, label and settings stay; the old file's proxy and audio takes are dropped. `rename` gives the item
/// the new file's name (Premiere's "Rename Clip to File Name"). Errors: NotFound, InvalidArgument (a generated item; a
/// file without a stream a clip plays; a clip that would play past the new file's end).
std::expected<ProjectState, Error> replaceFootage(const ProjectState& state, MediaId media, const MediaItem& replacement,
                                                  bool rename = true);

/// Make Offline (media files remain on disk): the items are offline (their clips show Media Offline) until they are
/// linked again (MediaItem::madeOffline). Generated items and unknown ids are errors.
std::expected<ProjectState, Error> makeOffline(const ProjectState& state, std::span<const MediaId> media);

// ---------------------------------------------------------------------------------------------------
// Make Subsequence (Sequence > Make Subsequence, Shift+U)

/// A new sequence with the parent's settings and tracks (names and roles), holding copies (new ids) of `clips` and their
/// linked partners on the same tracks, the earliest at 0, with the transitions between copied clips, the links among
/// them, and the caption cues and markers over their span. With no clips: everything on the targeted tracks between
/// the sequence's In and Out (trimmed to them). The parent is unchanged. Named "<sequence> Sub_01" (02, ...) unless
/// `name` is given; in the parent's bin. Errors: NotFound, InvalidArgument (nothing to copy; no In/Out).
std::expected<Created<SequenceId>, Error> makeSubsequence(const ProjectState& state, SequenceId sequence,
                                                          std::span<const ClipId> clips, std::string name = {});

// ---------------------------------------------------------------------------------------------------
// Through edits (Premiere draws them with a dotted edit line)

/// Whether `right` continues `left` without a visible change: adjacent on one track, the same source (media or sequence,
/// stream, angles), speed, direction, enabled state and content (name, parameters, effects, blend, time remap), the
/// right one's source starting where the left one's ends (a frame hold: the same frame), no transition at the cut.
bool isThroughEdit(const Clip& left, const Clip& right);

/// Join Through Edits: removes the through edits at the edges of `clips` (and of their linked partners under
/// FollowLinks), each pair becoming one clip; with no clips, every through edit of the sequence's unlocked tracks.
/// Errors: NotFound, InvalidArgument (no through edit there).
EditOutcome joinThroughEdits(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                             LinkMode links = LinkMode::FollowLinks);

// ---------------------------------------------------------------------------------------------------
// Gaps (Sequence > Go to Gap, Close Gap)

/// Go to Gap > Next / Previous in Sequence (`tracks` empty): the start of the next (previous) gap - a time no track has
/// a clip in, before the sequence's last clip - after (before) `t`; > Next / Previous in Track: the nearest gap start on
/// any of `tracks` (a gap between clips, or before the first). nullopt when there is none.
std::optional<Ticks> nextGap(const Sequence& sequence, Ticks t, std::span<const TrackId> tracks = {});
std::optional<Ticks> previousGap(const Sequence& sequence, Ticks t, std::span<const TrackId> tracks = {});

/// Close Gap: removes the empty time on the tracks that hold `clips` (every unlocked track with no clips) - between the
/// selected clips, or with none from the sequence's start to its last clip - where none of those tracks has material, so
/// linked clips stay in sync; other unlocked sync-locked tracks lose the same time, and a gap where one of them has
/// material is left alone. Caption cues follow. Errors: NotFound, InvalidArgument (no gap to close).
EditOutcome closeGaps(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips);

// ---------------------------------------------------------------------------------------------------
// Frame Hold Options (Clip > Video Options)

/// The frame Frame Hold Options holds on.
enum class HoldOn : std::uint8_t {
    SourceTimecode,   ///< `time` is a source time (media time)
    SequenceTimecode, ///< `time` is a sequence time inside the clip
    InPoint,          ///< the clip's first frame
    OutPoint,         ///< the clip's last frame
    Playhead,         ///< `time` is the playhead (inside the clip)
};
std::string_view toString(HoldOn on);
std::optional<HoldOn> holdOnFromString(std::string_view text);

struct FrameHoldOptions {
    /// Off: the clip plays normally again from the frame it held (at 100 %).
    bool enabled = true;
    HoldOn holdOn = HoldOn::InPoint;
    Ticks time;
    /// Hold Filters: the clip's keyframed effects hold at the held frame too (ClipContent::holdFilters).
    bool holdFilters = false;
};

/// Frame Hold Options on a video clip: the whole clip holds the chosen frame (a freeze, keeping its place and length),
/// with or without Hold Filters; `enabled` false undoes a hold. Errors: NotFound, InvalidArgument (an audio, nested,
/// generated or time-remapped clip, a time outside the clip or its media).
EditOutcome setFrameHoldOptions(const ProjectState& state, SequenceId sequence, ClipId clip,
                                const FrameHoldOptions& options);

// ---------------------------------------------------------------------------------------------------
// Merge Clips (Clip > Merge Clips)

/// What Merge Clips builds: one video clip and separately recorded audio, synced.
struct MergeSpec {
    std::string name;            ///< default: "<video item> - Merged"
    BinId bin;                   ///< default: the video item's bin
    MediaId video;               ///< the camera clip (an item with video), or invalid for audio-only merges
    std::vector<MediaId> audio;  ///< the separately recorded sound (items with audio), at least one
    MulticamSync sync = MulticamSync::InPoints; ///< In points, Out points, timecode, clip markers or audio
    bool removeCameraAudio = false;             ///< Premiere's "Remove Audio from AV Clip"
    const AudioSyncAnalyzer* audioSync = nullptr;
    AnalysisControl audioSyncControl;
};

/// Merge Clips: a merged clip - a synced source sequence (MulticamInfo::merged) whose angle 0 is the camera (its video,
/// and its audio unless removeCameraAudio) and the others the audio recordings, placed by `sync` as Create Multi-Camera
/// Source Sequence places angles. Edited into a sequence, its clips show the camera and play every audio angle
/// (multicam clips: they trim, ripple and undo like any clip; Flatten turns them into the items' own linked clips).
/// Errors: as createMulticam, plus InvalidArgument (no audio, an audio item without sound, the video item without
/// video).
std::expected<Created<SequenceId>, Error> mergeClips(const ProjectState& state, const MergeSpec& spec);

// ---------------------------------------------------------------------------------------------------
// Interpret Footage, Modify > Timecode (Clip > Modify)

/// The speed a clip of `item` plays at so every frame shows at the interpreted rate: interpreted / native (a 120 fps
/// capture interpreted as 30 fps: 1/4, slow motion with every frame). 1/1 without an interpreted rate or a known native
/// rate.
Speed conformSpeed(const MediaItem& item);

/// The time a clip shows from: its item's start timecode (Modify > Timecode, else the file's), nullopt when it has none.
std::optional<Ticks> mediaStartTimecode(const MediaItem& item);

/// Interpret Footage on items: their interpretation becomes `interpretation` (an empty one clears it). When the
/// assumed frame rate changes, every clip of the items (video and audio, in every sequence; not frame holds or
/// time-remapped clips) keeps its source In and material and plays at its speed times the change, so a 120 fps capture
/// interpreted as 30 fps plays every frame at a quarter speed and lasts four times as long (each clip growing into the
/// gap after it, or clamped at the next clip, like Speed/Duration without ripple); new clips get conformSpeed().
/// Errors: NotFound, InvalidArgument (a value out of range, a generated item).
EditOutcome interpretFootage(const ProjectState& state, std::span<const MediaId> media,
                             const MediaInterpretation& interpretation);

/// Modify > Timecode: the items' start timecode (`start` nullopt = the file's) and reel name. Errors: NotFound,
/// InvalidArgument (a start outside 0 to 24 hours, a reel name over MediaTimecode::kMaxReelNameBytes).
std::expected<ProjectState, Error> setMediaTimecode(const ProjectState& state, std::span<const MediaId> media,
                                                    std::optional<Ticks> start, std::string reelName);

} // namespace wabcut::edit
