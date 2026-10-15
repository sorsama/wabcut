// Time remapping operations (FinishingOps.h, TimeRemap.h). Each touches one clip: its content block's remap curve
// (copy-on-write) and, turning remapping on or off, its speed, direction and sourceIn.

#include <wabcut/model/FinishingOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>

namespace wabcut::edit {

using detail::ClipRef;
using detail::SequenceEditor;

namespace {

/// A clip on an unlocked track of the sequence.
std::expected<ClipRef, Error> editableClip(const Sequence& seq, ClipId clip) {
    const auto refs = detail::resolveClips(seq, std::span<const ClipId>(&clip, 1), LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    return refs->front();
}

Keyframe keyAt(Ticks time, Ticks position) { return Keyframe{time, static_cast<double>(position.value)}; }

std::string clipText(const Clip& c) { return "clip " + std::to_string(c.id.value) + " ('" + c.name() + "')"; }

/// Why the remapped clip `c` would show something outside its media, or nullopt.
std::optional<std::string> boundsProblem(const Clip& c, const SourceBounds& bounds) {
    const Curve& remap = *c.timeRemap();
    if (const auto problem = remapProblem(remap)) {
        return "the time remap " + *problem;
    }
    if (!checkedAdd(c.sourceIn, c.duration())) {
        return "the remapped clip's curve range exceeds the tick range";
    }
    if (bounds.start.value > 0) {
        // A restricted subclip: no key and no frame shown before its start.
        for (const Keyframe& k : remap.keys) {
            if (k.value < static_cast<double>(bounds.start.value)) {
                return "a remap key lies before the start of the subclip";
            }
        }
        if (remapShownRange(remap, c.sourceIn, c.sourceOut()).first < bounds.start) {
            return clipText(c) + " would show a frame before the start of its subclip";
        }
    }
    const std::optional<Ticks> extent = bounds.end;
    if (!extent) {
        return std::nullopt;
    }
    for (const Keyframe& k : remap.keys) {
        if (k.value > static_cast<double>(extent->value)) {
            return "a remap key lies past the end of the media";
        }
    }
    if (remapShownRange(remap, c.sourceIn, c.sourceOut()).second >= *extent) {
        return clipText(c) + " would hold a frame past the end of its media";
    }
    return std::nullopt;
}

std::expected<void, Error> checkKey(const Curve& remap, std::size_t index) {
    if (index >= remap.keys.size()) {
        return std::unexpected(detail::notFound("there is no remap key " + std::to_string(index) + " (the remap has " +
                                                std::to_string(remap.keys.size()) + ")"));
    }
    return {};
}

using RemapEdit = std::function<std::expected<Curve, Error>(const Curve& current)>;

/// Applies `edit` to a remapped clip's curve, checks the result and stores it.
/// Time an edit adds after a point of the curve (curve time of the point, ticks added; negative: removed), so the
/// captions over a remapped audio clip follow its speech (Sequence.h).
using ContentShift = std::optional<std::pair<Ticks, Ticks>>;

EditOutcome editRemap(const ProjectState& state, SequenceId sequenceId, ClipId clipId, const RemapEdit& edit,
                      const ContentShift* shift = nullptr) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto site = editableClip(**seq, clipId);
    if (!site) {
        return std::unexpected(site.error());
    }
    SequenceEditor ed(state, **seq);
    const Clip& clip = ed.track(site->track).clips[site->index];
    if (!clip.isRemapped()) {
        return std::unexpected(detail::invalidArgument("time remapping is off for " + clipText(clip)));
    }
    auto next = edit(*clip.timeRemap());
    if (!next) {
        return std::unexpected(next.error());
    }
    if (*next == *clip.timeRemap()) {
        return detail::unchanged(state);
    }
    ClipContent content = clip.content();
    content.timeRemap = std::move(*next);
    Clip candidate = clip;
    candidate.setContent(std::move(content));
    if (const auto problem = boundsProblem(candidate, ed.sourceBounds(clip))) {
        return std::unexpected(detail::invalidArgument(*problem));
    }
    ed.mutableClip(site->track, site->index) = std::move(candidate);
    if (shift && *shift && ed.track(site->track).kind == TrackKind::Audio) {
        const Clip& c = ed.track(site->track).clips[site->index];
        const Ticks point = c.start() + ((*shift)->first - c.sourceIn);
        const Ticks delta = (*shift)->second;
        if (point >= c.start() && point < c.end()) {
            if (delta.value > 0) {
                ed.captionsInsert(point, delta, c.end());
            } else if (delta.value < 0) {
                ed.captionsRemove(TimeRange::fromStartEnd(std::max(c.start(), point + delta), point), c.end());
            }
        }
    }
    return ed.finish();
}

/// The speed a constant-speed clip needs to play `source` ticks of media in `timeline` ticks like a straight
/// remap segment does: the simplest ratio (a convergent of source / timeline) whose positions over `timeline`
/// end exactly at `source`, else the closest representable one below it.
std::optional<Speed> constantSpeedFor(Ticks source, Ticks timeline) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int32_t>::max();
    std::int64_t h0 = 0;
    std::int64_t h1 = 1;
    std::int64_t k0 = 1;
    std::int64_t k1 = 0;
    std::int64_t a = source.value;
    std::int64_t b = timeline.value;
    while (b != 0) {
        const std::int64_t q = a / b;
        if ((h1 != 0 && q > (kMax - h0) / h1) || (k1 != 0 && q > (kMax - k0) / k1)) {
            break;
        }
        const std::int64_t h = q * h1 + h0;
        const std::int64_t k = q * k1 + k0;
        if (h > 0 && k > 0 && mulDiv(timeline.value, h, k, Rounding::Floor) == source.value) {
            return Speed{static_cast<std::int32_t>(h), static_cast<std::int32_t>(k)};
        }
        h0 = h1;
        h1 = h;
        k0 = k1;
        k1 = k;
        const std::int64_t r = a - q * b;
        a = b;
        b = r;
    }
    return speedForDuration(source, timeline);
}

/// Keys of `remap` before `time`, then `inserted`, then the keys after `time` moved by `shift`. A key exactly at
/// `time` is replaced (the caller derives the inserted keys from it).
std::expected<Curve, Error> spliced(const Curve& remap, Ticks time, std::vector<Keyframe> inserted, Ticks shift) {
    Curve out;
    for (const Keyframe& k : remap.keys) {
        if (k.time < time) {
            out.keys.push_back(k);
        }
    }
    out.keys.insert(out.keys.end(), inserted.begin(), inserted.end());
    for (Keyframe k : remap.keys) {
        if (k.time > time) {
            const auto shifted = checkedAdd(k.time, shift);
            if (!shifted) {
                return std::unexpected(detail::invalidArgument("the shifted remap keys exceed the tick range"));
            }
            k.time = *shifted;
            out.keys.push_back(k);
        }
    }
    return out;
}

/// The key at `time` (a copy) or a linear one, placed at `time` holding `position`.
Keyframe derivedKey(const Curve& remap, Ticks time, Ticks position) {
    Keyframe k = keyAt(time, position);
    if (const auto i = remap.keyAt(time)) {
        k = remap.keys[*i];
        k.value = static_cast<double>(position.value);
    } else if (remapSegmentAt(remap, time).motion == RemapMotion::Hold) {
        // Inside a hold (a Hold step until its next key): the new key holds on too, so what follows the inserted
        // segment still steps where the step was instead of turning into motion.
        k.interpolation = Interpolation::Hold;
    }
    return k;
}

/// The remap that plays like `clip` at its constant speed and direction, inside the clip and as far into the media as
/// that speed goes both ways (transition handles and trims read there): one straight segment whose keys lie a whole
/// number of speed denominators from the clip's start, so it matches the constant-speed mapping tick for tick. Where
/// that grid cannot reach (unknown media length, overflow) the key sits at the clip's end as before. The media usable
/// is [lower, extent): `lower` is 0, or a restricted subclip's start (M5k).
Curve constantSpeedRemap(const Clip& clip, Ticks lower, std::optional<Ticks> extent) {
    const Ticks in = clip.sourceIn; // the curve time at the clip's start (its sourceIn stays)
    if (clip.isFreeze()) {
        return Curve{{keyAt(in, in)}};
    }
    if (clip.speed.isUnity() && !clip.reversed && extent && *extent >= clip.sourceOut() && lower <= clip.sourceIn) {
        return Curve{{keyAt(lower, lower), keyAt(*extent, *extent)}};
    }
    const std::int64_t num = clip.speed.num;
    const std::int64_t den = clip.speed.den;
    // Curve ticks (a multiple of den) that move the position by at most `room`.
    const auto steps = [&](Ticks room) -> Ticks {
        if (room.value <= 0) {
            return {};
        }
        return Ticks{mulDiv(room.value / num, den, 1, Rounding::Floor).value_or(0)};
    };
    const auto moved = [&](Ticks curve) { return Ticks{curve.value / den * num}; }; // exact: curve is a multiple of den
    const Ticks duration = clip.duration();
    const Ticks curveRoom{in.value / den * den}; // curve time stays >= 0
    if (!clip.reversed) {
        const Ticks before = std::min(steps(clip.sourceIn - lower), curveRoom);
        const Ticks after = extent ? steps(*extent - clip.sourceIn) : Ticks{};
        const Keyframe start = keyAt(in - before, clip.sourceIn - moved(before));
        const Keyframe end = after >= duration ? keyAt(in + after, clip.sourceIn + moved(after))
                                               : keyAt(in + duration, clip.sourceOut());
        return Curve{{start, end}};
    }
    // Reversed: the position runs down from the source Out (at the clip's start) to the source In.
    const Ticks top = clip.sourceOut();
    const Ticks before = std::min(extent ? steps(*extent - top) : Ticks{}, curveRoom);
    const Ticks after = steps(top - lower);
    const Keyframe start = keyAt(in - before, top + moved(before));
    const Keyframe end = after >= duration ? keyAt(in + after, top - moved(after)) : keyAt(in + duration, clip.sourceIn);
    return Curve{{start, end}};
}

} // namespace

