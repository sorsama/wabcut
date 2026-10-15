// Time remap curves (TimeRemap.h): segments, exact positions and the source time shown.

#include <wabcut/model/TimeRemap.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wabcut {

namespace {

constexpr Ticks kEarliest{std::numeric_limits<std::int64_t>::min()};
constexpr Ticks kLatest{std::numeric_limits<std::int64_t>::max()};

/// Curve's rule (Parameters.cpp): the sides of a segment that are straight.
bool linearLeaving(Interpolation i) {
    return i == Interpolation::Linear || i == Interpolation::Hold || i == Interpolation::EaseIn;
}
bool linearArriving(Interpolation i) {
    return i == Interpolation::Linear || i == Interpolation::Hold || i == Interpolation::EaseOut;
}

Ticks positionOf(const Keyframe& k) { return Ticks{static_cast<std::int64_t>(k.value)}; }

RemapSegment holdSegment(Ticks from, Ticks to, Ticks position) {
    return {from, to, position, position, RemapMotion::Hold, true};
}

RemapSegment segmentBetween(const Keyframe& a, const Keyframe& b) {
    RemapSegment s{a.time, b.time, positionOf(a), positionOf(b), RemapMotion::Hold, true};
    if (a.interpolation != Interpolation::Hold && s.sourceTo != s.sourceFrom) {
        s.motion = s.sourceTo > s.sourceFrom ? RemapMotion::Forward : RemapMotion::Reverse;
        s.straight = linearLeaving(a.interpolation) && linearArriving(b.interpolation);
    }
    return s;
}

/// The position at curve time `c` inside segment `s` (s.from <= c < s.to).
Ticks positionIn(const Curve& remap, const RemapSegment& s, Ticks c) {
    if (s.motion == RemapMotion::Hold || c <= s.from) {
        return s.sourceFrom;
    }
    const std::int64_t elapsed = (c - s.from).value;
    const std::int64_t length = (s.to - s.from).value;
    if (s.straight) {
        // Exact: rounded toward the segment's first key, so it never reaches the next key's position before it.
        if (s.motion == RemapMotion::Forward) {
            return s.sourceFrom +
                   Ticks{mulDiv(elapsed, (s.sourceTo - s.sourceFrom).value, length, Rounding::Floor).value_or(0)};
        }
        return s.sourceFrom -
               Ticks{mulDiv(elapsed, (s.sourceFrom - s.sourceTo).value, length, Rounding::Floor).value_or(0)};
    }
    // A ramp: the curve's value (monotonic: remapProblem checks the controls), rounded toward the first key and
    // kept strictly before the second.
    const double v = remap.valueAt(c);
    if (s.motion == RemapMotion::Forward) {
        const auto p = static_cast<std::int64_t>(std::floor(v));
        return Ticks{std::clamp(p, s.sourceFrom.value, s.sourceTo.value - 1)};
    }
    const auto p = static_cast<std::int64_t>(std::ceil(v));
    return Ticks{std::clamp(p, s.sourceTo.value + 1, s.sourceFrom.value)};
}

Ticks shownIn(const Curve& remap, const RemapSegment& s, Ticks c) {
    const Ticks p = positionIn(remap, s, c);
    return s.motion == RemapMotion::Reverse ? p - Ticks{1} : p;
}

} // namespace

std::vector<RemapSegment> remapSegments(const Curve& remap) {
    std::vector<RemapSegment> out;
    if (remap.keys.empty()) {
        return out;
    }
    out.reserve(remap.keys.size() + 1);
    out.push_back(holdSegment(kEarliest, remap.keys.front().time, positionOf(remap.keys.front())));
    for (std::size_t i = 0; i + 1 < remap.keys.size(); ++i) {
        out.push_back(segmentBetween(remap.keys[i], remap.keys[i + 1]));
    }
    out.push_back(holdSegment(remap.keys.back().time, kLatest, positionOf(remap.keys.back())));
    return out;
}

