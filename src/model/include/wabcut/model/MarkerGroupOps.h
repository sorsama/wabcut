#pragma once

// 0.7 (card 119): editing several markers at once. Pure functions on a marker list (a sequence's markers in sequence
// time, or a media item's clip markers in media time): they take the whole list and give the whole new list, sorted by
// start like every marker list, plus where the markers they touched ended up, so the caller can keep them selected. The
// caller stores the list with edit::setSequenceMarkers / edit::setMediaMarkers (one undo step for the whole group).

#include <wabcut/model/Error.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

namespace wabcut::edit {

/// A marker list after a group edit.
struct MarkerGroupEdit {
    std::vector<Marker> markers;       ///< the whole list, sorted by start (equal starts keep their order)
    std::vector<std::size_t> touched;  ///< indices (into `markers`, ascending) of the markers the edit moved or added
    Ticks appliedDelta;                ///< moveMarkersBy: the move actually made (clamped so no marker starts before 0)
};

/// The markers at `indices` (each at most once; indices into `markers`) removed. InvalidArgument for an index out of range
/// or none given.
std::expected<MarkerGroupEdit, Error> removeMarkersAt(std::vector<Marker> markers, std::span<const std::size_t> indices);

/// The markers at `indices` moved by `delta` (negative = earlier; their highlight peaks go with them). The move is cut
/// short so the earliest of them does not start before 0; appliedDelta says how far they went. InvalidArgument for an
/// index out of range or none given.
std::expected<MarkerGroupEdit, Error> moveMarkersBy(std::vector<Marker> markers, std::span<const std::size_t> indices, Ticks delta);

/// Copies of the markers at `indices`, each `offset` later than its original (offset > 0 or < 0, never 0; a copy that would
/// start before 0 is an error). The copies are what `touched` lists. Fails when the list would pass kMaxMarkers.
std::expected<MarkerGroupEdit, Error> duplicateMarkersBy(std::vector<Marker> markers, std::span<const std::size_t> indices,
                                                         Ticks offset);

/// `pasted` (a copied group, in any order) added to `markers` with the earliest of them starting at `at` and the others
/// keeping their distance from it. Fails when the list would pass kMaxMarkers or a copy is not a valid marker.
std::expected<MarkerGroupEdit, Error> pasteMarkersAt(std::vector<Marker> markers, std::span<const Marker> pasted, Ticks at);

} // namespace wabcut::edit