EditOutcome setTimeRemapEnabled(const ProjectState& state, SequenceId sequenceId, ClipId clipId, bool enabled) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto site = editableClip(**seq, clipId);
    if (!site) {
        return std::unexpected(site.error());
    }
    SequenceEditor ed(state, **seq);
    const Track& track = ed.track(site->track);
    const Clip& clip = track.clips[site->index];
    if (clip.isRemapped() == enabled) {
        return detail::unchanged(state);
    }
    const SourceBounds bounds = ed.sourceBounds(clip);
    const std::optional<Ticks> extent = bounds.end;
    Clip next = clip;
    ClipContent content = clip.content();
    next.speed = Speed{1, 1};
    next.reversed = false;
    if (enabled) {
        // Linked partners on the same media and in sync with this clip (its other streams) share one curve in
        // timeline terms, so key edits follow them all: the curve reaches only as far as the shortest of their streams
        // (real files rarely end their video and audio together), and turning each partner on gives the same keys.
        std::optional<Ticks> reach = extent;
        if (clip.linkGroup.isValid()) {
            (*seq)->forEachTrack([&](const Track& t) {
                for (const Clip& p : t.clips) {
                    if (p.id == clip.id || p.linkGroup != clip.linkGroup || p.media != clip.media ||
                        p.sourceIn - p.start() != clip.sourceIn - clip.start()) {
                        continue;
                    }
                    if (const auto other = ed.sourceBounds(p).end; other && (!reach || *other < *reach)) {
                        reach = other;
                    }
                }
            });
        }
        // The usable media starts at a restricted subclip's range (curve time = source time there too).
        content.timeRemap = constantSpeedRemap(clip, bounds.start, reach);
        next.setContent(std::move(content));
        if (const auto problem = boundsProblem(next, bounds)) {
            return std::unexpected(detail::invalidArgument(*problem));
        }
    } else {
        const Curve& remap = *clip.timeRemap();
        const Ticks from = clip.sourceIn;
        const RemapSegment s = remapSegmentAt(remap, from);
        const bool single = s.to >= clip.sourceOut(); // one segment shows the whole clip
        next.sourceIn = remapSourceAt(remap, from);
        if (single && s.motion == RemapMotion::Hold && track.kind == TrackKind::Video) {
            next.speed = Speed{0, 1};
            next.sourceIn = s.sourceFrom;
        } else if (single && s.straight && s.motion != RemapMotion::Hold) {
            const bool forward = s.motion == RemapMotion::Forward;
            const Ticks length = forward ? s.sourceTo - s.sourceFrom : s.sourceFrom - s.sourceTo;
            if (const auto speed = constantSpeedFor(length, s.to - s.from)) {
                const Ticks position = remapPositionAt(remap, from);
                next.speed = *speed;
                next.reversed = !forward;
                next.sourceIn = forward ? position : position - toSourceTicks(clip.duration(), *speed);
            }
        }
        content.timeRemap.reset();
        next.setContent(std::move(content));
        const bool past = extent && (next.isFreeze() ? next.sourceIn >= *extent : next.sourceOut() > *extent);
        if (next.sourceIn < bounds.start || past) {
            return std::unexpected(detail::invalidArgument(
                "turning time remapping off would play " + clipText(clip) +
                " past the ends of its media at a constant speed; trim it first"));
        }
    }
    ed.mutableClip(site->track, site->index) = std::move(next);
    return ed.finish();
}

