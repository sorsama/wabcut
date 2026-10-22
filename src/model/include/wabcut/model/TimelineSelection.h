#pragma once

// Timeline queries for selection and navigation (0.2 parity round): Premiere's Sequence > Selection Follows Playhead
// and Sequence > Go to Gap > Next / Previous in Sequence. Pure queries: the app keeps the selection and the playhead
// (ProjectController) and expands a selection with groups and links (ClipGroups.h withGroupedClips).

#include <wabcut/model/Sequence.h>

#include <optional>
#include <vector>

namespace wabcut::edit {

/// The clips under `time` on the targeted tracks (every track when none is targeted), in track order: what Selection
/// Follows Playhead selects as the playhead moves.
std::vector<ClipId> clipsUnderTime(const Sequence& sequence, Ticks time);

/// Go to Gap > Next in Sequence (`forward`, Shift+;) / Previous in Sequence (Ctrl+Shift+;): the start of the next
/// (previous) gap after (before) `time` where no track has a clip, between the sequence start and its last clip.
/// nullopt when there is none.
std::optional<Ticks> sequenceGapStart(const Sequence& sequence, Ticks time, bool forward);

} // namespace wabcut::edit
