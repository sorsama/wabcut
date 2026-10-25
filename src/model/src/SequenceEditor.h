#pragma once

// Private building blocks of the edit engine.
//
// SequenceEditor is a copy-on-write working copy of one sequence. Operations read the original tracks
// and clone a track the first time they change it; finish() fixes up link groups and produces the new
// ProjectState, sharing every untouched track with the input.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::edit::detail {

Error invalidArgument(std::string message);
Error notFound(std::string message);

std::expected<const Sequence*, Error> findSequence(const ProjectState& state, SequenceId sequence);

/// "V1", "A2", …
std::string trackLabel(const Sequence& sequence, TrackId track);

/// A clip taking part in an edit.
struct ClipRef {
    TrackId track;
    ClipId clip;
    std::size_t index = 0; // position on its track in the input sequence
};

/// Resolves clip ids (and, with FollowLinks, their partners) in one pass over the sequence.
/// Unknown ids and named clips on locked tracks are errors; partners on locked tracks are skipped.
/// Result order: track order (V1..Vn, A1..An), then time.
std::expected<std::vector<ClipRef>, Error> resolveClips(const Sequence& sequence, std::span<const ClipId> clips,
                                                        LinkMode links);

/// The clip and its linked partners for a single-edge operation (trim, roll, slide): at most one clip
/// per track, the named clip first. A partner is the group member on another unlocked track whose
/// matching edge is closest to the named clip's edge.
std::expected<std::vector<ClipRef>, Error> edgeParticipants(const Sequence& sequence, ClipId clip, Edge edge,
                                                            LinkMode links);

/// Closed interval of allowed deltas; starts unbounded. clamp() always allows 0 (no change).
struct DeltaBounds {
    std::int64_t lo = std::numeric_limits<std::int64_t>::min();
    std::int64_t hi = std::numeric_limits<std::int64_t>::max();

    void atLeast(Ticks v) { lo = std::max(lo, v.value); }
    void atMost(Ticks v) { hi = std::min(hi, v.value); }
    Ticks clamp(Ticks requested) const {
        const std::int64_t low = std::min<std::int64_t>(lo, 0);
        const std::int64_t high = std::max<std::int64_t>(hi, 0);
        return {std::clamp(requested.value, low, high)};
    }
};

class SequenceEditor {
public:
    SequenceEditor(const ProjectState& state, const Sequence& sequence);

    const ProjectState& project() const { return m_state; }
    /// The working revision (tracks reflect changes made so far).
    const Sequence& sequence() const { return m_seq; }
    /// Mutable sequence header (marks, markers, settings, track lists). Marks the sequence changed.
    Sequence& header() {
        m_headerChanged = true;
        return m_seq;
    }

    /// V1..Vn then A1..An.
    std::vector<TrackId> trackIds() const;
    const Track& track(TrackId id) const;
    /// Clones the track on first use. Callers enforce locking.
    Track& mutableTrack(TrackId id);
    /// Writable clip; copies only the clip's chunk (see ClipList).
    Clip& mutableClip(TrackId track, std::size_t index) { return mutableTrack(track).clips.mutableAt(index); }

    ClipId newClipId() { return m_ids.allocate<ClipId>(); }
    LinkGroupId newLinkGroup() { return m_ids.allocate<LinkGroupId>(); }
    ClipGroupId newClipGroup() { return m_ids.allocate<ClipGroupId>(); }
    TransitionId newTransitionId() {
        const auto id = m_ids.allocate<TransitionId>();
        m_createdTransitions.push_back(id);
        return id;
    }
    CaptionTrackId newCaptionTrackId() { return m_ids.allocate<CaptionTrackId>(); }
    EffectInstanceId newEffectId() {
        const auto id = m_ids.allocate<EffectInstanceId>();
        m_createdEffects.push_back(id);
        return id;
    }
    /// One frame at the sequence rate: the shortest clip a trim may leave.
    Ticks oneFrame() const { return framesToTicks(1, m_seq.frameRate); }
    /// The source media the clip may use (MediaItem::sourceBounds: the media, or a restricted subclip's range). The
    /// one helper every op that bounds a clip's source goes through.
    SourceBounds sourceBounds(const Clip& clip) const;
    /// Longest timeline duration the clip may have keeping its head where it is (direction-aware).
    std::optional<Ticks> maxDuration(const Clip& clip) const;
    /// Timeline ticks the clip's head may move earlier (direction-aware; nullopt = unbounded).
    std::optional<Ticks> headRoom(const Clip& clip) const;

    // --- Primitives. All keep the track sorted and overlap-free. -----------------------------------

    /// Splits the clip strictly containing `at`; returns the index of the new right piece.
    std::optional<std::size_t> splitAt(TrackId track, Ticks at);
    /// Removes everything inside `range`, trimming or splitting clips that cross its edges.
    void clearRange(TrackId track, TimeRange range);
    /// clearRange, then pulls everything after the range back by its duration.
    void rippleRemove(TrackId track, TimeRange range);
    /// Splits at `at` and pushes everything at or after it later by `duration`.
    void rippleInsert(TrackId track, Ticks at, Ticks duration);
    /// Moves every clip that starts at or after `from` by `delta` (caller guarantees no overlap).
    void shiftFrom(TrackId track, Ticks from, Ticks delta);
    /// Deletes a clip (reported in EditResult::removed).
    void removeClip(TrackId track, ClipId clip);

    // --- Captions and markers follow edits (C.7, Sequence.h): every caption track's cues, as a sync-locked track; ----
    // --- 0.4: the sequence's markers too while markersFollow() (Sequence::rippleMarkers, Ripple Sequence Markers). ---

