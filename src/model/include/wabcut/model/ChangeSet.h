#pragma once

// ChangeSet: what an edit, undo or redo changed, so views (timeline, monitors, caches) update only that.
//
// Every EditResult carries one, EditSession hands one to its listeners for every apply/undo/redo, and
// diff() computes one between any two revisions. diff() relies on structural sharing: pointer-equal
// sequences, tracks and clip chunks are skipped without looking inside, so its cost is proportional to
// what changed, not to the size of the project.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Parameters.h>
#include <wabcut/model/Time.h>

#include <vector>

namespace wabcut {

struct ProjectState;
struct Sequence;

struct TrackChange {
    TrackId track;
    /// Union of the old and the new extents of every clip added, removed or modified on this track:
    /// sorted, disjoint. Redraw at least these ranges.
    std::vector<TimeRange> ranges;
    bool headerChanged = false;      ///< name, flags (lock, sync lock, target, mute, solo, output), role or effects
    bool transitionsChanged = false;
    bool added = false;              ///< the track is new
    bool removed = false;            ///< the track was deleted
};

struct SequenceChange {
    SequenceId sequence;
    /// Tracks with any change, in the old track order followed by new tracks.
    std::vector<TrackChange> tracks;
    std::vector<ClipId> addedClips;
    std::vector<ClipId> removedClips;
    /// Clips that exist before and after but differ, including clips moved to another track
    /// (in track and time order, moved clips last).
    std::vector<ClipId> modifiedClips;
    /// What changed on the modified clips.
    PropertyRoles roles;
    bool settingsChanged = false;  ///< name, bin, frame/audio rate, canvas, drop-frame display, master effect stacks
    bool marksChanged = false;     ///< In/Out marks
    bool markersChanged = false;
    bool extentChanged = false;    ///< explicit duration or work area
    bool trackListChanged = false; ///< tracks added, removed or reordered
    bool captionsChanged = false;  ///< caption tracks or their cues
    bool beatGridChanged = false;  ///< the beat grid was set, changed or removed

    const TrackChange* track(TrackId id) const;
    std::vector<TrackId> affectedTracks() const;
    bool empty() const;
};

struct ChangeSet {
    /// Sequences that exist before and after and changed.
    std::vector<SequenceChange> sequences;
    std::vector<SequenceId> addedSequences;
    std::vector<SequenceId> removedSequences;
    std::vector<MediaId> addedMedia;
    std::vector<MediaId> removedMedia;
    std::vector<MediaId> modifiedMedia;
    bool binsChanged = false;
    bool aiLockChanged = false; ///< ProjectState::aiNoCloud
    bool assetsChanged = false; ///< ProjectState::assets (added, removed, relinked)
    bool redactionsChanged = false; ///< ProjectState::redactions (I.6)
    bool speakersChanged = false;   ///< ProjectState::speakers (C.7)
    bool transcriptEditsChanged = false; ///< ProjectState::transcriptEdits (C.7)
    bool captionPresetsChanged = false;  ///< ProjectState::captionPresets (C.7)
    bool reviewNotesChanged = false;     ///< ProjectState::reviewNotes (C.9)
    bool brandKitChanged = false;        ///< ProjectState::brandKit (C.13)
    bool templatesChanged = false;       ///< ProjectState::templates (C.13)
    bool colorManagementChanged = false; ///< ProjectState::colorManagement (I.9)
    bool textStylesChanged = false;      ///< ProjectState::textStyles (0.2)
    bool freeformChanged = false;        ///< ProjectState::freeform (0.2, P.3: Freeform views)

    const SequenceChange* sequence(SequenceId id) const;
    bool empty() const;
};

/// What changed from `before` to `after`.
ChangeSet diff(const ProjectState& before, const ProjectState& after);

/// What changed in one sequence (both revisions of the same sequence id).
SequenceChange diffSequence(const Sequence& before, const Sequence& after);

} // namespace wabcut
