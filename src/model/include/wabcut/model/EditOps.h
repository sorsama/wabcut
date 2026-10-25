#pragma once

// Timeline edit operations with Premiere Pro semantics.
//
// Every operation is a pure function: (old ProjectState, arguments) -> new ProjectState + result, or an
// Error and no change. Nothing is modified in place, there are no globals, and the result only depends
// on the inputs. The new state shares every track the edit did not touch with the old one.
//
// Rules shared by all operations (the invariants validate() checks are always kept):
//  * Locked tracks are never changed. Naming a clip on a locked track is an error; linked partners and
//    targeted/sync-locked tracks that are locked are skipped.
//  * Linked clips (same LinkGroupId) edit together when LinkMode::FollowLinks (Premiere's "linked
//    selection"). Partners on locked tracks are left alone.
//  * Ripple edits (insert, extract, ripple delete, ripple trim, Q/W) shift the edited tracks and every
//    other unlocked sync-locked track. On those other tracks the edit cuts through clips so later
//    material stays in sync: added time splits a clip spanning the edit point and opens a gap; removed
//    time removes the same range there too.
//  * Trimming or moving a clip keeps its id. Splitting a clip keeps its id on the left piece; the right
//    piece gets a new id and shares the clip's content (name, properties). Right pieces cut together
//    from one link group are linked to each other; a link group left with one clip unlinks it, except
//    on a locked track: that clip stays exactly as it was (still in its group, as Premiere leaves it) and
//    the group dissolves when the track is unlocked (setTrackState).
//  * A clip shrunk to zero length by an edit is removed. Trims never go below one frame of the
//    sequence rate and clamp to the available source media and to neighbouring clips; the delta that was
//    actually applied is reported in EditResult::appliedDelta.
//  * Positions are plain ticks; snapping to frames is the caller's choice (see Snap.h for snap targets).
//  * A sequence's explicit duration grows when content passes it and never shrinks on its own, so
//    ripple removals leave a trailing gap (ProjectOps.h: setSequenceDuration / fitSequenceDurationToContent).
//  * Transitions the edit invalidates (a clip removed, edit point moved apart, handles gone) are dropped;
//    a transition at a clip's Out point follows the right-hand piece when the clip is split.

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::edit {

enum class LinkMode {
    /// Linked partners take part in the edit (Premiere's default "linked selection").
    FollowLinks,
    /// Only the named clips take part (Alt-click in Premiere).
    IgnoreLinks,
};

/// A clip edge: its In point (head) or Out point (tail).
enum class Edge { Head, Tail };

struct EditResult {
    ProjectState state;
    /// What changed (one SequenceChange for the edited sequence; empty for a no-op).
    ChangeSet changes;
    /// Clips an insert/overwrite placed from the source, in patch order.
    std::vector<ClipId> placed;
    /// Every clip that exists in `state` but did not exist before (placed clips and split pieces).
    std::vector<ClipId> created;
    /// Every clip that existed before and no longer exists.
    std::vector<ClipId> removed;
    /// Where Premiere leaves the playhead after this edit, when the edit moves it.
    std::optional<Ticks> playhead;
    /// For trim/slip/slide/move: the delta actually applied after clamping.
    Ticks appliedDelta;
    /// Transitions the edit created (FinishingOps.h).
    std::vector<TransitionId> createdTransitions;
    /// Effect instances the edit created (FinishingOps.h).
    std::vector<EffectInstanceId> createdEffects;
    /// The caption track setCaptionTrack created.
    CaptionTrackId createdCaptionTrack;
    /// The media item the edit created (addTitle's generator item, TitleOps.h).
    MediaId createdMedia;
    /// The graphic layer the edit added (GraphicOps.h).
    std::uint32_t createdLayer = 0;
    /// Tracks the edit added (A.10: source edits whose patches ask for a new track), in patch order.
    std::vector<TrackId> createdTracks;
};

using EditOutcome = std::expected<EditResult, Error>;

// ---------------------------------------------------------------------------------------------------
// Source edits: insert (",") and overwrite (".")

/// Source patching: a source stream (Premiere's source track indicator) and the timeline track it is
/// patched to. Video streams go to video tracks, audio streams to audio tracks.
struct SourcePatch {
    StreamRef source;
    TrackId destination;
    /// A.10 (multi-track ingest), audio streams only: the name of the track the stream goes to. Without a
    /// `destination` the edit adds an audio track with this name below the last audio track and places the stream
    /// there (reported in EditResult::createdTracks); a destination that has no name yet is given it. A track the edit
    /// adds or names this way takes the role the name implies (sourceTrackName: Mic and Discord dialogue, Music music,
    /// Game and Alerts effects) when it has none. Empty: the destination is used as it is.
    std::string newTrack;
};

/// What to edit into the sequence: source media, the source range to use and the enabled patches.
/// Several patches produce one clip each, linked together.
struct SourceClip {
    MediaId media;
    TimeRange sourceRange;
    std::vector<SourcePatch> patches;
    /// C.8: edit a sequence instead of media (Premiere's sequence dragged into another one: a nested clip; a multicam
    /// clip when it is a multicam source). `media` is ignored; the source range is in that sequence's time; its
    /// streams are Video 0 (the picture) and Audio 0 (the mix). A sequence never goes into itself (Nesting.h).
    SequenceId sequence;
    /// A multicam source's active angles: the picture's (one) and the sound's. Empty: the first angle with each.
    AngleSet videoAngle;
    AngleSet audioAngles;
};

/// Premiere's default patching: the media's first video stream to V1 and its audio streams to A1, A2, …
/// (as far as the sequence has tracks). A.10: the audio of a multi-track recording (routesAudioByName) is routed by
/// name instead (audioPatchesByName from A1), which may name free tracks and ask for new ones.
std::vector<SourcePatch> defaultSourcePatches(const Sequence& sequence, const MediaItem& media);
/// The same for a sequence used as a source (C.8): its picture to V1 and its mix to A1, as far as both have them.
std::vector<SourcePatch> defaultSourcePatches(const Sequence& sequence, const Sequence& source);

// ---------------------------------------------------------------------------------------------------
// Multi-track recordings (A.10): OBS, ShadowPlay and Game Bar files with several audio streams. Every stream becomes
// its own named track; the same source of the next recording (its "Mic", say) lands on the same track, so muting or
// soloing that track header mutes or solos every recording's mic at once.

/// What a recording's audio stream is called on the timeline.
struct SourceTrackName {
    /// Where the name comes from.
    enum class Origin : std::uint8_t {
        None,       ///< a file's single untitled stream: no name (patched as before)
        Title,      ///< the stream's title as written
        Recognised, ///< a title recognised as a common source: Game, Mic, Discord, Music, Alerts, Track N
        Inferred,   ///< an untitled stream of a file with several audio streams
    };

    std::string name;
    /// The role a track the edit adds or names for the stream gets (SourcePatch::newTrack).
    TrackRole role = TrackRole::None;
    Origin origin = Origin::None;

    bool operator==(const SourceTrackName&) const = default;
};

/// Longest track name sourceTrackName makes from a title, in bytes (cut on a UTF-8 character boundary).
inline constexpr std::size_t kMaxSourceTrackNameBytes = 32;

/// The timeline name of a stream title (AudioStreamInfo::title). Common sources are recognised ignoring case and
/// everything but letters and digits, and get their canonical name:
///  * "game", "game audio", "gameplay", "desktop", "desktop audio", "system", "system audio", "app audio",
///    "application audio" -> Game (effects);
///  * "mic", "microphone", "mic/aux", "mic aux", "mic audio", "voice", "commentary" -> Mic (dialogue);
///  * voice chat: "discord", "voice chat", "chat", "party chat", "teamspeak", "ts3" -> Discord (dialogue);
///  * "music", "spotify", "bgm" -> Music (music);  "alerts", "stream alerts" -> Alerts (effects);
///  * OBS's track names "Track1".."Track6" (or "Track 1") -> "Track N" (none).
/// A number after a recognised name stays, so two mics stay apart: "Mic/Aux 2" -> "Mic 2". Any other title is used as
/// written (trimmed, at most kMaxSourceTrackNameBytes); an empty title gives no name (Origin::None).
SourceTrackName sourceTrackName(std::string_view title);

/// The names of `media`'s audio streams, one per MediaInfo::audio entry: a titled stream by sourceTrackName. Untitled
/// streams of a file with two or more audio streams are named by position: the two of a recording with video (both
/// untitled: ShadowPlay's "separate both tracks", Xbox Game Bar with the microphone) are Game and Mic, otherwise
/// stream k is "Track k+1". A file's single untitled stream has no name.
std::vector<SourceTrackName> sourceTrackNames(const MediaInfo& media);

/// Whether `media`'s audio is routed by name: it has two or more audio streams, or one whose title is recognised
/// (Origin::Recognised; a single stream with another title, such as a camera's handler name, is patched as before).
bool routesAudioByName(const MediaInfo& media);

/// Where `media`'s audio streams go when routed by name (sourceTrackNames), each stream to:
///  1. the first unlocked audio track with its name (ignoring case), so a recording's Mic joins the Mic track;
///  2. an untitled stream k (Origin::Inferred): its place in Premiere's default patching - the k-th unlocked audio
///     track from `firstTrack` - when that track has no name (it is named when it is also empty), so untitled files
///     land where they always did;
///  3. the next free audio track from `firstTrack` on: unlocked, no name, no clips (named so);
///  4. else a new track with its name (`destination` invalid, SourcePatch::newTrack set).
/// Two streams never share a track and locked tracks are never destinations. A stream without a name (only when
/// routesAudioByName is false) is placed like an untitled one but never named and never given a new track: it is left
/// out when no track takes it. Patches are in stream order.
std::vector<SourcePatch> audioPatchesByName(const Sequence& sequence, const MediaInfo& media,
                                            std::size_t firstTrack = 0);

/// Source monitor marks; out is exclusive.
struct SourceMarks {
    std::optional<Ticks> in;
    std::optional<Ticks> out;
};

/// Duration used for unbounded sources (still images) without a source Out mark.
inline constexpr Ticks kDefaultStillDuration = Ticks::fromSeconds(5);

struct ThreePointEdit {
    Ticks at;
    TimeRange sourceRange;
};

/// Premiere three-point editing: resolves where a source edit lands and which source range it uses
/// from the source marks, the sequence In/Out marks and the playhead.
///  * sequence In and Out set: the edit fills [In, Out); the source starts at its In (or ends at its Out
///    when only a source Out is set). With source In and Out also set, the source Out is ignored.
///  * only sequence In: the edit starts there.  * only sequence Out: the edit ends there (back-timed).
///  * no sequence marks: the edit starts at the playhead.
/// Source In defaults to 0 and source Out to the end of the media (the shortest patched stream).
std::expected<ThreePointEdit, Error> resolveThreePointEdit(const ProjectState& state, SequenceId sequence,
                                                           MediaId media, std::span<const SourcePatch> patches,
                                                           SourceMarks marks, Ticks playhead);

/// How far source marks can reach in `media`: its longest stream, or nullopt when unbounded (still images) or
/// unknown.
std::optional<Ticks> sourceMarkExtent(const MediaItem& media);
/// The Source Monitor marks stored on `media` (MediaItem::markIn/markOut), clamped to sourceMarkExtent(): a mark
/// past the end (relinked to a shorter file) moves to the end, and an In that no longer comes before the Out is
/// dropped. `clamped` (optional) says whether anything changed.
SourceMarks storedSourceMarks(const MediaItem& media, bool* clamped = nullptr);
/// Sets (nullopt clears) the Source Monitor marks stored on a media item. No sequence changes. Errors: NotFound
/// for an unknown item; InvalidArgument for In < 0, Out <= In, or a mark past the media's end.
EditOutcome setSourceMarks(const ProjectState& state, MediaId media, SourceMarks marks);

/// Insert edit: opens room at `at` on the destination tracks and on every unlocked sync-locked track
/// (splitting clips that span `at`), then places the source there. Playhead -> end of the new clips.
EditOutcome insertEdit(const ProjectState& state, SequenceId sequence, const SourceClip& source, Ticks at);

/// Overwrite edit: replaces whatever is in [at, at + duration) on the destination tracks. Nothing
/// moves. Playhead -> end of the new clips.
EditOutcome overwriteEdit(const ProjectState& state, SequenceId sequence, const SourceClip& source, Ticks at);

/// One clip for placeClips: `duration` of media `media`'s stream `stream` from `sourceIn`, at `start` on `track`.
struct ClipPlacement {
    TrackId track;
    MediaId media;
    StreamRef stream;
    Ticks start;
    Ticks duration;
    Ticks sourceIn;
};

/// Bulk build for importers (X.1): places many media clips in one edit, each into free time on its track (nothing is
/// overwritten, nothing moves, nothing is linked), with the media's default content and label as overwriteEdit gives
/// them. One edit instead of one per clip: O(n log n) for n clips where single edits cost O(n) each. Errors (nothing is
/// placed then): NotFound (track, media), InvalidArgument (a locked track or one of the other kind, a stream the media
/// lacks, an empty range, a start before 0, a source range outside the media, a clip that overlaps a clip of its track
/// or another placement). EditResult::placed lists the new clips in the order given.
EditOutcome placeClips(const ProjectState& state, SequenceId sequence, std::span<const ClipPlacement> clips);

// ---------------------------------------------------------------------------------------------------
// Lift (";") and extract ("'") of the sequence In/Out range on the targeted tracks

/// Removes [In, Out) from the targeted tracks, leaving a gap. Clears the marks; playhead -> In.
EditOutcome lift(const ProjectState& state, SequenceId sequence);
/// Removes [In, Out) from the targeted tracks and closes the gap (sync-locked tracks follow).
/// Clears the marks; playhead -> In.
EditOutcome extract(const ProjectState& state, SequenceId sequence);

// ---------------------------------------------------------------------------------------------------
// Razor / add edit

/// Splits the clip under `at` on each of `tracks` (Ctrl+K on the targeted tracks, or the razor tool on
/// one track). With FollowLinks, linked partners under `at` are cut too. Positions on a clip boundary
/// or in a gap are left alone; `created` lists the new right-hand pieces.
EditOutcome addEdit(const ProjectState& state, SequenceId sequence, Ticks at, std::span<const TrackId> tracks,
                    LinkMode links = LinkMode::IgnoreLinks);
/// Add Edit to All Tracks (Ctrl+Shift+K): every unlocked track.
EditOutcome addEditToAllTracks(const ProjectState& state, SequenceId sequence, Ticks at);

// ---------------------------------------------------------------------------------------------------
// Delete

/// Delete (clear): removes the clips and leaves gaps.
EditOutcome deleteClips(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                        LinkMode links = LinkMode::FollowLinks);

/// Ripple delete of clips: removes them and closes the time they occupied. The removed time is the
/// union of the clips' ranges; it is also removed from every other unlocked sync-locked track. Fails if
/// that would remove an unselected clip on a track that holds a selected one.
EditOutcome rippleDeleteClips(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                              LinkMode links = LinkMode::FollowLinks);

/// Ripple delete of the gap at `at` on `track` (select gap + Delete). Fails when `at` is not in a gap
/// followed by a clip, or when another unlocked sync-locked track has material in that time (Premiere
/// refuses the same way).
EditOutcome rippleDeleteGap(const ProjectState& state, SequenceId sequence, TrackId track, Ticks at);

// ---------------------------------------------------------------------------------------------------
// Trims

/// Q: ripple-trims the previous edit to the playhead on the targeted tracks. The previous edit is the
/// latest start of the clip (or gap) under the playhead across the targeted tracks; [edit, playhead) is
/// removed and closed. Playhead -> the edit point. No-op when the playhead sits on that edit.
EditOutcome rippleTrimPreviousEditToPlayhead(const ProjectState& state, SequenceId sequence, Ticks playhead);
/// W: ripple-trims the next edit to the playhead: removes [playhead, next edit) on the targeted tracks,
/// where the next edit is the earliest end of the clip (or gap) under the playhead.
EditOutcome rippleTrimNextEditToPlayhead(const ProjectState& state, SequenceId sequence, Ticks playhead);

/// Regular trim (selection tool on an edge): moves the clip's head or tail by `delta` without moving
/// anything else; clamps to neighbours, source media and one frame minimum.
EditOutcome trimEdge(const ProjectState& state, SequenceId sequence, ClipId clip, Edge edge, Ticks delta,
                     LinkMode links = LinkMode::FollowLinks);

/// Ripple edit (B): moves the clip's head or tail by `delta` and ripples later material. A positive
/// delta moves the edge later (tail: longer, head: shorter). The clip's start stays fixed for a head
/// trim; everything after the clip shifts by the change in length. Clamps to source media and one frame.
EditOutcome rippleTrim(const ProjectState& state, SequenceId sequence, ClipId clip, Edge edge, Ticks delta,
                       LinkMode links = LinkMode::FollowLinks);

/// Rolling edit (N): moves the edit point at the clip's head or tail by `delta`, trimming the adjacent
/// clip the opposite way (or the gap, when there is no adjacent clip). Nothing else moves.
EditOutcome rollEdit(const ProjectState& state, SequenceId sequence, ClipId clip, Edge edge, Ticks delta,
                     LinkMode links = LinkMode::FollowLinks);

/// Slip (Y): shifts the source range of the clips by `sourceDelta` (positive = later source frames),
/// keeping their timeline position and length. Clamps to the available media.
EditOutcome slip(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips, Ticks sourceDelta,
                 LinkMode links = LinkMode::FollowLinks);

/// Slide (U): moves the clip by `delta` keeping its source range; the previous adjacent clip's tail and
/// the next adjacent clip's head are trimmed to compensate (adjacent gaps grow or shrink instead).
EditOutcome slide(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks delta,
                  LinkMode links = LinkMode::FollowLinks);

// ---------------------------------------------------------------------------------------------------
// Move

enum class MoveMode {
    /// Plain drag: the clips overwrite whatever is at the destination; their old place becomes a gap.
    Overwrite,
    /// Ctrl-drag: room is opened at the destination (destination tracks and sync-locked tracks shift);
    /// their old place becomes a gap.
    Insert,
    /// 0.4, Ctrl+Alt-drag (Premiere's rearrange edit): the clips' time is ripple-removed at their old place and opened
    /// at the drop (as Insert), ONLY on the unlocked tracks that hold a dragged clip and the destination tracks: other
    /// tracks (sync-locked or not), the captions and the sequence markers are untouched, so a music bed under rearranged
    /// shots stays whole. Refused like rippleDeleteClips when closing the old place would take an unselected clip off a
    /// track the selection is on. The drop time is measured on the timeline before the removal, so a clip dropped at the
    /// cut after its neighbour swaps with it. Dropping it back where it was changes nothing. Clips selected apart
    /// pack at the drop in time order (only their total length opens); a link group spread over separate stretches
    /// is refused.
    Rearrange,
};

/// Track change for a move, counted outward from the video/audio divider like Premiere draws it:
/// video +1 = one track up (V1 -> V2), audio +1 = one track down (A1 -> A2).
struct TrackShift {
    int video = 0;
    int audio = 0;
};

/// Moves the clips by `delta` in time and `tracks` across tracks. The delta is clamped so no clip starts
/// before 0. Destination tracks must exist and be unlocked.
EditOutcome moveClips(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips, Ticks delta,
                      TrackShift tracks = {}, MoveMode mode = MoveMode::Overwrite,
                      LinkMode links = LinkMode::FollowLinks);

/// 0.4, Alt-drag (Premiere): copies the clips (with their linked partners unless IgnoreLinks) to where moveClips would
/// move them, leaving the originals in place. Copies get new clip ids, one new link group per copied link group, one new
/// clip group per copied group with 2+ copies (else none), new effect instance ids (parameters, keyframes, time remap,
/// speed, label, enabled state kept) and copies of the transitions between two copied clips. Overwrite replaces what lies
/// under each copy (an original included); Insert opens room first (destination and unlocked sync-locked tracks,
/// captions). EditResult::created lists the copies in resolveClips order; appliedDelta is the clamped delta. No change
/// when the copy would land exactly on its original (delta 0, no track shift). Errors as moveClips (no such track, a
/// locked destination); InvalidArgument for MoveMode::Rearrange.
EditOutcome duplicateClips(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips, Ticks delta,
                           TrackShift tracks = {}, MoveMode mode = MoveMode::Overwrite,
                           LinkMode links = LinkMode::FollowLinks);

// ---------------------------------------------------------------------------------------------------
// Link / unlink

/// Links the clips (at least two) into a new link group, taking them out of their previous groups.
EditOutcome linkClips(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips);
/// linkClips for many groups in one edit (X.1, importers): each group (at least two clips of the sequence) becomes a
/// link group of its own. One pass over the sequence instead of one per group. Errors: NotFound (a clip), InvalidArgument
/// (a group of fewer than two clips, a clip in two groups, a clip on a locked track); nothing changes then.
EditOutcome linkClipGroups(const ProjectState& state, SequenceId sequence, std::span<const std::vector<ClipId>> groups);
/// Takes the clips out of their link groups (a group left with one clip disappears).
EditOutcome unlinkClips(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips);

// ---------------------------------------------------------------------------------------------------
// Marks ("I", "O", "X", "/", clear)

/// Sets the In mark at the start of the frame under `playhead`; clears an Out mark at or before it.
EditOutcome markIn(const ProjectState& state, SequenceId sequence, Ticks playhead);
/// Sets the Out mark at the end of the frame under `playhead`; clears an In mark at or after it.
EditOutcome markOut(const ProjectState& state, SequenceId sequence, Ticks playhead);
/// Mark Clip (X): In/Out around the clips under the playhead on the targeted tracks (all tracks when
/// none is targeted).
EditOutcome markClip(const ProjectState& state, SequenceId sequence, Ticks playhead);
/// Mark Selection ("/"): In/Out around the clips.
EditOutcome markSelection(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips);
EditOutcome clearMarks(const ProjectState& state, SequenceId sequence, bool clearIn = true, bool clearOut = true);

/// Adds a marker, keeping markers sorted by start.
EditOutcome addMarker(const ProjectState& state, SequenceId sequence, Marker marker);
/// Removes the marker at `index` (into Sequence::markers).
EditOutcome removeMarker(const ProjectState& state, SequenceId sequence, std::size_t index);
/// Replaces the marker at `index` (rename, comment, colour, type, duration or move), keeping markers sorted by start.
EditOutcome updateMarker(const ProjectState& state, SequenceId sequence, std::size_t index, Marker marker);
/// Replaces all of a sequence's markers (sorted by start here); an empty list clears them.
EditOutcome setSequenceMarkers(const ProjectState& state, SequenceId sequence, std::vector<Marker> markers);
/// Replaces a media item's clip markers (media time, sorted by start here); an empty list clears them.
EditOutcome setMediaMarkers(const ProjectState& state, MediaId media, std::vector<Marker> markers);

// ---------------------------------------------------------------------------------------------------
// Queries

/// Ids of the targeted tracks, V1..Vn then A1..An.
std::vector<TrackId> targetedTracks(const Sequence& sequence);
/// Ids of all tracks, V1..Vn then A1..An.
std::vector<TrackId> allTracks(const Sequence& sequence);
/// The clips plus every clip linked to one of them (input order first, then partners by track order).
std::vector<ClipId> withLinkedClips(const Sequence& sequence, std::span<const ClipId> clips);

} // namespace wabcut::edit