    /// A ripple insert of `duration` at `at`: later cues move, a cue across `at` is cut in two (both keep the text).
    /// With `until` (a time remap inside one clip) only material before it moves; what is pushed past it is cut off.
    /// Markers (without `until`): those starting at or after `at` move by `duration`, a range marker across `at` grows.
    void captionsInsert(Ticks at, Ticks duration, std::optional<Ticks> until = std::nullopt);
    /// A ripple removal of `range`: the cues' time inside it goes (cues left without time go), later cues move back.
    /// With `until`, material from it on stays where it is. Markers (without `until`): later ones move back, one
    /// starting inside moves to the cut, a range marker loses the removed part (never deleted); peaks are clamped.
    void captionsRemove(TimeRange range, std::optional<Ticks> until = std::nullopt);
    /// The clip time `range` now lasting `duration` (a speed change of an audio clip): cues inside scale with it;
    /// with `ripple` later cues move by the difference, else the earlier of two cues that now meet is trimmed.
    /// Markers: with `ripple`, those after the clip move by the difference.
    void captionsStretch(TimeRange range, Ticks duration, bool ripple);
    /// 0.4: whether the sequence's markers follow the caption hooks above. Starts as the sequence's rippleMarkers;
    /// ops with their own option (dead-time removal, instant replay) set it before rippling.
    void setMarkersFollow(bool on) { m_markersFollow = on; }
    bool markersFollow() const { return m_markersFollow; }
    /// Deletes clips in one pass per track. `refs` hold input-sequence indices, grouped by track with
    /// ascending indices (resolveClips order), and must precede any other change to those tracks.
    void removeClips(std::span<const ClipRef> refs) { extract(refs, true); }
    /// Takes clips out to be placed again (moves); not reported as removed. Same rules as removeClips;
    /// returns the clips in `refs` order.
    std::vector<Clip> takeClips(std::span<const ClipRef> refs) { return extract(refs, false); }
    /// Puts a clip into a free range of the track.
    void placeClip(TrackId track, Clip clip);
    /// placeClip for a clip new to the project (reported in created; `fromSource` also in placed).
    void placeNewClip(TrackId track, Clip clip, bool fromSource);
    /// Deletes a track with its clips (reported as removed).
    void eraseTrack(TrackId track);
    /// Adds an empty track above the top video track or below the last audio track, with ProjectOps' addTrack defaults
    /// and `role` (audio tracks only). Reported in EditResult::createdTracks.
    TrackId addTrack(TrackKind kind, std::string name, TrackRole role = TrackRole::None);
    /// p02 audio: adds a submix track of `format` after the last one (Sequence::submixTracks). Reported in
    /// EditResult::createdTracks.
    TrackId addSubmixTrack(ChannelFormat format, std::string name);
    /// Marks a link group whose membership changed, so finish() can dissolve it if one clip is left.
    void touchGroup(LinkGroupId group) {
        if (group.isValid()) {
            m_touchedGroups.push_back(group);
        }
    }

    bool changed() const { return m_headerChanged || !m_cloned.empty(); }

    /// Relinks split pieces, dissolves single-clip link groups, drops transitions the edit invalidated,
    /// grows the sequence duration to its content and builds the result with its ChangeSet.
    EditResult finish(std::optional<Ticks> playhead = std::nullopt, Ticks appliedDelta = {});

private:
    struct SplitRecord {
        LinkGroupId group;
        Ticks cut;
        ClipId right;
    };

    TrackPtr& slot(TrackId id);
    const TrackPtr& slot(TrackId id) const;
    std::vector<Clip> extract(std::span<const ClipRef> refs, bool reportRemoved);
    void relinkSplitPieces(std::vector<ClipId>& removedSorted);
    /// Maps every cue's start and end through `map` (monotonic), drops cues left without time, trims the earlier of
    /// two cues that now overlap (unless the track allows overlaps), and stores the tracks if anything changed.
    template <class Map>
    void mapCaptions(Map&& map);
    /// 0.4: the sequence markers through a ripple insert / removal (Ripple Sequence Markers rules, see captionsInsert
    /// and captionsRemove); they stay sorted by start, and the header is touched only when a marker moved.
    void markersInsert(Ticks at, Ticks duration);
    void markersRemove(TimeRange range);
    /// Maps every marker's start (and highlight peak) through `map` and its end through `mapEnd`, keeping start <= end.
    template <class Map, class EndMap>
    void mapMarkers(Map&& map, EndMap&& mapEnd);
    void dissolveSingletonGroups();
    void pruneTransitions();

    const ProjectState& m_state;
    const Sequence& m_original;
    Sequence m_seq;
    IdGenerator m_ids;
    std::vector<std::pair<TrackId, Track*>> m_cloned;
    std::vector<SplitRecord> m_splits;
    std::vector<ClipId> m_created;
    std::vector<ClipId> m_placed;
    std::vector<ClipId> m_removed;
    std::vector<LinkGroupId> m_touchedGroups;
    std::vector<TransitionId> m_createdTransitions;
    std::vector<EffectInstanceId> m_createdEffects;
    std::vector<TrackId> m_createdTracks;
    bool m_headerChanged = false;
    bool m_markersFollow = true; ///< 0.4: Ripple Sequence Markers (set from the sequence in the constructor)
};

/// The state unchanged, as an EditResult.
EditResult unchanged(const ProjectState& state);

/// Ids of unlocked targeted tracks.
std::vector<TrackId> unlockedTargets(const Sequence& sequence);

} // namespace wabcut::edit::detail