EditOutcome setTimeRemap(const ProjectState& state, SequenceId sequenceId, ClipId clipId, Curve remap, Ticks curveIn) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto site = editableClip(**seq, clipId);
    if (!site) {
        return std::unexpected(site.error());
    }
    if (curveIn.value < 0) {
        return std::unexpected(detail::invalidArgument("a remapped clip's curve time starts at 0 or later"));
    }
    SequenceEditor ed(state, **seq);
    const Clip& clip = ed.track(site->track).clips[site->index];
    Clip next = clip;
    ClipContent content = clip.content();
    content.timeRemap = std::move(remap);
    next.setContent(std::move(content));
    next.speed = Speed{1, 1};
    next.reversed = false;
    next.sourceIn = curveIn;
    if (const auto problem = boundsProblem(next, ed.sourceBounds(clip))) {
        return std::unexpected(detail::invalidArgument(*problem));
    }
    if (next == clip) {
        return detail::unchanged(state);
    }
    ed.mutableClip(site->track, site->index) = std::move(next);
    return ed.finish();
}

EditOutcome addTimeRemapKey(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks time) {
    return editRemap(state, sequence, clip, [&](const Curve& current) -> std::expected<Curve, Error> {
        if (current.keyAt(time)) {
            return current;
        }
        Curve next = current;
        Keyframe key = keyAt(time, remapPositionAt(current, time));
        if (remapSegmentAt(current, time).motion == RemapMotion::Hold) {
            key.interpolation = Interpolation::Hold;
        }
        next.keys.insert(next.keys.begin() + static_cast<std::ptrdiff_t>(next.insertionIndex(time)),
                         key);
        return next;
    });
}

