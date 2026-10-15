#pragma once

// Timeline indicators computed from the sequence (M5i), for TimelineView and tests.
//
//  * Through edits (Premiere's Show Through Edits): a cut between two adjacent clips that continue the same source
//    with nothing changed, as Add Edit leaves them; joining them would change nothing.
//  * Duplicate frames (Premiere's duplicate frame markers): video that uses source frames another video clip of the
//    sequence also uses. Each media item with duplicates gets its own colour; every clip gets the timeline ranges
//    where it shows a duplicated frame.
//  * Clip markers (M5i): where a clip shows its media's markers.

#include <wabcut/model/Sequence.h>

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace wabcut::app {

/// `b` starts where `a` ends and continues its source: same media and stream, same speed, both forward (or both
/// reversed), b's source picks up where a's left off, and the same enabled state, label colour and content.
bool isThroughEdit(const Clip& a, const Clip& b);

struct DuplicateRange {
    TimeRange range;   ///< timeline time
    int colorIndex = 0; ///< one per media item with duplicates, in order of first appearance (V1 first, by time)
};

/// clip id -> the clip's duplicate ranges (only clips that have any). Video tracks only; freeze frames count as one
/// frame.
std::unordered_map<std::uint64_t, std::vector<DuplicateRange>> duplicateFrameRanges(const Sequence& sequence);

/// A media marker where a clip shows it (timeline time).
struct ClipMarkerMark {
    Ticks at;                 ///< where the marker starts
    bool ranged = false;      ///< a marker with a duration
    std::optional<Ticks> end; ///< a ranged marker's end inside the clip (nullopt: it runs to the clip's end)
    std::int32_t colorIndex = 0;
};

/// The media's markers (`markers`, media time, sorted) that `clip` shows, where it shows them.
std::vector<ClipMarkerMark> clipMarkerMarks(const Clip& clip, std::span<const Marker> markers);

} // namespace wabcut::app
