#pragma once

// What changed between two revisions of a project (C.9): for version history's "compare versions" view and for
// people. Clips, tracks, sequences and media match by id (ids are stable across edits, saves and history), so a clip
// that moved is "moved", not removed and added.

#include <wabcut/model/ProjectState.h>

#include <string>
#include <vector>

namespace wabcut {

struct ClipDiff {
    enum class Kind : std::uint8_t { Added, Removed, Changed };
    Kind kind = Kind::Changed;
    ClipId clip;
    std::string name;        ///< after the change (before, for a removed clip)
    std::string trackBefore; ///< "V1", "A2"; empty for an added clip
    std::string trackAfter;  ///< empty for a removed clip
    TimeRange before;        ///< timeline range before (removed, changed)
    TimeRange after;         ///< timeline range after (added, changed)
    // Changed: what changed.
    bool moved = false;   ///< another start or track
    bool trimmed = false; ///< another length or source In
    bool retimed = false; ///< speed, direction, freeze or time remap
    bool source = false;  ///< plays other media, another stream or another sequence; or its graphic changed (C.13)
    bool enabled = false; ///< enabled or disabled
    bool renamed = false;
    bool relinked = false; ///< linked or unlinked
    std::vector<std::string> effectsAdded;   ///< effect ids
    std::vector<std::string> effectsRemoved; ///< effect ids
    std::vector<std::string> effectsChanged; ///< effect ids whose parameters, keyframes, masks or state changed
    std::vector<std::string> keyframes;      ///< intrinsic parameters whose keyframes changed ("Opacity")
    std::vector<std::string> parameters;     ///< intrinsic parameters whose constant value changed
};

struct SequenceDiff {
    enum class Kind : std::uint8_t { Added, Removed, Changed };
    Kind kind = Kind::Changed;
    SequenceId id;
    std::string name;
    Rate frameRate = kRate30; ///< for timecodes
    bool dropFrame = false;
    std::vector<std::string> settings;      ///< "name", "frame rate", "canvas", ... that changed
    std::vector<std::string> tracksAdded;   ///< "V3"
    std::vector<std::string> tracksRemoved;
    std::vector<std::string> tracksChanged; ///< track header, effects or faders changed
    bool marksChanged = false;
    bool markersChanged = false;
    bool captionsChanged = false;
    bool transitionsChanged = false;
    std::vector<ClipDiff> clips; ///< in track order (V1.., A1..), then time
};

struct ProjectDiff {
    std::vector<SequenceDiff> sequences; ///< added, removed and changed ones, by id
    std::vector<std::string> mediaAdded;  ///< names
    std::vector<std::string> mediaRemoved;
    std::vector<std::string> mediaChanged; ///< relinked, renamed, marks, markers
    std::size_t reviewNotesAdded = 0;
    std::size_t reviewNotesRemoved = 0;
    std::size_t reviewNotesChanged = 0; ///< replies, status, text
    bool binsChanged = false;
    bool assetsChanged = false;
    bool redactionsChanged = false;
    bool transcriptsChanged = false; ///< speakers or transcript edits
    bool graphicsLibraryChanged = false; ///< C.13: the brand kit or the graphic templates

    bool empty() const;
    /// One line per change for people, with timecodes in each sequence's rate, e.g.
    /// "Main Edit: V1 'Interview' trimmed 00:00:04:00-00:00:08:00 -> 00:00:04:00-00:00:07:12".
    std::vector<std::string> summary() const;
};

/// What changed from `before` to `after`.
ProjectDiff diffProjects(const ProjectState& before, const ProjectState& after);

} // namespace wabcut
