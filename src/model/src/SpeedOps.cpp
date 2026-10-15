// Speed/Duration and frame holds.

#include <wabcut/model/FinishingOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <tuple>
#include <limits>
#include <numeric>

namespace wabcut::edit {

using detail::ClipRef;
using detail::SequenceEditor;

std::optional<Speed> speedForDuration(Ticks sourceDuration, Ticks timelineDuration) {
    if (sourceDuration.value <= 0 || timelineDuration.value <= 0) {
        return std::nullopt;
    }
    constexpr std::int64_t kMax = std::numeric_limits<std::int32_t>::max();
    std::int64_t num = sourceDuration.value;
    std::int64_t den = timelineDuration.value;
    const std::int64_t g = std::gcd(num, den);
    num /= g;
    den /= g;
    if (num <= kMax && den <= kMax) {
        return Speed{static_cast<std::int32_t>(num), static_cast<std::int32_t>(den)};
    }
    // Not representable exactly: the largest p / 2^20 not above the ratio, reduced.
    constexpr std::int64_t kScale = std::int64_t{1} << 20;
    const auto p = mulDiv(sourceDuration.value, kScale, timelineDuration.value, Rounding::Floor);
    if (!p || *p <= 0 || *p > kMax * 64) {
        return std::nullopt;
    }
    const std::int64_t r = std::gcd(*p, kScale);
    if (*p / r > kMax) {
        return std::nullopt;
    }
    return Speed{static_cast<std::int32_t>(*p / r), static_cast<std::int32_t>(kScale / r)};
}

EditOutcome setSpeed(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips,
                     const SpeedChange& change, LinkMode links) {
    if (change.speed.num <= 0 || change.speed.den <= 0) {
        return std::unexpected(detail::invalidArgument("a speed must be positive (a freeze frame is a frame hold)"));
    }
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto refs = detail::resolveClips(**seq, clips, links);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    SequenceEditor ed(state, **seq);
    const Rate rate = (*seq)->frameRate;
    const Ticks oneFrame = ed.oneFrame();

    struct Plan {
        ClipRef ref;
        Ticks oldEnd;
        Ticks newDuration;
        Ticks start;
        bool audio = false;
    };
    std::vector<Plan> plans;
    for (const ClipRef& r : *refs) {
        const Track& track = ed.track(r.track);
        const Clip& c = track.clips[r.index];
        if (c.isFreeze()) {
            return std::unexpected(detail::invalidArgument("clip " + std::to_string(c.id.value) +
                                                           " is a frame hold; it has no speed to change"));
        }
        if (c.isRemapped()) {
            return std::unexpected(detail::invalidArgument(
                "clip " + std::to_string(c.id.value) +
                " is time-remapped; change its remap, or turn time remapping off for a constant speed"));
        }
        // Same source material at the new speed, in whole frames; or (keepDuration) the same length.
        Ticks duration = change.keepDuration ? c.duration()
                                             : std::max(snapToFrame(toTimelineTicks(c.sourceDuration(), change.speed), rate),
                                                        oneFrame);
        if (const auto extent = ed.sourceBounds(c).end;
            extent && c.sourceIn + toSourceTicks(duration, change.speed) > *extent) {
            return std::unexpected(detail::invalidArgument(
                "clip " + std::to_string(c.id.value) +
                (change.keepDuration ? " would play past the end of its media at this speed"
                                     : " has too little media for one frame at this speed")));
        }
        if (!change.ripple && !change.keepDuration && duration > c.duration()) {
            // Without ripple the clip may only grow into the gap after it.
            if (r.index + 1 < track.clips.size()) {
                duration = std::min(duration, track.clips[r.index + 1].start() - c.start());
            }
        }
        plans.push_back({r, c.end(), duration, c.start(), track.kind == TrackKind::Audio});
    }
    // Later clips first, so shifting after an earlier clip moves the already retimed later ones.
    std::sort(plans.begin(), plans.end(), [](const Plan& a, const Plan& b) { return a.oldEnd > b.oldEnd; });
    std::vector<std::pair<Ticks, Ticks>> edits; // (old end, change in length), for sync-locked tracks
    for (const Plan& p : plans) {
        Clip& c = ed.mutableClip(p.ref.track, p.ref.index);
        const Ticks delta = p.newDuration - c.duration();
        c.speed = change.speed;
        c.reversed = change.reversed.value_or(c.reversed);
        c.maintainPitch = change.maintainPitch.value_or(c.maintainPitch);
        c.timelineRange.duration = p.newDuration;
        if (change.ripple && !change.keepDuration && delta.value != 0) {
            ed.shiftFrom(p.ref.track, p.oldEnd, delta);
            if (std::find(edits.begin(), edits.end(), std::pair{p.oldEnd, delta}) == edits.end()) {
                edits.emplace_back(p.oldEnd, delta);
            }
        }
    }
    // Captions follow (Sequence.h): over a retimed audio clip they scale with its speech; elsewhere they move like a
    // sync-locked track. Plans are in descending old ends, so earlier regions keep their coordinates.
    if (!change.keepDuration) {
        std::vector<std::tuple<Ticks, Ticks, Ticks>> done; // (start, old end, new length)
        for (const Plan& p : plans) {
            const auto key = std::tuple{p.start, p.oldEnd, p.newDuration};
            if (std::find(done.begin(), done.end(), key) != done.end()) {
                continue;
            }
            const bool audio = std::any_of(plans.begin(), plans.end(), [&](const Plan& q) {
                return q.audio && std::tuple{q.start, q.oldEnd, q.newDuration} == key;
            });
            const Ticks delta = p.newDuration - (p.oldEnd - p.start);
            if (audio) {
                ed.captionsStretch(TimeRange::fromStartEnd(p.start, p.oldEnd), p.newDuration, change.ripple);
            } else if (change.ripple && delta.value > 0) {
                ed.captionsInsert(p.oldEnd, delta);
            } else if (change.ripple && delta.value < 0) {
                ed.captionsRemove(TimeRange::fromStartEnd(p.oldEnd + delta, p.oldEnd));
            }
            done.push_back(key);
        }
    }
    if (change.ripple && !change.keepDuration) {
        for (const TrackId id : ed.trackIds()) {
            const bool participant =
                std::any_of(plans.begin(), plans.end(), [&](const Plan& p) { return p.ref.track == id; });
            const Track& t = ed.track(id);
            if (participant || t.locked || !t.syncLocked) {
                continue;
            }
            for (const auto& [at, delta] : edits) { // descending edit points
                if (delta.value > 0) {
                    ed.rippleInsert(id, at, delta);
                } else {
                    ed.rippleRemove(id, TimeRange::fromStartEnd(at + delta, at));
                }
            }
        }
    }
    return ed.finish();
}

EditOutcome frameHold(const ProjectState& state, SequenceId sequenceId, ClipId clipId, Ticks at,
                      std::optional<Ticks> duration) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto refs = detail::resolveClips(**seq, std::span<const ClipId>(&clipId, 1), LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    const ClipRef ref = refs->front();
    SequenceEditor ed(state, **seq);
    const Track& track = ed.track(ref.track);
    const Clip original = track.clips[ref.index];
    if (track.kind != TrackKind::Video) {
        return std::unexpected(detail::invalidArgument("a frame hold needs a video clip"));
    }
    if (!original.timelineRange.contains(at)) {
        return std::unexpected(detail::invalidArgument("the hold point lies outside the clip"));
    }
    if (original.isFreeze()) {
        return std::unexpected(detail::invalidArgument("the clip is already a frame hold"));
    }
    if (original.isRemapped()) {
        return std::unexpected(
            detail::invalidArgument("the clip is time-remapped; add a hold segment to its remap instead"));
    }
    const Ticks held = original.sourceTimeAt(at);
    const auto makeHold = [&](Clip& c) {
        c.speed = Speed{0, 1};
        c.reversed = false;
        c.sourceIn = held;
    };

    if (!duration) {
        // Add Frame Hold: the rest of the clip shows the frame at `at`.
        std::size_t index = ref.index;
        if (at > original.start()) {
            index = *ed.splitAt(ref.track, at);
        }
        makeHold(ed.mutableClip(ref.track, index));
        return ed.finish();
    }
    if (duration->value <= 0) {
        return std::unexpected(detail::invalidArgument("a frame hold segment needs a positive duration"));
    }
    // Insert Frame Hold Segment: open room at `at` like an insert edit, then place the hold there.
    for (const TrackId id : ed.trackIds()) {
        const Track& t = ed.track(id);
        if (id == ref.track || (!t.locked && t.syncLocked)) {
            ed.rippleInsert(id, at, *duration);
        }
    }
    ed.captionsInsert(at, *duration);
    Clip hold = original;
    hold.id = ed.newClipId();
    hold.timelineRange = TimeRange{at, *duration};
    hold.linkGroup = {};
    makeHold(hold);
    ed.placeNewClip(ref.track, std::move(hold), false);
    return ed.finish();
}

} // namespace wabcut::edit
