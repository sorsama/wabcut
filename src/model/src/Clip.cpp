#include <wabcut/model/Clip.h>

#include <wabcut/model/Graphics.h>

#include <algorithm>

namespace wabcut {

bool ClipContent::operator==(const ClipContent& o) const {
    const bool sameGraphic = graphic == o.graphic || (graphic && o.graphic && *graphic == *o.graphic);
    return name == o.name && properties == o.properties && effects == o.effects && timeRemap == o.timeRemap &&
           unknownSource == o.unknownSource && channels == o.channels && sameGraphic && blendMode == o.blendMode;
}

// Direction-aware source bookkeeping. A forward clip shows [sourceIn, sourceOut) from its head to its tail;
// a reversed clip shows it from its tail (sourceIn) to its head (sourceOut); a freeze frame shows sourceIn
// everywhere and can grow without limit. Trims keep the source under the untouched end exact.

namespace {

/// A remap segment that shows a frame past the media: a hold at (or beyond) its end. Keys never lie past the end
/// (validate), so moving segments always show frames inside it.
bool showsPastEnd(const RemapSegment& s, std::optional<Ticks> extent) {
    return extent && s.motion == RemapMotion::Hold && s.sourceFrom >= *extent;
}

} // namespace

std::optional<Ticks> Clip::headRoom(std::optional<Ticks> extent) const { return headRoom(Ticks{}, extent); }

std::optional<Ticks> Clip::tailRoom(std::optional<Ticks> extent) const { return tailRoom(Ticks{}, extent); }

// A remapped clip's keys lie inside [start, end] (validate, the remap ops), so every frame a moving segment shows does
// too; only a hold at (or past) the end bounds it.
std::optional<Ticks> Clip::headRoom(Ticks start, std::optional<Ticks> end) const {
    if (const Curve* remap = timeRemap()) {
        // Curve time stays >= 0, and the head stops at a hold past the media's end.
        Ticks room = sourceIn;
        for (const RemapSegment& s : remapSegments(*remap)) {
            if (s.to <= sourceIn && showsPastEnd(s, end)) {
                room = std::min(room, sourceIn - s.to);
            }
        }
        return std::max(room, Ticks{});
    }
    if (isFreeze()) {
        return std::nullopt;
    }
    if (!reversed) {
        return toTimelineTicks(sourceIn - start, speed);
    }
    if (!end) {
        return std::nullopt;
    }
    return toTimelineTicks(*end - sourceOut(), speed);
}

std::optional<Ticks> Clip::tailRoom(Ticks start, std::optional<Ticks> end) const {
    if (const Curve* remap = timeRemap()) {
        // Unbounded (the last key holds) unless a hold past the media's end follows.
        const Ticks out = sourceOut();
        for (const RemapSegment& s : remapSegments(*remap)) {
            if (s.to > out && showsPastEnd(s, end)) {
                return std::max(s.from, out) - out; // s.from is the lowest Ticks for the leading hold
            }
        }
        return std::nullopt;
    }
    if (isFreeze()) {
        return std::nullopt;
    }
    if (reversed) {
        return toTimelineTicks(sourceIn - start, speed);
    }
    if (!end) {
        return std::nullopt;
    }
    return toTimelineTicks(*end - sourceOut(), speed);
}

void Clip::moveHead(Ticks d) {
    if (!reversed && !isFreeze()) {
        sourceIn += toSourceTicks(d, speed);
    }
    // Reversed: the head shows the top of the source range, so sourceIn (the bottom, at the tail) stays.
    timelineRange.start += d;
    timelineRange.duration -= d;
}

void Clip::moveTail(Ticks d) {
    if (reversed && !isFreeze()) {
        // The tail shows the bottom of the source range: keep the top (sourceOut) where it is.
        const Ticks top = sourceOut();
        timelineRange.duration += d;
        sourceIn = top - toSourceTicks(timelineRange.duration, speed);
        return;
    }
    timelineRange.duration += d;
}

Clip Clip::splitOff(Ticks at) {
    Clip right = *this;
    const Ticks leftDuration = at - start();
    right.timelineRange = TimeRange::fromStartEnd(at, end());
    if (!isFreeze()) {
        if (reversed) {
            // The left piece keeps the top of the range, the right piece the bottom (from the old sourceIn).
            sourceIn = sourceOut() - toSourceTicks(leftDuration, speed);
        } else {
            right.sourceIn = sourceIn + toSourceTicks(leftDuration, speed);
        }
    }
    timelineRange.duration = leftDuration;
    return right;
}

std::vector<TimeRange> Clip::unitySpeedRanges() const {
    std::vector<TimeRange> out;
    if (const Curve* remap = timeRemap()) {
        const Ticks from = sourceIn;
        const Ticks to = sourceOut();
        for (const RemapSegment& s : remapSegments(*remap)) {
            const Ticks a = std::max(from, s.from);
            const Ticks b = std::min(to, s.to);
            if (a < b && s.isUnitySpeed()) {
                const TimeRange r = TimeRange::fromStartEnd(start() + (a - from), start() + (b - from));
                if (!out.empty() && out.back().end() == r.start) {
                    out.back().duration += r.duration; // consecutive 100 % segments that continue the media
                } else {
                    out.push_back(r);
                }
            }
        }
        return out;
    }
    if (speed.isUnity() && !reversed) {
        out.push_back(timelineRange);
    }
    return out;
}

TimeRange Clip::shownSourceRange(Ticks from, Ticks to) const {
    if (const Curve* remap = timeRemap()) {
        const auto [lo, hi] = remapShownRange(*remap, curveTimeAt(from), curveTimeAt(to));
        return TimeRange::fromStartEnd(lo, hi + Ticks{1});
    }
    const Ticks a = sourceTimeAt(from);
    const Ticks b = sourceTimeAt(to - Ticks{1});
    return TimeRange::fromStartEnd(std::min(a, b), std::max(a, b) + Ticks{1});
}

std::optional<Ticks> Clip::timelineTimeOfSourceTime(Ticks source) const {
    if (isFreeze()) {
        return std::nullopt;
    }
    const Curve* remap = timeRemap();
    if (!remap) {
        return source < sourceIn || source >= sourceOut() ? std::nullopt : timelineTimeOfCurveTime(source);
    }
    const Ticks from = sourceIn;
    const Ticks to = sourceOut();
    for (const RemapSegment& s : remapSegments(*remap)) {
        const Ticks a = std::max(from, s.from);
        const Ticks b = std::min(to, s.to);
        if (a >= b) {
            continue;
        }
        const Ticks first = remapSourceAt(*remap, a);
        const Ticks last = remapSourceAt(*remap, b - Ticks{1});
        if (source < std::min(first, last) || source > std::max(first, last)) {
            continue;
        }
        if (s.motion == RemapMotion::Hold) {
            if (source == first) {
                return start() + (a - from);
            }
            continue;
        }
        // The segment is monotonic: the first curve time whose source reaches `source`.
        const bool forward = s.motion == RemapMotion::Forward;
        Ticks lo = a;
        Ticks hi = b - Ticks{1};
        while (lo < hi) {
            const Ticks mid = lo + Ticks{(hi - lo).value / 2};
            const Ticks shown = remapSourceAt(*remap, mid);
            if (forward ? shown >= source : shown <= source) {
                hi = mid;
            } else {
                lo = mid + Ticks{1};
            }
        }
        return start() + (lo - from);
    }
    return std::nullopt;
}

std::optional<Ticks> Clip::timelineTimeOfCurveTime(Ticks curveTime) const {
    std::optional<Ticks> t;
    if (isFreeze() || isRemapped()) {
        t = start() + (curveTime - sourceIn);
    } else {
        const Ticks offset = reversed ? (sourceOut() - Ticks{1}) - curveTime : curveTime - sourceIn;
        if (const auto scaled = mulDiv(offset.value, speed.den, speed.num, Rounding::Ceil)) {
            t = start() + Ticks{*scaled};
        }
    }
    if (!t || *t < start() || *t >= end()) {
        return std::nullopt;
    }
    return t;
}

} // namespace wabcut
