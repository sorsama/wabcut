#pragma once

// Snap targets for the timeline (Premiere's "S" snapping): clip edges, playhead, In/Out marks, marker
// edges and, when the sequence has a beat grid, its beats (beat snapping for music-video editing). The UI converts its pixel tolerance to ticks and asks for the nearest target.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <optional>
#include <span>
#include <vector>

namespace wabcut::edit {

struct SnapOptions {
    bool clipEdges = true;
    bool playhead = true;
    bool marks = true;
    bool markers = true;
    /// Beats of the sequence's beat grid, up to a bar past the end of the sequence.
    bool beats = true;
    /// Clips whose edges are not targets (typically the clips being dragged).
    std::span<const ClipId> exclude;
};

/// Sorted, de-duplicated snap positions.
std::vector<Ticks> snapTargets(const Sequence& sequence, std::optional<Ticks> playhead,
                               const SnapOptions& options = {});

/// The target nearest to `t` within `tolerance` (inclusive); the earlier one wins a tie.
std::optional<Ticks> nearestSnapTarget(std::span<const Ticks> sortedTargets, Ticks t, Ticks tolerance);

} // namespace wabcut::edit