RemapSegment remapSegmentAt(const Curve& remap, Ticks c) {
    if (remap.keys.empty()) {
        return holdSegment(kEarliest, kLatest, Ticks{});
    }
    if (c < remap.keys.front().time) {
        return holdSegment(kEarliest, remap.keys.front().time, positionOf(remap.keys.front()));
    }
    if (c >= remap.keys.back().time) {
        return holdSegment(remap.keys.back().time, kLatest, positionOf(remap.keys.back()));
    }
    const auto next = std::upper_bound(remap.keys.begin(), remap.keys.end(), c,
                                       [](Ticks t, const Keyframe& k) { return t < k.time; });
    return segmentBetween(*(next - 1), *next);
}

Ticks remapPositionAt(const Curve& remap, Ticks c) { return positionIn(remap, remapSegmentAt(remap, c), c); }

Ticks remapSourceAt(const Curve& remap, Ticks c) { return shownIn(remap, remapSegmentAt(remap, c), c); }

std::pair<Ticks, Ticks> remapShownRange(const Curve& remap, Ticks from, Ticks to) {
    Ticks lo = kLatest;
    Ticks hi = kEarliest;
    for (const RemapSegment& s : remapSegments(remap)) {
        const Ticks a = std::max(from, s.from);
        const Ticks b = std::min(to, s.to);
        if (a >= b) {
            continue;
        }
        // Each segment is monotonic, so its ends bound what it shows.
        for (const Ticks c : {a, b - Ticks{1}}) {
            const Ticks shown = shownIn(remap, s, c);
            lo = std::min(lo, shown);
            hi = std::max(hi, shown);
        }
    }
    return {lo, hi};
}

std::optional<std::string> remapProblem(const Curve& remap) {
    if (remap.keys.empty()) {
        return "has no keys";
    }
    if (remap.keys.size() > kMaxRemapKeys) {
        return "has more than " + std::to_string(kMaxRemapKeys) + " keys";
    }
    if (!remap.isWellFormed()) {
        return "needs keys at increasing times with finite Bezier handles";
    }
    for (const Keyframe& k : remap.keys) {
        if (!std::isfinite(k.value) || k.value != std::floor(k.value) || k.value < 0.0 ||
            k.value > static_cast<double>(kMaxRemapSource)) {
            return "holds a source position that is not a whole number of ticks from 0 to 2^53";
        }
    }
    for (std::size_t i = 0; i + 1 < remap.keys.size(); ++i) {
        const Keyframe& a = remap.keys[i];
        const Keyframe& b = remap.keys[i + 1];
        if (!checkedSub(b.time, a.time)) {
            return "has a segment whose duration exceeds the tick range";
        }
        const RemapSegment s = segmentBetween(a, b);
        if (s.motion == RemapMotion::Hold || s.straight) {
            continue;
        }
        // The value controls of the ramp's cubic (Curve::valueAt) must run one way: then so does the ramp.
        const double seconds = static_cast<double>((b.time - a.time).value) / static_cast<double>(kTicksPerSecond);
        const double third = (b.value - a.value) / 3.0;
        double out = third;
        if (a.interpolation == Interpolation::EaseOut || a.interpolation == Interpolation::EaseInOut) {
            out = 0.0;
        } else if (a.interpolation == Interpolation::Bezier) {
            out = a.out.slope * std::clamp(a.out.influence, 0.0, 1.0) * seconds;
        }
        double in = third;
        if (b.interpolation == Interpolation::EaseIn || b.interpolation == Interpolation::EaseInOut) {
            in = 0.0;
        } else if (b.interpolation == Interpolation::Bezier) {
            in = b.in.slope * std::clamp(b.in.influence, 0.0, 1.0) * seconds;
        }
        const double p1 = a.value + out;
        const double p2 = b.value - in;
        const bool monotonic = s.motion == RemapMotion::Forward ? (a.value <= p1 && p1 <= p2 && p2 <= b.value)
                                                                : (a.value >= p1 && p1 >= p2 && p2 >= b.value);
        if (!monotonic) {
            return "has a speed ramp whose Bezier handles would turn it back (key " + std::to_string(i) + ")";
        }
    }
    return std::nullopt;
}

} // namespace wabcut
