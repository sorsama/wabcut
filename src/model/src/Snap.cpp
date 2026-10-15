#include <wabcut/model/Snap.h>

#include <algorithm>

namespace wabcut::edit {

std::vector<Ticks> snapTargets(const Sequence& sequence, std::optional<Ticks> playhead, const SnapOptions& options) {
    std::vector<ClipId> excluded(options.exclude.begin(), options.exclude.end());
    std::sort(excluded.begin(), excluded.end());

    std::vector<Ticks> out;
    if (options.clipEdges) {
        sequence.forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                if (!std::binary_search(excluded.begin(), excluded.end(), c.id)) {
                    out.push_back(c.start());
                    out.push_back(c.end());
                }
            }
        });
    }
    if (options.playhead && playhead) {
        out.push_back(*playhead);
    }
    if (options.marks) {
        if (sequence.markIn) {
            out.push_back(*sequence.markIn);
        }
        if (sequence.markOut) {
            out.push_back(*sequence.markOut);
        }
    }
    if (options.markers) {
        for (const Marker& m : sequence.markers) {
            out.push_back(m.start);
            if (m.duration.value > 0) {
                out.push_back(m.start + m.duration);
            }
        }
    }
    if (options.beats && sequence.beatGrid) {
        const BeatGrid& grid = *sequence.beatGrid;
        const Ticks end = std::max(sequence.duration, sequence.contentEnd()) +
                          Ticks{grid.period().value * std::max<std::int32_t>(grid.beatsPerBar, 1)};
        for (const Beat& b : grid.beatsIn(TimeRange::fromStartEnd(Ticks{}, end))) {
            out.push_back(b.time);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<Ticks> nearestSnapTarget(std::span<const Ticks> sortedTargets, Ticks t, Ticks tolerance) {
    const auto it = std::lower_bound(sortedTargets.begin(), sortedTargets.end(), t);
    std::optional<Ticks> best;
    Ticks bestDistance{};
    const auto consider = [&](Ticks candidate) {
        const Ticks distance = candidate > t ? candidate - t : t - candidate;
        if (distance <= tolerance && (!best || distance < bestDistance)) {
            best = candidate;
            bestDistance = distance;
        }
    };
    if (it != sortedTargets.begin()) {
        consider(*(it - 1));
    }
    if (it != sortedTargets.end()) {
        consider(*it);
    }
    return best;
}

} // namespace wabcut::edit
