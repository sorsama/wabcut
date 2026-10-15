#pragma once

// Project-level operations: bins, media, sequences, tracks and clip attributes. Same rules as
// EditOps.h: pure functions from an old state to a new one, std::expected for failures.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

namespace wabcut::edit {

/// Result of an operation that creates one object.
template <class IdT>
struct Created {
    ProjectState state;
    IdT id;
};

std::expected<Created<BinId>, Error> addBin(const ProjectState& state, std::string name, BinId parent = {});

/// Adds a media item; `item.id` is ignored and a new id is assigned.
std::expected<Created<MediaId>, Error> addMedia(const ProjectState& state, MediaItem item);
/// Removes a media item. Fails while any clip still uses it.
std::expected<ProjectState, Error> removeMedia(const ProjectState& state, MediaId media);

/// Records an offline audio take of a media item (A.4, AudioTake), replacing its take of the same stream and kind.
/// Errors: NotFound (no such media), InvalidArgument (a generated item, a stream it does not have, or a take the
/// validation refuses).
std::expected<ProjectState, Error> setAudioTake(const ProjectState& state, MediaId media, AudioTake take);
/// Forgets the take of `stream` made by `kind` (the source plays again). Nothing to forget is no change.
std::expected<ProjectState, Error> removeAudioTake(const ProjectState& state, MediaId media, int stream, std::string_view kind);

/// Adds an asset (a LUT file): `asset.id` is ignored and a new id assigned. Needs a non-empty kind and name and a
/// locator path.
std::expected<Created<AssetId>, Error> addAsset(const ProjectState& state, ProjectAsset asset);
/// Points an asset at another file (relink), or records a check's fingerprint / offline state.
std::expected<ProjectState, Error> relinkAsset(const ProjectState& state, AssetId asset, MediaLocator locator);
/// Removes an asset. Refuses while an effect refers to it ("asset:<id>") unless `clearReferences`: those data entries
/// are then removed in the same edit.
std::expected<ProjectState, Error> removeAsset(const ProjectState& state, AssetId asset, bool clearReferences = false);

// ---- Masks and redactions (I.6, Masks.h) ----
/// Adds `mask` (its id is assigned) to an effect instance of a clip. `effect` invalid: the clip's opacity mask
/// effect ("wabcut.mask", appended to the stack when missing): its masks cut the clip out (alpha).
std::expected<Created<std::uint32_t>, Error> addMask(const ProjectState& state, SequenceId sequence, ClipId clip,
                                                     EffectInstanceId effect, Mask mask);
/// Replaces the mask with `mask.id` (shape, mode, params with their keyframes, path keys).
std::expected<ProjectState, Error> updateMask(const ProjectState& state, SequenceId sequence, ClipId clip,
                                              EffectInstanceId effect, Mask mask);
std::expected<ProjectState, Error> removeMask(const ProjectState& state, SequenceId sequence, ClipId clip,
                                              EffectInstanceId effect, std::uint32_t mask);
/// Adds a redaction: `redaction.id` is ignored and a new id assigned; its media must exist.
std::expected<Created<RedactionId>, Error> addRedaction(const ProjectState& state, Redaction redaction);
/// Replaces the redaction with `redaction.id`.
std::expected<ProjectState, Error> updateRedaction(const ProjectState& state, Redaction redaction);
std::expected<ProjectState, Error> removeRedaction(const ProjectState& state, RedactionId redaction);
/// "Don't redact here" (`redact` false) for one clip, or back on.
std::expected<ProjectState, Error> setRedactionOverride(const ProjectState& state, RedactionId redaction, ClipId clip,
                                                        bool redact);

struct SequenceSettings {
    std::string name = "Sequence 01";
    Rate frameRate = kRate30;
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    Rate audioRate = kRate48kHz;
    bool dropFrame = false;
    std::int32_t videoTracks = 3;
    std::int32_t audioTracks = 3;
    BinId bin;
};

/// Adds a sequence with the given empty tracks; V1 and A1 start targeted (Premiere's default).
std::expected<Created<SequenceId>, Error> addSequence(const ProjectState& state, const SequenceSettings& settings);
std::expected<ProjectState, Error> removeSequence(const ProjectState& state, SequenceId sequence);

/// Sets the explicit sequence duration. It may not end before the content (InvalidArgument).
EditOutcome setSequenceDuration(const ProjectState& state, SequenceId sequence, Ticks duration);
/// X.1: the timecode of the sequence's first frame (0 to kMaxStartTimecode, whole frames not required). Errors:
/// NotFound, InvalidArgument (out of range).
EditOutcome setSequenceStartTimecode(const ProjectState& state, SequenceId sequence, Ticks start);
/// Shrinks (or grows) the explicit duration to the end of the last clip, dropping a trailing gap.
EditOutcome fitSequenceDurationToContent(const ProjectState& state, SequenceId sequence);
/// Sets or clears the work area; it must be non-empty and lie within [0, duration].
EditOutcome setWorkArea(const ProjectState& state, SequenceId sequence, std::optional<TimeRange> workArea);

/// Adds a track above the top video track (Vn+1) or below the last audio track (An+1).
std::expected<Created<TrackId>, Error> addTrack(const ProjectState& state, SequenceId sequence, TrackKind kind,
                                                std::string name = {});
/// Deletes a track and its clips (unlinking their partners). The track must be unlocked.
EditOutcome removeTrack(const ProjectState& state, SequenceId sequence, TrackId track);

/// Fields to change on a track; unset fields keep their value.
struct TrackStateChange {
    std::optional<std::string> name;
    std::optional<bool> enabled;
    std::optional<bool> locked;
    std::optional<bool> syncLocked;
    std::optional<bool> muted;
    std::optional<bool> solo;
    std::optional<bool> targeted;
    /// Audio tracks only (a role on a video track is an error).
    std::optional<TrackRole> role;
    /// A.8, audio tracks only (arming a video track is an error).
    std::optional<bool> recordArmed;
    /// A.9, audio tracks only: the channel format (mono, stereo, 5.1, adaptive) and the 5.1 panner (x, y in -1..1, the
    /// LFE send in -144..0 dB).
    std::optional<ChannelFormat> format;
    std::optional<SurroundPan> surround;
};

/// Track header toggles (lock, sync lock, target, mute, solo, output, record-arm), rename and audio role. Allowed on locked
/// tracks (that is how they get unlocked). Unlocking dissolves link groups that only a clip of this
/// track still belongs to (kept while it was locked).
EditOutcome setTrackState(const ProjectState& state, SequenceId sequence, TrackId track,
                          const TrackStateChange& change);

/// Fields to change on clips; unset fields keep their value.
struct ClipAttributeChange {
    std::optional<std::string> name;
    std::optional<std::int32_t> colorIndex;
    std::optional<bool> enabled;
    std::optional<ClipProperties> properties;
    /// C.15: the blend mode (video clips only: an audio clip in the list is an error).
    std::optional<BlendMode> blendMode;
};

/// Rename, label colour, enable/disable (Shift+E), effect properties and (C.15) the blend mode. Clips on locked tracks
/// are an error. Property values are validated (finite; opacity in [0,1]; pan in [-1,1]; scale >= 0).
EditOutcome setClipAttributes(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                              const ClipAttributeChange& change);

// ---- Bins and library metadata (M5k) ----

/// Renames a bin. The name may not be empty (InvalidArgument).
std::expected<ProjectState, Error> renameBin(const ProjectState& state, BinId bin, std::string name);
/// Moves bins, media items and sequences into `target` (invalid = the root bin). A bin cannot move into itself or
/// one of its sub-bins (InvalidArgument); unknown ids are NotFound. Items already in `target` stay; moving nothing
/// returns the state unchanged.
std::expected<ProjectState, Error> moveToBin(const ProjectState& state, BinId target, std::span<const BinId> bins,
                                             std::span<const MediaId> media, std::span<const SequenceId> sequences);
/// Deletes bins with everything inside them (sub-bins, media items, sequences) and the review notes on those
/// sequences. Refuses (InvalidArgument, naming the item and the sequence) while a media item or a sequence inside is
/// used (played, nested or multicam) by a sequence that is not deleted with it.
std::expected<ProjectState, Error> removeBins(const ProjectState& state, std::span<const BinId> bins);

/// Library metadata of media items (M5k); unset fields keep their value.
struct MediaAttributeChange {
    /// Not empty. New clips take it; existing clips keep their names (Premiere). Refreshes clipDefaults.
    std::optional<std::string> name;
    std::optional<std::int32_t> colorIndex; ///< 0..15
    std::optional<std::string> comment;     ///< at most MediaItem::kMaxTextBytes
    std::optional<std::string> description; ///< at most MediaItem::kMaxTextBytes
    /// Poster frame, media time inside the media; an inner nullopt clears it.
    std::optional<std::optional<Ticks>> posterFrame;
};
/// One undo step for any number of items (rename one, label many). NotFound for unknown ids.
std::expected<ProjectState, Error> setMediaAttributes(const ProjectState& state, std::span<const MediaId> media,
                                                      const MediaAttributeChange& change);

/// Make Subclip: a new item in `bin` named `name` that plays `range` (media time, non-empty, inside the media's
/// sourceBounds) of `master`'s file. Copies the master's locator, info and label; sets its markIn/markOut to `range`.
/// A subclip of a subclip refers to the same file (master = the item it came from). Refuses generator items (titles)
/// and stills (InvalidArgument).
std::expected<Created<MediaId>, Error> makeSubclip(const ProjectState& state, MediaId master, TimeRange range,
                                                   std::string name, bool restricted = true, BinId bin = {});
/// Edits a subclip's range and restriction (Premiere's Edit Subclip). Restricting refuses (InvalidArgument) while a
/// clip of it uses media outside the new range. Marks outside the new range are clamped into it.
std::expected<ProjectState, Error> setSubclip(const ProjectState& state, MediaId subclip, TimeRange range,
                                              bool restricted);

} // namespace wabcut::edit
