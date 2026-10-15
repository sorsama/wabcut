#include <wabcut/audio/TransportMap.h>

#include <algorithm>
#include <limits>

namespace wabcut::audio {

using timemath::addSaturate;
using timemath::mulDivSaturate;

namespace {

/// a mod b in [0, b) for b > 0.
std::int64_t floorMod(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t r = a % b;
    return r < 0 ? r + b : r;
}

bool loopActive(const TransportMap::Segment& s) noexcept { return s.loopEnd > s.loopStart; }

/// Apply the segment's loop to an unwrapped timeline value.
std::int64_t wrap(const TransportMap::Segment& s, std::int64_t raw) noexcept {
    if (!loopActive(s) || s.speedNum == 0) {
        return raw;
    }
    const std::int64_t length = s.loopEnd - s.loopStart;
    if (s.speedNum > 0 && s.timeline < s.loopEnd && raw >= s.loopEnd) {
        return s.loopStart + floorMod(raw - s.loopStart, length);
    }
    if (s.speedNum < 0 && s.timeline >= s.loopStart && raw < s.loopStart) {
        return s.loopStart + floorMod(raw - s.loopStart, length);
    }
    return raw;
}

} // namespace

TransportMap::TransportMap(Rate sampleRate, Ticks position) noexcept
    : m_rate(sampleRate.isExact() ? sampleRate : kRate48kHz), m_ticksPerFrame(m_rate.ticksPerUnit()) {
    m_segments[0] = makeSegment(0, position.value, TransportSpeed::zero());
    m_count = 1;
}

// ---- Commands ----------------------------------------------------------------------------------------------

TransportMap::Segment TransportMap::makeSegment(std::int64_t atFrame, std::int64_t timeline,
                                                TransportSpeed speed) const noexcept {
    const TransportSpeed s = speed.normalized();
    return {atFrame, timeline, s.num, s.den, m_loopStart, m_loopEnd};
}

void TransportMap::push(const Segment& segment) noexcept {
    // Segments at or after the new one are superseded (same-frame changes, a pending preroll end).
    while (m_count > 0 && m_segments[m_count - 1].deviceFrame >= segment.deviceFrame) {
        --m_count;
    }
    if (m_count == kMaxSegments) {
        // More transport changes inside one output latency than we keep history for: the oldest segment's
        // start then also covers the frames before it (the clock briefly holds; it never goes backwards).
        std::copy(m_segments + 1, m_segments + kMaxSegments, m_segments);
        --m_count;
    }
    m_segments[m_count++] = segment;
}

void TransportMap::play(std::int64_t atFrame) noexcept {
    if (m_playing) {
        return;
    }
    const Ticks position = timelineAtFrame(atFrame);
    m_playing = true;
    push(makeSegment(atFrame, position.value, m_playSpeed));
}

void TransportMap::pause(std::int64_t atFrame) noexcept {
    if (!m_playing) {
        return;
    }
    const Ticks position = timelineAtFrame(atFrame);
    m_playing = false;
    push(makeSegment(atFrame, position.value, TransportSpeed::zero())); // also cancels a pending preroll end
}

void TransportMap::setSpeed(TransportSpeed speed, std::int64_t atFrame) noexcept {
    if (!speed.isValid()) {
        return;
    }
    const TransportSpeed s = speed.normalized();
    const bool changed = !(s == m_playSpeed);
    m_playSpeed = s;
    if (!m_playing || !changed) {
        return;
    }
    const int i = indexFor(atFrame);
    const bool prerollPending = i >= 0 && i + 1 < m_count && m_segments[i].speedNum == 0;
    if (prerollPending) {
        for (int k = i + 1; k < m_count; ++k) { // keep the preroll; the pending play segment adopts the speed
            m_segments[k].speedNum = s.num;
            m_segments[k].speedDen = s.den;
        }
        return;
    }
    const Ticks position = timelineAtFrame(atFrame);
    push(makeSegment(atFrame, position.value, s));
}

void TransportMap::seek(Ticks target, std::int64_t atFrame, std::int64_t prerollFrames) noexcept {
    m_count = 0; // flush: nothing before the seek maps to the old timeline any more
    if (m_playing && prerollFrames > 0 && !m_playSpeed.isZero()) {
        push(makeSegment(atFrame, target.value, TransportSpeed::zero()));
        push(makeSegment(addSaturate(atFrame, prerollFrames), target.value, m_playSpeed));
    } else {
        push(makeSegment(atFrame, target.value, effectiveSpeed()));
    }
}

bool TransportMap::prerollHolding(std::int64_t frame) const noexcept {
    const int i = indexFor(frame);
    if (!m_playing || i < 0 || i + 1 >= m_count) {
        return false;
    }
    const Segment& hold = m_segments[i];
    const Segment& next = m_segments[i + 1];
    return hold.speedNum == 0 && next.speedNum != 0 && next.deviceFrame > frame && next.timeline == hold.timeline;
}

bool TransportMap::endPreroll(std::int64_t atFrame) noexcept {
    if (!prerollHolding(atFrame)) {
        return false;
    }
    const int i = indexFor(atFrame);
    Segment future[kMaxSegments];
    int futureCount = 0;
    for (int k = i + 1; k < m_count; ++k) { // the play segment and anything scheduled after it
        future[futureCount++] = m_segments[k];
    }
    future[0].deviceFrame = atFrame;
    for (int k = 0; k < futureCount; ++k) {
        push(future[k]); // supersedes the hold where it starts at atFrame; later segments keep their frames
    }
    return true;
}

void TransportMap::setLoop(std::optional<TimeRange> range, std::int64_t atFrame) noexcept {
    const bool active = range.has_value() && !range->isEmpty();
    m_loopStart = active ? range->start.value : 0;
    m_loopEnd = active ? range->end().value : 0;

    const int i = indexFor(atFrame);
    const Segment current = m_segments[std::max(i, 0)];
    Segment future[kMaxSegments];
    int futureCount = 0;
    for (int k = i + 1; k < m_count; ++k) { // pending segments (a preroll end) keep their timing, adopt the loop
        future[futureCount] = m_segments[k];
        future[futureCount].loopStart = m_loopStart;
        future[futureCount].loopEnd = m_loopEnd;
        ++futureCount;
    }
    const Ticks position = timelineAtFrame(atFrame);
    push(makeSegment(atFrame, position.value, TransportSpeed{current.speedNum, current.speedDen}));
    for (int k = 0; k < futureCount; ++k) {
        push(future[k]);
    }
}

void TransportMap::rebase(std::uint32_t newEpoch, std::int64_t oldFrame, std::int64_t newFrame) noexcept {
    // The segment in effect at `oldFrame` continues at `newFrame` with its speed and loop (a preroll's speed 0
    // included), and segments still pending after `oldFrame` (a preroll's end) keep their distance from it, so a
    // rebase in the middle of a preroll neither starts playback early nor loses it. History before `oldFrame`
    // is dropped: those frames belong to the old device.
    const int i = indexFor(oldFrame);
    Segment current = i >= 0 ? m_segments[i] : makeSegment(oldFrame, m_segments[0].timeline, TransportSpeed::zero());
    current.deviceFrame = newFrame;
    current.timeline = timelineAtFrame(oldFrame).value;
    Segment pending[kMaxSegments];
    int pendingCount = 0;
    const std::int64_t shift = newFrame - oldFrame;
    for (int k = i + 1; k < m_count; ++k) {
        pending[pendingCount] = m_segments[k];
        pending[pendingCount].deviceFrame = addSaturate(m_segments[k].deviceFrame, shift);
        ++pendingCount;
    }
    m_epoch = newEpoch;
    m_count = 0;
    push(current);
    for (int k = 0; k < pendingCount; ++k) {
        push(pending[k]);
    }
    m_hasClock = false;
    m_clock = {};
}

void TransportMap::setClock(const ClockSample& clock) noexcept {
    m_clock = clock;
    m_hasClock = clock.running;
}

// ---- State -------------------------------------------------------------------------------------------------

TransportSpeed TransportMap::speedAtFrame(std::int64_t frame) const noexcept {
    const int i = indexFor(frame);
    if (i < 0) {
        return TransportSpeed::zero();
    }
    return {m_segments[i].speedNum, m_segments[i].speedDen};
}

std::optional<TimeRange> TransportMap::loop() const noexcept {
    if (m_loopEnd <= m_loopStart) {
        return std::nullopt;
    }
    return TimeRange::fromStartEnd(Ticks{m_loopStart}, Ticks{m_loopEnd});
}

// ---- Mapping -----------------------------------------------------------------------------------------------

int TransportMap::indexFor(std::int64_t frame) const noexcept {
    int found = -1;
    for (int i = 0; i < m_count; ++i) {
        if (m_segments[i].deviceFrame > frame) {
            break;
        }
        found = i;
    }
    return found;
}

std::int64_t TransportMap::frameTicks(std::int64_t frame) const noexcept {
    return mulDivSaturate(frame, m_ticksPerFrame, 1, Rounding::Floor);
}

std::int64_t TransportMap::evaluate(const Segment& s, std::int64_t offsetTicks) const noexcept {
    const std::int64_t scaled = mulDivSaturate(offsetTicks, s.speedNum, s.speedDen, Rounding::Floor);
    return wrap(s, addSaturate(s.timeline, scaled));
}

Ticks TransportMap::timelineAtFrame(std::int64_t frame) const noexcept {
    return timelineAtDeviceTicks(frameTicks(frame));
}

Ticks TransportMap::timelineAtDeviceTicks(std::int64_t deviceTicks) const noexcept {
    const int i = indexFor(floorDiv(deviceTicks, m_ticksPerFrame));
    if (i < 0) {
        return Ticks{m_segments[0].timeline}; // before the history: hold at its start
    }
    const Segment& s = m_segments[i];
    return Ticks{evaluate(s, deviceTicks - frameTicks(s.deviceFrame))};
}

TransportMap::Piece TransportMap::piece(std::int64_t frame, std::int64_t maxFrames) const noexcept {
    std::int64_t limit = std::max<std::int64_t>(maxFrames, 1);
    const int i = indexFor(frame);
    if (i < 0) {
        limit = std::min(limit, m_segments[0].deviceFrame - frame);
        return {Ticks{m_segments[0].timeline}, TransportSpeed::zero(), std::max<std::int64_t>(limit, 1)};
    }
    const Segment& s = m_segments[i];
    if (i + 1 < m_count) {
        limit = std::min(limit, m_segments[i + 1].deviceFrame - frame);
    }
    const std::int64_t offset = frameTicks(frame) - frameTicks(s.deviceFrame);
    const TransportSpeed speed{s.speedNum, s.speedDen};

    if (loopActive(s) && s.speedNum != 0) {
        // Frames until the next wrap. The unwrapped timeline at frame offset m is T0 + floor(m·tpf·num/den).
        const std::int64_t length = s.loopEnd - s.loopStart;
        const std::int64_t raw = addSaturate(s.timeline, mulDivSaturate(offset, s.speedNum, s.speedDen, Rounding::Floor));
        const std::int64_t perFrame = m_ticksPerFrame * (s.speedNum < 0 ? -std::int64_t{s.speedNum} : s.speedNum);
        std::int64_t wrapOffset = -1; // frames from the segment start to the first wrapped frame
        if (s.speedNum > 0 && s.timeline < s.loopEnd) {
            // First m with raw(m) >= B, where B is the end of the current lap: m = ceil((B - T0)·den / (tpf·num)).
            const std::int64_t lapEnd =
                raw < s.loopEnd ? s.loopEnd : s.loopStart + (floorDiv(raw - s.loopStart, length) + 1) * length;
            wrapOffset = mulDivSaturate(lapEnd - s.timeline, s.speedDen, perFrame, Rounding::Ceil);
        } else if (s.speedNum < 0 && s.timeline >= s.loopStart) {
            // First m with raw(m) < B, where B is the start of the current lap: m = floor((T0 - B)·den / (tpf·|num|)) + 1.
            const std::int64_t lapStart =
                raw >= s.loopStart ? s.loopStart : s.loopStart - (floorDiv(s.loopStart - 1 - raw, length) + 1) * length;
            wrapOffset = mulDivSaturate(s.timeline - lapStart, s.speedDen, perFrame, Rounding::Floor) + 1;
        }
        if (wrapOffset >= 0) {
            const std::int64_t toWrap = s.deviceFrame + wrapOffset - frame;
            if (toWrap > 0) {
                limit = std::min(limit, toWrap);
            }
        }
    }
    return {Ticks{evaluate(s, offset)}, speed, std::max<std::int64_t>(limit, 1)};
}

Ticks TransportMap::transportTime(std::int64_t hostTimeNs) const noexcept {
    if (!m_hasClock) {
        return timelineAtFrame(m_segments[m_count - 1].deviceFrame);
    }
    return timelineAtDeviceTicks(m_clock.presentedDeviceTicksAt(hostTimeNs));
}

std::optional<std::int64_t> TransportMap::deviceTicksFor(Ticks timeline) const noexcept {
    const Segment& s = m_segments[m_count - 1];
    const std::int64_t start = frameTicks(s.deviceFrame);
    const std::int64_t target = timeline.value;
    if (s.speedNum == 0) {
        return target == s.timeline ? std::optional<std::int64_t>(start) : std::nullopt;
    }
    std::int64_t raw = target; // the unwrapped timeline value to reach
    if (s.speedNum > 0) {
        if (loopActive(s) && s.timeline < s.loopEnd) {
            if (target >= s.loopEnd) {
                return std::nullopt; // wraps before getting there
            }
            if (target < s.timeline) {
                if (target < s.loopStart) {
                    return std::nullopt;
                }
                raw = s.loopEnd + (target - s.loopStart); // next lap
            }
        } else if (target < s.timeline) {
            return std::nullopt;
        }
        // First offset with T0 + floor(offset·num/den) >= raw.
        const auto offset = mulDiv(raw - s.timeline, s.speedDen, s.speedNum, Rounding::Ceil);
        if (!offset) {
            return std::nullopt;
        }
        return addSaturate(start, *offset);
    }
    if (loopActive(s) && s.timeline >= s.loopStart) {
        if (target < s.loopStart || target >= s.loopEnd) {
            return std::nullopt;
        }
        if (target > s.timeline) {
            raw = s.loopStart - (s.loopEnd - target); // next lap below
        }
    } else if (target > s.timeline) {
        return std::nullopt;
    }
    // Reverse: first offset with T0 + floor(offset·num/den) <= raw, i.e. offset > (T0 - raw - 1)·den/|num|.
    const auto bound = mulDiv(s.timeline - raw - 1, s.speedDen, -std::int64_t{s.speedNum}, Rounding::Floor);
    if (!bound) {
        return std::nullopt;
    }
    return addSaturate(start, std::max<std::int64_t>(*bound + 1, 0));
}

std::optional<std::int64_t> TransportMap::hostTimeFor(Ticks timeline) const noexcept {
    if (!m_hasClock) {
        return std::nullopt;
    }
    const auto deviceTicks = deviceTicksFor(timeline);
    if (!deviceTicks) {
        return std::nullopt;
    }
    return m_clock.hostTimeForDeviceTicks(*deviceTicks);
}

} // namespace wabcut::audio
