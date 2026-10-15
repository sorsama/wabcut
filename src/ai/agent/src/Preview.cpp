#include <wabcut/ai/agent/Preview.h>

#include <algorithm>

namespace wabcut::ai::agent {

namespace {

using Interval = std::pair<Ticks, Ticks>; // source [lo, hi)

bool sameSource(const Clip& a, const Clip& b) {
    return a.media == b.media && a.stream == b.stream && a.speed == b.speed && a.reversed == b.reversed;
}

Interval sourceOf(const Clip& c) {
    // A freeze frame shows one source instant: one tick stands for it.
    return c.isFreeze() ? Interval{c.sourceIn, c.sourceIn + Ticks{1}} : Interval{c.sourceIn, c.sourceOut()};
}

/// Parts of [lo, hi) not covered by `cover`.
std::vector<Interval> uncovered(Interval range, std::vector<Interval> cover) {
    std::sort(cover.begin(), cover.end());
    std::vector<Interval> out;
    Ticks at = range.first;
    for (const auto& [a, b] : cover) {
        if (b <= at) {
            continue;
        }
        if (a >= range.second) {
            break;
        }
        if (a > at) {
            out.emplace_back(at, a);
        }
        at = std::max(at, b);
        if (at >= range.second) {
            break;
        }
    }
    if (at < range.second) {
        out.emplace_back(at, range.second);
    }
    return out;
}

/// The timeline range of `clip` that shows source [lo, hi) (a part of its source range).
std::pair<Ticks, Ticks> timelineOf(const Clip& clip, Interval part) {
    if (clip.isFreeze()) {
        return {clip.start(), clip.end()};
    }
    const auto toTimeline = [&](Ticks source) { return scaleTicks(source, clip.speed.den, clip.speed.num); };
    Ticks a;
    Ticks b;
    if (!clip.reversed) {
        a = clip.start() + toTimeline(part.first - clip.sourceIn);
        b = clip.start() + toTimeline(part.second - clip.sourceIn);
    } else {
        a = clip.start() + toTimeline(clip.sourceOut() - part.second);
        b = clip.start() + toTimeline(clip.sourceOut() - part.first);
    }
    return {std::clamp(a, clip.start(), clip.end()), std::clamp(b, clip.start(), clip.end())};
}

/// For every clip of `from` (on its track), the timeline ranges whose source is on no matching clip of the same
/// track in `other`.
void difference(const Sequence& from, const Sequence& other, std::vector<StagedPreview::Range>& out) {
    from.forEachTrack([&](const Track& track) {
        const Track* counterpart = other.findTrack(track.id);
        std::vector<StagedPreview::Range> ranges;
        for (const Clip& clip : track.clips) {
            std::vector<Interval> cover;
            if (counterpart) {
                for (const Clip& o : counterpart->clips) {
                    if (sameSource(clip, o)) {
                        cover.push_back(sourceOf(o));
                    }
                }
            }
            for (const Interval& part : uncovered(sourceOf(clip), std::move(cover))) {
                const auto [a, b] = timelineOf(clip, part);
                if (b > a) {
                    ranges.push_back({track.id, a, b});
                }
            }
        }
        std::sort(ranges.begin(), ranges.end(), [](const auto& x, const auto& y) { return x.start < y.start; });
        for (const auto& r : ranges) {
            if (!out.empty() && out.back().track == r.track && r.start <= out.back().end) {
                out.back().end = std::max(out.back().end, r.end);
            } else {
                out.push_back(r);
            }
        }
    });
}

} // namespace

StagedPreview stagedPreview(const ProjectState& base, const ProjectState& staged, SequenceId sequence) {
    StagedPreview p;
    const Sequence* before = base.findSequence(sequence);
    const Sequence* after = staged.findSequence(sequence);
    if (!before || !after) {
        return p;
    }
    p.lengthBefore = before->contentEnd();
    p.lengthAfter = after->contentEnd();
    difference(*before, *after, p.removed);
    difference(*after, *before, p.added);
    for (const Marker& m : after->markers) {
        const bool known = std::any_of(before->markers.begin(), before->markers.end(),
                                       [&](const Marker& b) { return b.start == m.start && b.name == m.name; });
        if (!known) {
            p.markers.push_back(m.start);
        }
    }
    return p;
}

} // namespace wabcut::ai::agent