EditOutcome moveTimeRemapKey(const ProjectState& state, SequenceId sequence, ClipId clip, std::size_t index,
                             Ticks time, bool ripple) {
    ContentShift shift;
    return editRemap(state, sequence, clip, [&](const Curve& current) -> std::expected<Curve, Error> {
        if (auto ok = checkKey(current, index); !ok) {
            return std::unexpected(ok.error());
        }
        Curve next = current;
        const auto delta = checkedSub(time, current.keys[index].time);
        if (!delta) {
            return std::unexpected(detail::invalidArgument("the remap key move exceeds the tick range"));
        }
        if (index > 0 && time <= current.keys[index - 1].time) {
            return std::unexpected(detail::invalidArgument("a remap key cannot move onto or past the key before it"));
        }
        if (!ripple && index + 1 < current.keys.size() && time >= current.keys[index + 1].time) {
            return std::unexpected(detail::invalidArgument(
                "a remap key cannot move onto or past the key after it (ripple moves the later keys too)"));
        }
        for (std::size_t i = index; i < next.keys.size() && (ripple || i == index); ++i) {
            const auto shifted = checkedAdd(next.keys[i].time, *delta);
            if (!shifted) {
                return std::unexpected(detail::invalidArgument("the shifted remap keys exceed the tick range"));
            }
            next.keys[i].time = *shifted;
        }
        if (ripple) {
            shift = std::pair{current.keys[index].time, *delta};
        }
        return next;
    }, &shift);
}

EditOutcome setTimeRemapKey(const ProjectState& state, SequenceId sequence, ClipId clip, std::size_t index,
                            const TimeRemapKeyChange& change) {
    return editRemap(state, sequence, clip, [&](const Curve& current) -> std::expected<Curve, Error> {
        if (auto ok = checkKey(current, index); !ok) {
            return std::unexpected(ok.error());
        }
        Curve next = current;
        if (change.source) {
            if (change.source->value < 0 || change.source->value > kMaxRemapSource) {
                return std::unexpected(detail::invalidArgument("a remap position must lie inside the media"));
            }
            next.keys[index].value = static_cast<double>(change.source->value);
        }
        if (change.interpolation) {
            next.keys[index].interpolation = *change.interpolation;
        }
        return next;
    });
}

EditOutcome removeTimeRemapKey(const ProjectState& state, SequenceId sequence, ClipId clip, std::size_t index) {
    return editRemap(state, sequence, clip, [&](const Curve& current) -> std::expected<Curve, Error> {
        if (auto ok = checkKey(current, index); !ok) {
            return std::unexpected(ok.error());
        }
        if (current.keys.size() == 1) {
            return std::unexpected(
                detail::invalidArgument("a remap needs at least one key (turn time remapping off instead)"));
        }
        Curve next = current;
        next.keys.erase(next.keys.begin() + static_cast<std::ptrdiff_t>(index));
        return next;
    });
}

