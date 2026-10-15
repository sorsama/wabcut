#pragma once

// What a staged edit does, for the timeline's read-only preview (the assistant panel shows it over the CURRENT
// sequence, so the user sees what goes before accepting): content the edit cuts as base-time ranges (struck
// through), new content as ghost clips, new markers. Comparison is by source content per track, like the
// "protected" check of Verification.h, so ripple shifts and splits are not changes and a moved range is neither
// cut nor added.

#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <vector>

namespace wabcut::ai::agent {

struct StagedPreview {
    struct Range {
        TrackId track;
        Ticks start;
        Ticks end;

        bool operator==(const Range&) const = default;
    };
    std::vector<Range> removed;  ///< BASE timeline: what the edit cuts, merged per track, in track then time order
    std::vector<Range> added;    ///< STAGED timeline: content no base clip on that track had (inserts, titles)
    std::vector<Ticks> markers;  ///< staged markers that are new
    Ticks lengthBefore;          ///< content length of the base and the staged sequence
    Ticks lengthAfter;

    bool empty() const { return removed.empty() && added.empty() && markers.empty(); }
};

StagedPreview stagedPreview(const ProjectState& base, const ProjectState& staged, SequenceId sequence);

} // namespace wabcut::ai::agent
