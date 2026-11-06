#include <wabcut/model/Clip.h>

#include <wabcut/model/Auditions.h>
#include <wabcut/model/Graphics.h>

#include <algorithm>

namespace wabcut {

bool ClipContent::operator==(const ClipContent& o) const {
    const bool sameGraphic = graphic == o.graphic || (graphic && o.graphic && *graphic == *o.graphic);
    return name == o.name && note == o.note && properties == o.properties && effects == o.effects && timeRemap == o.timeRemap &&
           unknownSource == o.unknownSource && channels == o.channels && sameGraphic && blendMode == o.blendMode &&
           scaleToFrame == o.scaleToFrame && nearestNeighbor == o.nearestNeighbor && holdFilters == o.holdFilters &&
           timeInterpolation == o.timeInterpolation && inputColorSpace == o.inputColorSpace &&
           fieldOptions == o.fieldOptions && masterEffectsOff == o.masterEffectsOff &&
           (audition == o.audition || (audition && o.audition && *audition == *o.audition)) &&
           renderedNest == o.renderedNest;
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
    if (const Audition* a = audition()) {
        // 0.6: the audition now starts at the new In; every other take keeps its source under the rest of the range.
        const TimeRange whole = auditionRange(*this);
        Audition next = *a;
        for (AuditionTake& t : next.others) {
            Clip v = takeClip(t, whole);
            v.moveHead((start() + d) - whole.start);
            t.sourceIn = v.sourceIn;
        }
        next.headTrim = Ticks{};
        ClipContent block = content();
        block.audition = std::make_shared<const Audition>(std::move(next));
        setContent(std::move(block));
    }
    if (!reversed && !isFreeze()) {
        sourceIn += toSourceTicks(d, speed);
    }
    // Reversed: the head shows the top of the source range, so sourceIn (the bottom, at the tail) stays.
    timelineRange.start += d;
    timelineRange.duration -= d;
}

void Clip::moveTail(Ticks d) {
    if (const Audition* a = audition()) {
        // 0.6: the audition now ends at the new Out (a reversed take keeps the source under its head).
        const TimeRange whole = auditionRange(*this);
        Audition next = *a;
        for (AuditionTake& t : next.others) {
            Clip v = takeClip(t, whole);
            v.moveTail((end() + d) - whole.end());
            t.sourceIn = v.sourceIn;
        }
        next.tailTrim = Ticks{};
        ClipContent block = content();
        block.audition = std::make_shared<const Audition>(std::move(next));
        setContent(std::move(block));
    }
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
    if (const Audition* a = audition()) {
        // 0.6: both pieces keep every take: the left one over [audition start, at), the right one over [at, end).
        const TimeRange whole = auditionRange(*this);
        Audition left = *a;
        Audition rest = *a;
        for (std::size_t i = 0; i < a->others.size(); ++i) {
            Clip v = takeClip(a->others[i], whole);
            const Clip r = v.splitOff(at);
            left.others[i].sourceIn = v.sourceIn;
            rest.others[i].sourceIn = r.sourceIn;
        }
        left.tailTrim = Ticks{};
        rest.headTrim = Ticks{};
        ClipContent leftContent = content();
        leftContent.audition = std::make_shared<const Audition>(std::move(left));
        ClipContent rightContent = content();
        rightContent.audition = std::make_shared<const Audition>(std::move(rest));
        setContent(std::move(leftContent));
        right.setContent(std::move(rightContent));
    }
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
    if (isFreeze() && content().holdFilters) {
        t = curveTime == sourceIn ? std::optional<Ticks>(start()) : std::nullopt; // P.2: Hold Filters holds one curve time
    } else if (isFreeze() || isRemapped()) {
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
