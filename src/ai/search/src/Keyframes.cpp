#include <wabcut/ai/search/Keyframes.h>

#include <algorithm>

namespace wabcut::ai::search {

std::vector<Keyframe> planKeyframes(const analysis::ShotAnalysis& shots, const KeyframeOptions& options) {
    std::vector<Keyframe> out;
    if (shots.end <= shots.start) {
        return out;
    }
    std::vector<Ticks> bounds{shots.start};
    for (const analysis::ShotCut& cut : shots.cuts) {
        if (cut.time > bounds.back() && cut.time < shots.end) {
            bounds.push_back(cut.time);
        }
    }
    bounds.push_back(shots.end);

    // A longer interval when the plain one would exceed the budget (never shorter than asked).
    std::int64_t interval = std::max<std::int64_t>(options.interval.value, 1);
    const std::int64_t total = shots.end.value - shots.start.value;
    const auto budget = static_cast<std::int64_t>(std::max<std::size_t>(options.maxKeyframes, bounds.size()));
    if (total / interval + static_cast<std::int64_t>(bounds.size()) > budget) {
        interval = std::max(interval, total / std::max<std::int64_t>(budget - static_cast<std::int64_t>(bounds.size()), 1) + 1);
    }

    for (std::size_t i = 0; i + 1 < bounds.size(); ++i) {
        const TimeRange shot = TimeRange::fromStartEnd(bounds[i], bounds[i + 1]);
        const std::int64_t length = shot.duration.value;
        const std::int64_t pieces = std::max<std::int64_t>(1, (length + interval - 1) / interval);
        for (std::int64_t p = 0; p < pieces; ++p) {
            const Ticks start{shot.start.value + length * p / pieces};
            const Ticks end{shot.start.value + length * (p + 1) / pieces};
            if (end <= start) {
                continue;
            }
            out.push_back(Keyframe{Ticks{start.value + (end.value - start.value) / 2}, TimeRange::fromStartEnd(start, end), shot});
        }
    }
    return out;
}

} // namespace wabcut::ai::search
