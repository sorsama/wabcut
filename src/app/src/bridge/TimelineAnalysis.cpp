#include "TimelineAnalysis.h"

#include <algorithm>
#include <map>
#include <utility>

namespace wabcut::app {

bool isThroughEdit(const Clip& a, const Clip& b) {
    if (a.end() != b.start() || a.media != b.media || a.stream != b.stream || a.speed != b.speed ||
        a.reversed != b.reversed || a.isFreeze() || b.isFreeze() || a.enabled != b.enabled ||
        a.colorIndex != b.colorIndex || !(a.content() == b.content()))
        return false;
    return a.reversed ? b.sourceOut() == a.sourceIn : a.sourceOut() == b.sourceIn;
}

namespace {

struct Use {
    std::uint64_t clip;
    Ticks sourceStart, sourceEnd; // [start, end)
    const Clip* ptr;
};

/// Source ranges [start, end) used by two or more of `uses` (merged, ascending).
std::vector<std::pair<Ticks, Ticks>> sharedRanges(const std::vector<Use>& uses) {
    std::vector<std::pair<Ticks, int>> events;
    events.reserve(uses.size() * 2);
    for (const Use& u : uses) {
        events.emplace_back(u.sourceStart, +1);
        events.emplace_back(u.sourceEnd, -1);
    }
    // Ends before starts at the same time: touching ranges do not overlap.
    std::sort(events.begin(), events.end(), [](const auto& x, const auto& y) {
        return x.first != y.first ? x.first < y.first : x.second < y.second;
    });
    std::vector<std::pair<Ticks, Ticks>> out;
    int depth = 0;
    Ticks openedAt;
    for (const auto& [t, delta] : events) {
        const int before = depth;
        depth += delta;
        if (before < 2 && depth >= 2) {
            openedAt = t;
        } else if (before >= 2 && depth < 2 && t > openedAt) {
            if (!out.empty() && out.back().second == openedAt)
                out.back().second = t;
            else
                out.emplace_back(openedAt, t);
        }
    }
    return out;
}

/// The media `clip` shows. A remapped clip's sourceIn/sourceOut are curve times (review P2-24): its shown range is read
/// from the curve and widened to whole frames of `rate`, since a hold shows only its frame's first tick.
std::pair<Ticks, Ticks> shownSource(const Clip& clip, Rate rate) {
    if (!clip.isRemapped())
        return {clip.sourceIn, clip.isFreeze() ? clip.sourceIn + Ticks{1} : clip.sourceOut()};
    const TimeRange shown = clip.shownSourceRange(clip.start(), clip.end());
    const auto first = ticksToFrames(shown.start, rate, Rounding::Floor);
    const auto last = ticksToFrames(shown.end(), rate, Rounding::Ceil);
    const auto lo = first ? framesToTicks(*first, rate, Rounding::Floor) : std::nullopt;
    const auto hi = last ? framesToTicks(*last, rate, Rounding::Ceil) : std::nullopt;
    if (!lo || !hi)
        return {shown.start, shown.end()};
    return {*lo, *hi};
}

/// The timeline ranges of a remapped `clip` whose frames (sampled at `rate` from its start) show media in [s0, s1):
/// holds, ramps and reverse segments map one media range to any number of timeline ranges.
void remappedRanges(const Clip& clip, Ticks s0, Ticks s1, Rate rate, std::vector<TimeRange>& out) {
    std::optional<Ticks> runStart;
    Ticks t = clip.start();
    for (std::int64_t i = 0; t < clip.end() && i < 1'000'000; ++i) {
        t = clip.start() + framesToTicks(i, rate);
        if (t >= clip.end())
            break;
        const Ticks shown = clip.sourceTimeAt(t);
        const bool inside = shown >= s0 && shown < s1;
        if (inside && !runStart) {
            runStart = t;
        } else if (!inside && runStart) {
            out.push_back(TimeRange::fromStartEnd(*runStart, t));
            runStart.reset();
        }
    }
    if (runStart)
        out.push_back(TimeRange::fromStartEnd(*runStart, clip.end()));
}

/// Timeline time of source time `s` in `clip` (forward clips; linear, for drawing).
Ticks timelineAt(const Clip& clip, Ticks s) {
    const Ticks sourceDuration = clip.sourceDuration();
    if (sourceDuration.value <= 0)
        return clip.start();
    const double fraction = static_cast<double>((s - clip.sourceIn).value) / static_cast<double>(sourceDuration.value);
    const double offset = std::clamp(fraction, 0.0, 1.0) * static_cast<double>(clip.duration().value);
    return clip.start() + Ticks{static_cast<std::int64_t>(offset)};
}

} // namespace

std::unordered_map<std::uint64_t, std::vector<DuplicateRange>> duplicateFrameRanges(const Sequence& sequence) {
    // Uses per media (and stream), in track and time order so colours follow the timeline.
    std::map<std::pair<std::uint64_t, std::int32_t>, std::vector<Use>> byMedia;
    std::vector<std::pair<std::uint64_t, std::int32_t>> order;
    for (const TrackPtr& track : sequence.videoTracks) {
        for (const Clip& clip : track->clips) {
            const auto key = std::pair{clip.media.value, clip.stream.index};
            const auto [start, end] = shownSource(clip, sequence.frameRate);
            auto& uses = byMedia[key];
            if (uses.empty())
                order.push_back(key);
            uses.push_back({clip.id.value, start, end, &clip});
        }
    }
    std::unordered_map<std::uint64_t, std::vector<DuplicateRange>> out;
    int color = 0;
    for (const auto& key : order) {
        const std::vector<Use>& uses = byMedia[key];
        if (uses.size() < 2)
            continue;
        const auto shared = sharedRanges(uses);
        if (shared.empty())
            continue;
        for (const Use& u : uses) {
            // The shared ranges this use overlaps (shared is sorted and disjoint).
            auto it = std::upper_bound(shared.begin(), shared.end(), u.sourceStart,
                                       [](Ticks t, const auto& r) { return t < r.second; });
            for (; it != shared.end() && it->first < u.sourceEnd; ++it) {
                const Ticks s0 = std::max(it->first, u.sourceStart);
                const Ticks s1 = std::min(it->second, u.sourceEnd);
                if (s1 <= s0)
                    continue;
                const Clip& clip = *u.ptr;
                if (clip.isRemapped()) {
                    std::vector<TimeRange> ranges;
                    remappedRanges(clip, s0, s1, sequence.frameRate, ranges);
                    for (const TimeRange& r : ranges)
                        out[u.clip].push_back({r, color});
                    continue;
                }
                Ticks t0, t1;
                if (clip.isFreeze()) {
                    t0 = clip.start();
                    t1 = clip.end();
                } else if (clip.reversed) {
                    // Reversed: the clip's start shows the top of its source range.
                    t0 = clip.start() + (timelineAt(clip, clip.sourceOut()) - timelineAt(clip, s1));
                    t1 = clip.start() + (timelineAt(clip, clip.sourceOut()) - timelineAt(clip, s0));
                } else {
                    t0 = timelineAt(clip, s0);
                    t1 = timelineAt(clip, s1);
                }
                if (t1 > t0)
                    out[u.clip].push_back({TimeRange::fromStartEnd(t0, t1), color});
            }
        }
        ++color;
    }
    return out;
}

std::vector<ClipMarkerMark> clipMarkerMarks(const Clip& clip, std::span<const Marker> markers) {
    std::vector<ClipMarkerMark> out;
    // The media the clip shows, where it shows it: a remapped clip's sourceIn/sourceOut are curve times (review
    // P2-24), and speed, reverse and remap segments move where a media time appears.
    const TimeRange shown = clip.shownSourceRange(clip.start(), clip.end());
    for (const Marker& m : markers) {
        if (!shown.contains(m.start))
            continue;
        const auto t = clip.timelineTimeOfSourceTime(m.start);
        if (!t)
            continue;
        ClipMarkerMark mark;
        mark.at = *t;
        mark.colorIndex = m.colorIndex;
        if (m.duration > Ticks{}) {
            mark.ranged = true;
            mark.end = clip.timelineTimeOfSourceTime(std::min(m.end(), shown.end() - Ticks{1}));
        }
        out.push_back(mark);
    }
    return out;
}

} // namespace wabcut::app
