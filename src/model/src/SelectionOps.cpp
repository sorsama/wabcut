// Timeline selection and navigation queries (TimelineSelection.h).

#include <wabcut/model/TimelineSelection.h>

#include <wabcut/model/EditOps.h>

#include <algorithm>
#include <iterator>

namespace wabcut::edit {

std::vector<ClipId> clipsUnderTime(const Sequence& sequence, Ticks time) {
    std::vector<TrackId> tracks = targetedTracks(sequence);
    if (tracks.empty()) {
        tracks = allTracks(sequence);
    }
    std::vector<ClipId> out;
    for (const TrackId id : tracks) {
        const Track* track = sequence.findTrack(id);
        if (const auto index = track ? track->clipIndexAt(time) : std::nullopt) {
            out.push_back(track->clips[*index].id);
        }
    }
    return out;
}

std::optional<Ticks> sequenceGapStart(const Sequence& sequence, Ticks time, bool forward) {
    // The clips' ranges on every track, merged: the gaps are the holes between them (and before the first one).
    std::vector<TimeRange> ranges;
    sequence.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            ranges.push_back(c.timelineRange);
        }
    });
    std::ranges::sort(ranges, [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<Ticks> gaps; // gap starts, ascending
    Ticks covered{};         // the end of the material so far
    for (const TimeRange& r : ranges) {
        if (r.start > covered) {
            gaps.push_back(covered);
        }
        covered = std::max(covered, r.end());
    }
    if (forward) {
        const auto it = std::ranges::upper_bound(gaps, time);
        return it == gaps.end() ? std::nullopt : std::optional<Ticks>(*it);
    }
    const auto it = std::ranges::lower_bound(gaps, time);
    return it == gaps.begin() ? std::nullopt : std::optional<Ticks>(*std::prev(it));
}

} // namespace wabcut::edit
