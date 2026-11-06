// Editing several markers at once (MarkerGroupOps.h).

#include <wabcut/model/MarkerGroupOps.h>

#include <algorithm>
#include <string>
#include <utility>

namespace wabcut::edit {

namespace {

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message)}; }

/// A marker and whether the edit touched it.
struct Tagged {
    Marker marker;
    bool touched = false;
};

/// `indices` as a sorted, distinct list; an error for none or one out of range.
std::expected<std::vector<std::size_t>, Error> checkedIndices(std::size_t count, std::span<const std::size_t> indices) {
    if (indices.empty()) {
        return std::unexpected(invalid("name at least one marker"));
    }
    std::vector<std::size_t> sorted(indices.begin(), indices.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted.back() >= count) {
        return std::unexpected(invalid("there is no marker " + std::to_string(sorted.back())));
    }
    return sorted;
}

/// The list sorted by start (equal starts keep their order) with the indices of the touched markers.
MarkerGroupEdit finish(std::vector<Tagged> items) {
    std::stable_sort(items.begin(), items.end(), [](const Tagged& a, const Tagged& b) { return a.marker.start < b.marker.start; });
    MarkerGroupEdit out;
    out.markers.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].touched) {
            out.touched.push_back(i);
        }
        out.markers.push_back(std::move(items[i].marker));
    }
    return out;
}

std::vector<Tagged> tagAll(std::vector<Marker> markers) {
    std::vector<Tagged> items;
    items.reserve(markers.size());
    for (Marker& m : markers) {
        items.push_back({std::move(m), false});
    }
    return items;
}

/// `marker` shifted by `delta` (its highlight peak with it); the caller checked the start stays >= 0.
Marker shifted(Marker marker, Ticks delta) {
    marker.start = marker.start + delta;
    if (marker.highlight) {
        marker.highlight->peak = marker.highlight->peak + delta;
    }
    return marker;
}

} // namespace

std::expected<MarkerGroupEdit, Error> removeMarkersAt(std::vector<Marker> markers, std::span<const std::size_t> indices) {
    const auto sorted = checkedIndices(markers.size(), indices);
    if (!sorted) {
        return std::unexpected(sorted.error());
    }
    std::vector<Tagged> kept;
    kept.reserve(markers.size() - sorted->size());
    std::size_t next = 0;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        if (next < sorted->size() && (*sorted)[next] == i) {
            ++next;
            continue;
        }
        kept.push_back({std::move(markers[i]), false});
    }
    return finish(std::move(kept));
}

std::expected<MarkerGroupEdit, Error> moveMarkersBy(std::vector<Marker> markers, std::span<const std::size_t> indices, Ticks delta) {
    const auto sorted = checkedIndices(markers.size(), indices);
    if (!sorted) {
        return std::unexpected(sorted.error());
    }
    Ticks earliest = markers[sorted->front()].start;
    for (const std::size_t i : *sorted) {
        earliest = std::min(earliest, markers[i].start);
    }
    if (earliest + delta < Ticks{}) {
        delta = Ticks{} - earliest; // the earliest one stops at 0; the others keep their distance from it
    }
    std::vector<Tagged> items = tagAll(std::move(markers));
    for (const std::size_t i : *sorted) {
        items[i].marker = shifted(std::move(items[i].marker), delta);
        items[i].touched = true;
    }
    MarkerGroupEdit out = finish(std::move(items));
    out.appliedDelta = delta;
    return out;
}

std::expected<MarkerGroupEdit, Error> duplicateMarkersBy(std::vector<Marker> markers, std::span<const std::size_t> indices,
                                                         Ticks offset) {
    const auto sorted = checkedIndices(markers.size(), indices);
    if (!sorted) {
        return std::unexpected(sorted.error());
    }
    if (offset.value == 0) {
        return std::unexpected(invalid("the copies would sit on their originals: give an offset"));
    }
    if (markers.size() + sorted->size() > kMaxMarkers) {
        return std::unexpected(invalid("the markers would be more than " + std::to_string(kMaxMarkers) + " (the limit of a sequence or a clip)"));
    }
    std::vector<Marker> copies;
    for (const std::size_t i : *sorted) {
        if (markers[i].start + offset < Ticks{}) {
            return std::unexpected(invalid("a copy would start before 0"));
        }
        copies.push_back(shifted(markers[i], offset));
    }
    std::vector<Tagged> items = tagAll(std::move(markers));
    for (Marker& copy : copies) {
        items.push_back({std::move(copy), true});
    }
    return finish(std::move(items));
}

std::expected<MarkerGroupEdit, Error> pasteMarkersAt(std::vector<Marker> markers, std::span<const Marker> pasted, Ticks at) {
    if (pasted.empty()) {
        return std::unexpected(invalid("there are no copied markers to paste"));
    }
    if (at < Ticks{}) {
        return std::unexpected(invalid("markers cannot start before 0"));
    }
    if (markers.size() + pasted.size() > kMaxMarkers) {
        return std::unexpected(invalid("the markers would be more than " + std::to_string(kMaxMarkers) + " (the limit of a sequence or a clip)"));
    }
    Ticks first = pasted.front().start;
    for (const Marker& m : pasted) {
        first = std::min(first, m.start);
    }
    std::vector<Tagged> items = tagAll(std::move(markers));
    for (const Marker& m : pasted) {
        Marker copy = shifted(m, at - first);
        if (const auto problem = markerProblem(copy)) {
            return std::unexpected(invalid(*problem));
        }
        items.push_back({std::move(copy), true});
    }
    return finish(std::move(items));
}

} // namespace wabcut::edit