EditOutcome setTimeRemapSpeed(const ProjectState& state, SequenceId sequence, ClipId clip, std::size_t index,
                              Speed speed) {
    ContentShift shift;
    return editRemap(state, sequence, clip, [&](const Curve& current) -> std::expected<Curve, Error> {
        if (index >= current.keys.size() - 1) {
            return std::unexpected(detail::notFound("there is no remap segment " + std::to_string(index) +
                                                    " (the remap has " +
                                                    std::to_string(current.keys.size() - 1) + ")"));
        }
        if (speed.num <= 0 || speed.den <= 0) {
            return std::unexpected(detail::invalidArgument("a segment speed must be positive"));
        }
        const Keyframe& a = current.keys[index];
        const Keyframe& b = current.keys[index + 1];
        const std::int64_t source = std::llabs(static_cast<std::int64_t>(b.value - a.value));
        // A hold (equal positions, or a Hold step that jumps at its end key) shows one frame: no speed to set.
        if (source == 0 || remapSegmentAt(current, a.time).motion == RemapMotion::Hold) {
            return std::unexpected(detail::invalidArgument("remap segment " + std::to_string(index) +
                                                           " holds a frame; it has no speed (move its end key)"));
        }
        const auto length = mulDiv(source, speed.den, speed.num, Rounding::Nearest);
        if (!length) {
            return std::unexpected(detail::invalidArgument("the remap segment duration exceeds the tick range"));
        }
        const Ticks delta = Ticks{std::max<std::int64_t>(*length, 1)} - (b.time - a.time);
        Curve next = current;
        for (std::size_t i = index + 1; i < next.keys.size(); ++i) {
            const auto shifted = checkedAdd(next.keys[i].time, delta);
            if (!shifted) {
                return std::unexpected(detail::invalidArgument("the shifted remap keys exceed the tick range"));
            }
            next.keys[i].time = *shifted;
        }
        shift = std::pair{b.time, delta};
        return next;
    }, &shift);
}

EditOutcome addTimeRemapHold(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks time,
                             Ticks duration) {
    const ContentShift captions = std::pair{time, duration};
    return editRemap(state, sequence, clip, [&](const Curve& current) -> std::expected<Curve, Error> {
        if (duration.value <= 0) {
            return std::unexpected(detail::invalidArgument("a hold segment needs a positive duration"));
        }
        // Both ends hold the frame shown at `time`; a key that was there keeps its sides on both.
        const Ticks shown = remapSourceAt(current, time);
        Keyframe start = derivedKey(current, time, shown);
        Keyframe end = start;
        const auto endTime = checkedAdd(time, duration);
        if (!endTime) {
            return std::unexpected(detail::invalidArgument("the hold segment exceeds the tick range"));
        }
        end.time = *endTime;
        return spliced(current, time, {start, end}, duration);
    }, &captions);
}

EditOutcome addTimeRemapReverse(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks time,
                                Ticks duration) {
    // Overflow is refused inside; the captions only move when the edit succeeds.
    const bool doubles = duration.value > 0 && duration.value <= std::numeric_limits<std::int64_t>::max() / 2;
    const ContentShift captions = std::pair{time, Ticks{doubles ? duration.value * 2 : 0}};
    return editRemap(state, sequence, clip, [&](const Curve& current) -> std::expected<Curve, Error> {
        if (duration.value <= 0) {
            return std::unexpected(detail::invalidArgument("a reverse segment needs a positive duration"));
        }
        if (remapSegmentAt(current, time).motion == RemapMotion::Reverse) {
            return std::unexpected(detail::invalidArgument("that point already plays in reverse"));
        }
        const Ticks position = remapPositionAt(current, time);
        if (position < duration) {
            return std::unexpected(detail::invalidArgument(
                "a reverse segment needs as much media before the point as it lasts; there are only " +
                std::to_string(position.value) + " ticks"));
        }
        // Back over `duration` of media at 100 %, then forwards over it again; the key that was at `time` (if
        // any) carries on after the return.
        Keyframe start = derivedKey(current, time, position);
        if (start.interpolation == Interpolation::Hold) {
            start.interpolation = Interpolation::Linear;
        }
        const auto shift = checkedAdd(duration, duration);
        const auto turnTime = checkedAdd(time, duration);
        const auto backTime = shift ? checkedAdd(time, *shift) : std::nullopt;
        if (!turnTime || !backTime) {
            return std::unexpected(detail::invalidArgument("the reverse segment exceeds the tick range"));
        }
        const Keyframe turn = keyAt(*turnTime, position - duration);
        Keyframe back = derivedKey(current, time, position);
        back.time = *backTime;
        return spliced(current, time, {start, turn, back}, *shift);
    }, &captions);
}

} // namespace wabcut::edit
