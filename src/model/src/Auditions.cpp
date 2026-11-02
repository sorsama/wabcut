// Auditions (0.6, card #74): take stacks at a cut. See Auditions.h for the model and docs/editing.md "Auditions".

#include <wabcut/model/Auditions.h>

#include <wabcut/model/EditingOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/TimeRemap.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>

namespace wabcut {

using edit::detail::ClipRef;
using edit::detail::invalidArgument;
using edit::detail::notFound;
using edit::detail::SequenceEditor;
using edit::detail::trackLabel;

const ClipContent& AuditionTake::contentOrDefault() const {
    static const ClipContent kDefault;
    return content ? *content : kDefault;
}

bool AuditionTake::operator==(const AuditionTake& o) const {
    return media == o.media && stream == o.stream && sequence == o.sequence && angles == o.angles &&
           sourceIn == o.sourceIn && speed == o.speed && reversed == o.reversed && maintainPitch == o.maintainPitch &&
           (content == o.content || contentOrDefault() == o.contentOrDefault());
}

const Audition* auditionOf(const Clip& clip) { return clip.audition(); }

std::size_t takeCount(const Clip& clip) {
    const Audition* a = clip.audition();
    return a ? a->size() : 1;
}

TimeRange auditionRange(const Clip& clip) {
    const Audition* a = clip.audition();
    if (!a) {
        return clip.timelineRange;
    }
    return TimeRange::fromStartEnd(clip.start() - a->headTrim, clip.end() + a->tailTrim);
}

Clip takeClip(const AuditionTake& take, TimeRange range) {
    Clip v;
    v.media = take.media;
    v.stream = take.stream;
    v.sequence = take.sequence;
    v.angles = take.angles;
    v.timelineRange = range;
    v.sourceIn = take.sourceIn;
    v.speed = take.speed;
    v.reversed = take.reversed;
    v.maintainPitch = take.maintainPitch;
    v.setContent(take.content);
    return v;
}

AuditionTake takeFromClip(const Clip& clip) {
    AuditionTake t;
    t.media = clip.media;
    t.stream = clip.stream;
    t.sequence = clip.sequence;
    t.angles = clip.angles;
    t.sourceIn = clip.sourceIn;
    t.speed = clip.speed;
    t.reversed = clip.reversed;
    t.maintainPitch = clip.maintainPitch;
    if (clip.isAudition()) {
        ClipContent block = clip.content();
        block.audition = nullptr;
        t.content = std::make_shared<const ClipContent>(std::move(block));
    } else {
        t.content = clip.sharedContent();
    }
    return t;
}

namespace {

/// The clip without its take stack (the active take alone).
Clip withoutAudition(Clip clip) {
    if (clip.isAudition()) {
        ClipContent block = clip.content();
        block.audition = nullptr;
        if (block == ClipContent{}) {
            clip.setContent(std::shared_ptr<const ClipContent>{});
        } else {
            clip.setContent(std::move(block));
        }
    }
    return clip;
}

/// `clip` with `audition` as its take stack (null: none).
Clip withAudition(Clip clip, std::optional<Audition> audition) {
    ClipContent block = clip.content();
    block.audition = audition ? std::make_shared<const Audition>(std::move(*audition)) : nullptr;
    clip.setContent(std::move(block));
    return clip;
}

/// The active take as it would play over the audition's whole range: the exact inverse of trimming a take to the part
/// it covers (fitTake: the head first, then the tail; here the tail first, then the head).
Clip widened(const Clip& clip) {
    Clip v = withoutAudition(clip);
    const Audition* a = clip.audition();
    if (!a) {
        return v;
    }
    if (a->tailTrim.value > 0) {
        v.moveTail(a->tailTrim); // forward: the length only; reversed: keeps the top (exact both ways)
    }
    if (a->headTrim.value > 0) {
        if (!v.reversed && !v.isFreeze()) {
            v.sourceIn -= toSourceTicks(a->headTrim, v.speed); // moveHead(h) adds exactly this back
        }
        v.timelineRange.start -= a->headTrim;
        v.timelineRange.duration += a->headTrim;
    }
    return v;
}

/// Every take of the clip in take order, the active one widened to the audition's range.
std::vector<AuditionTake> allTakes(const Clip& clip) {
    const Audition* a = clip.audition();
    std::vector<AuditionTake> out = a ? a->others : std::vector<AuditionTake>{};
    const std::size_t at = a ? std::min<std::size_t>(a->active, out.size()) : 0;
    out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), takeFromClip(widened(clip)));
    return out;
}

/// The media range a take may use (SequenceEditor::sourceBounds without an editor).
SourceBounds boundsOf(const ProjectState& state, const Clip& clip) {
    if (!clip.isNested() && !clip.hasUnknownSource()) {
        if (const MediaItem* media = state.findMedia(clip.media)) {
            return media->sourceBounds(clip.stream);
        }
    }
    return SourceBounds{Ticks{}, clipSourceExtent(state, clip)};
}

/// `t` rounded up to whole frames of `frame` ticks (t > 0).
Ticks ceilToFrames(Ticks t, Ticks frame) {
    if (t.value <= 0 || frame.value <= 0) {
        return std::max(t, Ticks{});
    }
    const std::int64_t frames = t.value / frame.value + (t.value % frame.value != 0 ? 1 : 0);
    return Ticks{frames * frame.value};
}

/// How much of `whole` (a take over the audition's range) lies outside its media at each end, in frames.
TakeFit shortfall(const ProjectState& state, const Clip& whole, Ticks frame) {
    TakeFit fit;
    const SourceBounds bounds = boundsOf(state, whole);
    Clip v = whole;
    // A few rounds: the speed's rounding can leave a tick over after the first trim.
    for (int round = 0; round < 4 && v.duration().value > 0; ++round) {
        Ticks head;
        Ticks tail;
        if (const auto room = v.headRoom(bounds.start, bounds.end); room && room->value < 0) {
            head = ceilToFrames(Ticks{-room->value}, frame);
        }
        if (const auto room = v.tailRoom(bounds.start, bounds.end); room && room->value < 0) {
            tail = ceilToFrames(Ticks{-room->value}, frame);
        }
        if (head.value == 0 && tail.value == 0) {
            break;
        }
        if (head.value + tail.value >= v.duration().value) {
            fit.head = whole.duration(); // nothing left: the whole range is short
            fit.tail = Ticks{};
            return fit;
        }
        if (head.value > 0) {
            v.moveHead(head);
            fit.head = fit.head + head;
        }
        if (tail.value > 0) {
            v.moveTail(Ticks{-tail.value});
            fit.tail = fit.tail + tail;
        }
    }
    return fit;
}

/// A take over the audition's range `whole`, trimmed to its media and to the free part of the track [freeFrom, freeTo).
struct Fitted {
    Clip clip;
    Ticks head;
    Ticks tail;
};

std::expected<Fitted, Error> fitTake(const ProjectState& state, Clip whole, Ticks frame, Ticks freeFrom, Ticks freeTo,
                                     const std::string& what) {
    TakeFit fit = shortfall(state, whole, frame);
    fit.head = std::max(fit.head, freeFrom - whole.start());
    fit.tail = std::max(fit.tail, whole.end() - freeTo);
    if (fit.head.value + fit.tail.value >= whole.duration().value) {
        return std::unexpected(invalidArgument(what + " has no media inside the audition's range, or no room there"));
    }
    Fitted out{std::move(whole), std::max(fit.head, Ticks{}), std::max(fit.tail, Ticks{})};
    if (out.head.value > 0) {
        out.clip.moveHead(out.head);
    }
    if (out.tail.value > 0) {
        out.clip.moveTail(Ticks{-out.tail.value});
    }
    if (const auto problem = clipBoundsProblem(state, out.clip)) {
        return std::unexpected(invalidArgument(what + " " + *problem));
    }
    return out;
}

/// The free part of the clip's track around it: from the end of the clip before it to the start of the one after.
std::pair<Ticks, Ticks> freeAround(const Track& track, std::size_t index) {
    const Ticks from = index > 0 ? track.clips[index - 1].end() : Ticks{};
    const Ticks to = index + 1 < track.clips.size() ? track.clips[index + 1].start() : Ticks::max();
    return {from, to};
}

/// "Take 3 ('Wide')".
std::string takeLabel(std::size_t index, const std::string& name) {
    std::string out = "take " + std::to_string(index + 1);
    if (!name.empty()) {
        out += " ('" + name + "')";
    }
    return out;
}

/// The clip `c` (at `index` on `track`) with take `k` of `takes` active over `whole`, keeping its id, links and colour.
std::expected<Clip, Error> activate(const ProjectState& state, const Track& track, std::size_t index, const Clip& c,
                                    std::vector<AuditionTake> takes, std::size_t k, TimeRange whole, Ticks frame) {
    const auto [freeFrom, freeTo] = freeAround(track, index);
    auto fitted = fitTake(state, takeClip(takes[k], whole), frame, freeFrom, freeTo, takeLabel(k, takes[k].name()));
    if (!fitted) {
        return std::unexpected(fitted.error());
    }
    Clip out = std::move(fitted->clip);
    out.id = c.id;
    out.linkGroup = c.linkGroup;
    out.group = c.group;
    out.colorIndex = c.colorIndex;
    out.enabled = c.enabled;
    if (takes.size() < 2) {
        return withoutAudition(std::move(out));
    }
    Audition a;
    a.active = static_cast<std::uint32_t>(k);
    a.headTrim = fitted->head;
    a.tailTrim = fitted->tail;
    for (std::size_t i = 0; i < takes.size(); ++i) {
        if (i != k) {
            a.others.push_back(std::move(takes[i]));
        }
    }
    return withAudition(std::move(out), std::move(a));
}

/// The clip with `takes` (take order, the active one at `active`) as its stack, keeping the clip as it is: the active
/// take is the clip itself, so only the other takes are stored.
Clip restacked(const Clip& clip, std::vector<AuditionTake> takes, std::size_t active, Ticks headTrim, Ticks tailTrim) {
    if (takes.size() < 2) {
        return withoutAudition(clip);
    }
    Audition a;
    a.active = static_cast<std::uint32_t>(active);
    a.headTrim = headTrim;
    a.tailTrim = tailTrim;
    for (std::size_t i = 0; i < takes.size(); ++i) {
        if (i != active) {
            a.others.push_back(std::move(takes[i]));
        }
    }
    return withAudition(withoutAudition(clip), std::move(a));
}

bool trackOrderLess(const Sequence& seq, TrackId a, TrackId b) {
    const auto pa = seq.trackPosition(a);
    const auto pb = seq.trackPosition(b);
    if (!pa || !pb) {
        return a.value < b.value;
    }
    if (pa->kind != pb->kind) {
        return pa->kind == TrackKind::Video;
    }
    return pa->index < pb->index;
}

/// The clips of one audition (the named clip and its audition partners) as editor references, in track order (video
/// first). Refuses a clip that is not there, not an audition (unless `allowPlain`) or on a locked track.
std::expected<std::vector<ClipRef>, Error> auditionLanes(const Sequence& seq, ClipId clip, bool allowPlain) {
    const auto loc = seq.findClip(clip);
    if (!loc) {
        return std::unexpected(notFound("clip " + std::to_string(clip.value) + " is not in this sequence"));
    }
    if (!allowPlain && !loc->clip().isAudition()) {
        return std::unexpected(invalidArgument("clip '" + loc->clip().name() + "' is not an audition"));
    }
    if (loc->track->locked) {
        return std::unexpected(invalidArgument("track " + trackLabel(seq, loc->track->id) + " is locked"));
    }
    std::vector<ClipRef> out;
    for (const ClipId id : auditionPartners(seq, clip)) {
        const auto at = seq.findClip(id);
        if (at) {
            out.push_back({at->track->id, id, at->index});
        }
    }
    std::stable_sort(out.begin(), out.end(), [&](const ClipRef& a, const ClipRef& b) {
        return trackOrderLess(seq, a.track, b.track);
    });
    return out;
}

/// New effect ids for a copied take's effects (Duplicate Take).
std::shared_ptr<const ClipContent> withNewEffectIds(const std::shared_ptr<const ClipContent>& content,
                                                    SequenceEditor& ed, const std::string& name) {
    ClipContent block = content ? *content : ClipContent{};
    for (EffectInstance& e : block.effects) {
        e.id = ed.newEffectId();
    }
    block.name = name;
    block.audition = nullptr;
    return std::make_shared<const ClipContent>(std::move(block));
}

/// "<name> copy", kept inside kMaxTakeNameBytes.
std::string copyName(const std::string& name) {
    std::string base = name.empty() ? std::string("Take") : name;
    if (base.size() + 5 > kMaxTakeNameBytes) {
        base.resize(kMaxTakeNameBytes - 5);
    }
    return base + " copy";
}

} // namespace

const std::string& takeName(const Clip& clip, std::size_t index) {
    const Audition* a = clip.audition();
    if (!a || index == a->active || index >= a->size()) {
        return clip.name();
    }
    return a->others[index < a->active ? index : index - 1].name();
}

Clip auditionTakeClip(const Clip& clip, std::size_t index) {
    const Audition* a = clip.audition();
    if (!a || index == a->active || index >= a->size()) {
        return widened(clip);
    }
    Clip v = takeClip(a->others[index < a->active ? index : index - 1], auditionRange(clip));
    v.id = clip.id;
    v.linkGroup = clip.linkGroup;
    v.group = clip.group;
    v.colorIndex = clip.colorIndex;
    v.enabled = clip.enabled;
    return v;
}

bool clipUsesMedia(const Clip& clip, MediaId media) {
    bool used = false;
    forEachTakeSource(clip, [&](MediaId m, SequenceId) { used = used || (m.isValid() && m == media); });
    return used;
}

bool clipUsesSequence(const Clip& clip, SequenceId sequence) {
    bool used = false;
    forEachTakeSource(clip, [&](MediaId, SequenceId s) { used = used || (s.isValid() && s == sequence); });
    return used;
}

std::optional<std::string> auditionProblem(const ProjectState& state, StreamKind kind, const Clip& clip) {
    const Audition* a = clip.audition();
    if (!a) {
        return std::nullopt;
    }
    if (a->others.empty()) {
        return std::string("is an audition with only one take");
    }
    if (a->size() > kMaxAuditionTakes) {
        return "is an audition with more than " + std::to_string(kMaxAuditionTakes) + " takes";
    }
    if (a->active > a->others.size()) {
        return std::string("is an audition whose active take is out of range");
    }
    constexpr std::int64_t kMaxTime = std::int64_t{1} << 60;
    if (a->headTrim.value < 0 || a->tailTrim.value < 0 || a->headTrim.value > kMaxTime || a->tailTrim.value > kMaxTime) {
        return std::string("is an audition with a negative or huge uncovered range");
    }
    if (clip.hasUnknownSource()) {
        return std::string("is an audition of a source this version does not know");
    }
    const EffectKind effects = kind == StreamKind::Video ? EffectKind::Video : EffectKind::Audio;
    for (std::size_t i = 0; i < a->others.size(); ++i) {
        const AuditionTake& t = a->others[i];
        const std::string what = "has audition take " + std::to_string(i + (i >= a->active ? 2 : 1)) + " that";
        if (t.stream.kind != kind) {
            return what + " plays a stream of the wrong kind";
        }
        if (!t.speed.isValid()) {
            return what + " has an invalid speed";
        }
        if (t.speed.isFreeze() && kind == StreamKind::Audio) {
            return what + " is a frame hold of sound";
        }
        if (t.sourceIn.value < -kMaxTime || t.sourceIn.value > kMaxTime) {
            return what + " lies beyond the longest timeline";
        }
        const ClipContent& content = t.contentOrDefault();
        if (content.audition) {
            return what + " is an audition itself";
        }
        if (!content.unknownSource.empty()) {
            return what + " plays a source this version does not know";
        }
        if (content.name.size() > kMaxTakeNameBytes) {
            return what + " has a name longer than " + std::to_string(kMaxTakeNameBytes) + " bytes";
        }
        if (content.properties.firstInvalid()) {
            return what + " has a parameter outside its range";
        }
        if (const auto problem = effectStackProblem(content.effects, effects, state.ids.next)) {
            return what + ": " + *problem;
        }
        if (content.timeRemap) {
            if (const auto problem = remapProblem(*content.timeRemap)) {
                return what + " has a time remap that " + *problem;
            }
            if (!t.speed.isUnity() || t.reversed) {
                return what + " is time-remapped but not at speed 100 % forwards";
            }
        }
        if (content.channels && (kind != StreamKind::Audio || t.isNested())) {
            return what + " has a channel map but is not sound of a media item";
        }
        const Clip v = takeClip(t, clip.timelineRange);
        if (t.isNested()) {
            if (t.media.isValid()) {
                return what + " plays both media and a sequence";
            }
            const Sequence* nested = state.findSequence(t.sequence);
            if (!nested) {
                return what + " refers to missing sequence " + std::to_string(t.sequence.value);
            }
            if (t.stream.index != 0 || !sequenceHasStream(*nested, t.stream)) {
                return what + " refers to a stream its sequence does not have";
            }
            if (const auto problem = clipAnglesProblem(state, v)) {
                return what + " " + *problem;
            }
            continue;
        }
        if (!t.angles.empty()) {
            return what + " has multicam angles but no multicam source";
        }
        const MediaItem* media = state.findMedia(t.media);
        if (!media) {
            return what + " refers to missing media " + std::to_string(t.media.value);
        }
        if (!media->hasStream(t.stream)) {
            return what + " refers to a stream its media does not have";
        }
        if (content.graphic && (kind != StreamKind::Video || !media->isGraphic())) {
            return what + " holds a graphic but does not play a graphic canvas";
        }
    }
    return std::nullopt;
}

TakeFit takeFit(const ProjectState& state, const Clip& clip, std::size_t index, Rate frameRate) {
    const Clip whole = withoutAudition(auditionTakeClip(clip, index));
    return shortfall(state, whole, framesToTicks(1, frameRate));
}

std::vector<ClipId> auditionPartners(const Sequence& sequence, ClipId clip) {
    std::vector<ClipId> out;
    const auto loc = sequence.findClip(clip);
    if (!loc) {
        return out;
    }
    const Clip& c = loc->clip();
    out.push_back(c.id);
    if (!c.linkGroup.isValid()) {
        return out;
    }
    const std::size_t n = takeCount(c);
    sequence.forEachTrack([&](const Track& t) {
        if (t.locked) {
            return;
        }
        for (const Clip& x : t.clips) {
            // Plain linked partners of a plain clip (New Audition makes them auditions together) or audition partners of
            // the same size.
            if (x.id != c.id && x.linkGroup == c.linkGroup && takeCount(x) == n) {
                out.push_back(x.id);
            }
        }
    });
    return out;
}

namespace edit {

namespace {

/// One take being gathered: its clips (track order, video first) as they are on the timeline.
struct Gathered {
    std::vector<ClipRef> clips;
    Ticks start; ///< its first clip's audition start (where it lines up with the others)
};

/// The clips (with their linked partners) grouped into takes: one per link group, an unlinked clip alone.
std::vector<Gathered> gatherTakes(const Sequence& seq, std::span<const ClipRef> refs) {
    std::vector<Gathered> takes;
    std::map<std::uint64_t, std::size_t> byGroup;
    for (const ClipRef& r : refs) {
        const Clip& c = seq.findTrack(r.track)->clips[r.index];
        std::size_t slot = takes.size();
        if (c.linkGroup.isValid()) {
            const auto [it, added] = byGroup.try_emplace(c.linkGroup.value, takes.size());
            slot = it->second;
            if (added) {
                takes.emplace_back();
            }
        } else {
            takes.emplace_back();
        }
        takes[slot].clips.push_back(r);
    }
    for (Gathered& g : takes) {
        std::stable_sort(g.clips.begin(), g.clips.end(), [&](const ClipRef& a, const ClipRef& b) {
            return trackOrderLess(seq, a.track, b.track);
        });
        g.start = auditionRange(seq.findTrack(g.clips.front().track)->clips[g.clips.front().index]).start;
    }
    return takes;
}

/// "1 picture and 2 sound clips".
std::string shapeText(std::size_t video, std::size_t audio) {
    std::string out = std::to_string(video) + (video == 1 ? " picture" : " pictures");
    out += " and " + std::to_string(audio) + (audio == 1 ? " sound clip" : " sound clips");
    return out;
}

std::pair<std::size_t, std::size_t> shapeOf(const Sequence& seq, const Gathered& g) {
    std::size_t video = 0;
    for (const ClipRef& r : g.clips) {
        if (seq.trackPosition(r.track)->kind == TrackKind::Video) {
            ++video;
        }
    }
    return {video, g.clips.size() - video};
}

/// Builds the stacks: `anchor`'s clips become the lanes (its own takes first, kept as they are) and every take of
/// `others` (each of their clips' takes, when they are auditions) is added after them, aligned to the anchor's start,
/// and leaves the timeline. With `duplicate` and no others, a copy of the anchor's active take is added and made
/// active.
EditOutcome build(const ProjectState& state, const Sequence& seq, const Gathered& anchor, std::span<const Gathered> others,
                  bool duplicate, bool alignStarts = true) {
    const auto [video, audio] = shapeOf(seq, anchor);
    for (std::size_t i = 0; i < others.size(); ++i) {
        const auto shape = shapeOf(seq, others[i]);
        if (shape.first != video || shape.second != audio) {
            const Clip& first = seq.findTrack(others[i].clips.front().track)->clips[others[i].clips.front().index];
            return std::unexpected(invalidArgument(
                "the takes must be alike: the first has " + shapeText(video, audio) + ", '" + first.name() + "' has " +
                shapeText(shape.first, shape.second) + " (unlink or select clips of one kind)"));
        }
    }
    const auto clipOf = [&](const ClipRef& r) -> const Clip& { return seq.findTrack(r.track)->clips[r.index]; };
    // Every lane of a take must hold the same number of takes (an audition's picture and sound move together).
    const auto takesOf = [&](const Gathered& g) -> std::expected<std::size_t, Error> {
        const std::size_t n = takeCount(clipOf(g.clips.front()));
        for (const ClipRef& r : g.clips) {
            if (takeCount(clipOf(r)) != n) {
                return std::unexpected(invalidArgument("the picture and sound of '" + clipOf(r).name() +
                                                       "' hold different numbers of takes: flatten it first"));
            }
        }
        return n;
    };
    const auto anchorCount = takesOf(anchor);
    if (!anchorCount) {
        return std::unexpected(anchorCount.error());
    }
    std::size_t total = *anchorCount;
    for (const Gathered& g : others) {
        const auto n = takesOf(g);
        if (!n) {
            return std::unexpected(n.error());
        }
        total += *n;
    }
    if (duplicate && others.empty()) {
        ++total;
    }
    if (total < 2) {
        return std::unexpected(invalidArgument("an audition needs at least two takes"));
    }
    if (total > kMaxAuditionTakes) {
        return std::unexpected(invalidArgument("an audition holds at most " + std::to_string(kMaxAuditionTakes) +
                                               " takes; this would make " + std::to_string(total)));
    }

    SequenceEditor ed(state, seq);
    // The new takes of each lane, in order.
    std::vector<std::vector<AuditionTake>> added(anchor.clips.size());
    for (const Gathered& g : others) {
        const Ticks delta = alignStarts ? anchor.start - g.start : Ticks{};
        for (std::size_t lane = 0; lane < anchor.clips.size(); ++lane) {
            const Clip& laneClip = clipOf(anchor.clips[lane]);
            const TimeRange target = auditionRange(laneClip);
            const Clip& source = clipOf(g.clips[lane]);
            for (std::size_t i = 0; i < takeCount(source); ++i) {
                Clip v = withoutAudition(auditionTakeClip(source, i));
                v.timelineRange.start += delta;
                v.moveHead(target.start - v.start());
                v.moveTail(target.end() - v.end());
                added[lane].push_back(takeFromClip(v));
            }
        }
    }
    for (const Gathered& g : others) {
        for (const ClipRef& r : g.clips) {
            ed.removeClip(r.track, r.clip);
        }
    }
    for (std::size_t lane = 0; lane < anchor.clips.size(); ++lane) {
        const ClipRef& r = anchor.clips[lane];
        const Clip& c = clipOf(r);
        const Audition* a = c.audition();
        std::vector<AuditionTake> takes = allTakes(c);
        std::size_t active = a ? a->active : 0;
        const Ticks head = a ? a->headTrim : Ticks{};
        const Ticks tail = a ? a->tailTrim : Ticks{};
        const auto index = ed.track(r.track).indexOf(r.clip);
        if (!index) {
            return std::unexpected(invalidArgument("clip '" + c.name() + "' is a take of itself"));
        }
        Clip next;
        if (duplicate && others.empty()) {
            // Duplicate as Audition: the copy (new effect ids, "<name> copy") is the active take, right after the original.
            AuditionTake copy = takes[active];
            copy.content = withNewEffectIds(copy.content, ed, copyName(copy.name()));
            takes.insert(takes.begin() + static_cast<std::ptrdiff_t>(active) + 1, copy);
            Clip clip = withoutAudition(c);
            clip.setContent(copy.content);
            next = restacked(clip, std::move(takes), active + 1, head, tail);
        } else {
            for (AuditionTake& t : added[lane]) {
                takes.push_back(std::move(t));
            }
            next = restacked(c, std::move(takes), active, head, tail);
        }
        ed.mutableClip(r.track, *index) = std::move(next);
    }
    return ed.finish();
}

/// Applies `change(clip, track, index)` to every lane of the audition `clip` (its partners included).
template <class Change>
EditOutcome editLanes(const ProjectState& state, SequenceId sequence, ClipId clip, Change&& change) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto lanes = auditionLanes(**seq, clip, false);
    if (!lanes) {
        return std::unexpected(lanes.error());
    }
    SequenceEditor ed(state, **seq);
    for (const ClipRef& r : *lanes) {
        const Track& t = ed.track(r.track);
        const auto index = t.indexOf(r.clip);
        if (!index) {
            continue;
        }
        auto next = change(t.clips[*index], t, *index, ed);
        if (!next) {
            return std::unexpected(next.error());
        }
        ed.mutableClip(r.track, *index) = std::move(*next);
    }
    return ed.finish();
}

std::expected<std::size_t, Error> checkedTake(const Clip& clip, std::size_t index) {
    const std::size_t n = takeCount(clip);
    if (index >= n) {
        return std::unexpected(invalidArgument("the audition has " + std::to_string(n) + " takes; there is no take " +
                                               std::to_string(index + 1)));
    }
    return index;
}

} // namespace

EditOutcome createAudition(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (clips.empty()) {
        return std::unexpected(invalidArgument("select the clips to make an audition of"));
    }
    const auto refs = detail::resolveClips(**seq, clips, LinkMode::FollowLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    std::vector<Gathered> takes = gatherTakes(**seq, *refs);
    std::stable_sort(takes.begin(), takes.end(), [&](const Gathered& a, const Gathered& b) {
        if (a.start != b.start) {
            return a.start < b.start;
        }
        return trackOrderLess(**seq, a.clips.front().track, b.clips.front().track);
    });
    const Gathered& anchor = takes.front();
    const std::span<const Gathered> others(takes.data() + 1, takes.size() - 1);
    if (others.empty()) {
        const Clip& only = (*seq)->findTrack(anchor.clips.front().track)->clips[anchor.clips.front().index];
        if (only.isAudition()) {
            return std::unexpected(invalidArgument("'" + only.name() +
                                                   "' is already an audition: use Duplicate Take or Add to Audition"));
        }
    }
    return build(state, **seq, anchor, others, true);
}

EditOutcome addAuditionTakes(const ProjectState& state, SequenceId sequence, ClipId audition,
                             std::span<const ClipId> clips, bool alignStarts) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto lanes = auditionLanes(**seq, audition, true);
    if (!lanes) {
        return std::unexpected(lanes.error());
    }
    Gathered anchor{*lanes, auditionRange((*seq)->findClip(audition)->clip()).start};
    std::vector<ClipId> rest;
    for (const ClipId id : clips) {
        const bool own = std::any_of(lanes->begin(), lanes->end(), [&](const ClipRef& r) { return r.clip == id; });
        if (!own) {
            rest.push_back(id);
        }
    }
    if (rest.empty()) {
        return std::unexpected(invalidArgument("select the clips to add to the audition"));
    }
    const auto refs = detail::resolveClips(**seq, rest, LinkMode::FollowLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    std::vector<ClipRef> outside;
    for (const ClipRef& r : *refs) {
        const bool own = std::any_of(lanes->begin(), lanes->end(), [&](const ClipRef& l) { return l.clip == r.clip; });
        if (!own) {
            outside.push_back(r);
        }
    }
    std::vector<Gathered> takes = gatherTakes(**seq, outside);
    std::stable_sort(takes.begin(), takes.end(), [&](const Gathered& a, const Gathered& b) {
        if (a.start != b.start) {
            return a.start < b.start;
        }
        return trackOrderLess(**seq, a.clips.front().track, b.clips.front().track);
    });
    if (takes.empty()) {
        return std::unexpected(invalidArgument("select the clips to add to the audition"));
    }
    return build(state, **seq, anchor, takes, false, alignStarts);
}

EditOutcome addAuditionSource(const ProjectState& state, SequenceId sequence, ClipId audition, const TakeSource& source) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto lanes = auditionLanes(**seq, audition, true);
    if (!lanes) {
        return std::unexpected(lanes.error());
    }
    const MediaItem* media = source.sequence.isValid() ? nullptr : state.findMedia(source.media);
    const Sequence* nested = source.sequence.isValid() ? state.findSequence(source.sequence) : nullptr;
    if (!media && !nested) {
        return std::unexpected(notFound("the source to add is not in the project"));
    }
    if (nested && sequenceContains(state, nested->id, sequence)) {
        return std::unexpected(invalidArgument("sequence '" + nested->name + "' plays this sequence: it cannot be a take here"));
    }
    if (takeCount((*seq)->findClip(audition)->clip()) + 1 > kMaxAuditionTakes) {
        return std::unexpected(invalidArgument("an audition holds at most " + std::to_string(kMaxAuditionTakes) + " takes"));
    }
    SequenceEditor ed(state, **seq);
    std::size_t videoLane = 0;
    std::size_t audioLane = 0;
    for (const ClipRef& r : *lanes) {
        const Track& t = ed.track(r.track);
        const auto index = t.indexOf(r.clip);
        if (!index) {
            continue;
        }
        const Clip& c = t.clips[*index];
        const bool video = t.kind == TrackKind::Video;
        const std::size_t lane = video ? videoLane++ : audioLane++;
        AuditionTake take;
        take.stream = {video ? StreamKind::Video : StreamKind::Audio, 0};
        take.sourceIn = source.sourceIn;
        std::shared_ptr<const ClipContent> content;
        if (nested) {
            take.sequence = nested->id;
            if (lane > 0 || !sequenceHasStream(*nested, take.stream)) {
                return std::unexpected(invalidArgument("sequence '" + nested->name + "' has no " +
                                                       (video ? "picture" : "sound") + " for this take"));
            }
            if (nested->multicam) {
                for (std::size_t i = 0; i < nested->multicam->angles.size(); ++i) {
                    const MulticamAngle& angle = nested->multicam->angles[i];
                    if (video ? angle.video.isValid() : !angle.audio.empty()) {
                        take.angles = AngleSet::of(i);
                        break;
                    }
                }
            }
            ClipContent named;
            named.name = source.name.empty() ? nested->name : source.name;
            content = std::make_shared<const ClipContent>(std::move(named));
        } else {
            const std::size_t streams = video ? media->info.video.size() : media->info.audio.size();
            if (lane >= streams) {
                return std::unexpected(invalidArgument("'" + media->name + "' has no " +
                                                       (video ? "picture" : "sound stream " + std::to_string(lane + 1)) +
                                                       " for this audition"));
            }
            take.media = media->id;
            take.stream.index = static_cast<std::int32_t>(lane);
            take.speed = conformSpeed(*media);
            content = video ? scaledClipContent(*media, take.stream, ed.sequence(), media->defaultClipContent())
                            : media->defaultClipContent();
            if (!source.name.empty()) {
                ClipContent renamed = content ? *content : ClipContent{};
                renamed.name = source.name;
                content = std::make_shared<const ClipContent>(std::move(renamed));
            }
        }
        take.content = std::move(content);
        if (take.contentOrDefault().name.size() > kMaxTakeNameBytes) {
            ClipContent cut = take.contentOrDefault();
            cut.name.resize(kMaxTakeNameBytes);
            take.content = std::make_shared<const ClipContent>(std::move(cut));
        }
        const Audition* a = c.audition();
        std::vector<AuditionTake> takes = allTakes(c);
        takes.push_back(std::move(take));
        ed.mutableClip(r.track, *index) = restacked(c, std::move(takes), a ? a->active : 0, a ? a->headTrim : Ticks{},
                                                    a ? a->tailTrim : Ticks{});
    }
    return ed.finish();
}

EditOutcome setActiveTake(const ProjectState& state, SequenceId sequence, ClipId audition, std::size_t index) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto loc = (*seq)->findClip(audition);
    if (loc && loc->clip().isAudition() && loc->clip().audition()->active == index) {
        return detail::unchanged(state);
    }
    const Ticks frame = framesToTicks(1, (*seq)->frameRate);
    return editLanes(state, sequence, audition,
                     [&](const Clip& c, const Track& t, std::size_t at, SequenceEditor&) -> std::expected<Clip, Error> {
                         const auto k = checkedTake(c, index);
                         if (!k) {
                             return std::unexpected(k.error());
                         }
                         return activate(state, t, at, c, allTakes(c), *k, auditionRange(c), frame);
                     });
}

EditOutcome removeAuditionTake(const ProjectState& state, SequenceId sequence, ClipId audition, std::size_t index) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Ticks frame = framesToTicks(1, (*seq)->frameRate);
    return editLanes(state, sequence, audition,
                     [&](const Clip& c, const Track& t, std::size_t at, SequenceEditor&) -> std::expected<Clip, Error> {
                         const auto k = checkedTake(c, index);
                         if (!k) {
                             return std::unexpected(k.error());
                         }
                         const Audition& a = *c.audition();
                         std::vector<AuditionTake> takes = allTakes(c);
                         if (*k != a.active) {
                             takes.erase(takes.begin() + static_cast<std::ptrdiff_t>(*k));
                             const std::size_t active = *k < a.active ? a.active - 1 : a.active;
                             return restacked(c, std::move(takes), active, a.headTrim, a.tailTrim);
                         }
                         // The active take goes: the next one (the previous one for the last) plays in its place.
                         const TimeRange whole = auditionRange(c);
                         takes.erase(takes.begin() + static_cast<std::ptrdiff_t>(*k));
                         const std::size_t next = std::min(*k, takes.size() - 1);
                         return activate(state, t, at, c, std::move(takes), next, whole, frame);
                     });
}

EditOutcome moveAuditionTake(const ProjectState& state, SequenceId sequence, ClipId audition, std::size_t from,
                             std::size_t to) {
    return editLanes(state, sequence, audition,
                     [&](const Clip& c, const Track&, std::size_t, SequenceEditor&) -> std::expected<Clip, Error> {
                         const auto a = checkedTake(c, from);
                         if (!a) {
                             return std::unexpected(a.error());
                         }
                         const auto b = checkedTake(c, to);
                         if (!b) {
                             return std::unexpected(b.error());
                         }
                         const Audition& stack = *c.audition();
                         std::vector<AuditionTake> takes = allTakes(c);
                         std::vector<std::size_t> order(takes.size());
                         for (std::size_t i = 0; i < order.size(); ++i) {
                             order[i] = i;
                         }
                         const std::size_t moved = order[*a];
                         order.erase(order.begin() + static_cast<std::ptrdiff_t>(*a));
                         order.insert(order.begin() + static_cast<std::ptrdiff_t>(*b), moved);
                         std::vector<AuditionTake> sorted;
                         std::size_t active = 0;
                         for (std::size_t i = 0; i < order.size(); ++i) {
                             if (order[i] == stack.active) {
                                 active = i;
                             }
                             sorted.push_back(std::move(takes[order[i]]));
                         }
                         return restacked(c, std::move(sorted), active, stack.headTrim, stack.tailTrim);
                     });
}

EditOutcome renameAuditionTake(const ProjectState& state, SequenceId sequence, ClipId audition, std::size_t index,
                               std::string name) {
    if (name.empty()) {
        return std::unexpected(invalidArgument("a take needs a name"));
    }
    if (name.size() > kMaxTakeNameBytes) {
        return std::unexpected(invalidArgument("a take name holds at most " + std::to_string(kMaxTakeNameBytes) + " bytes"));
    }
    return editLanes(state, sequence, audition,
                     [&](const Clip& c, const Track&, std::size_t, SequenceEditor&) -> std::expected<Clip, Error> {
                         const auto k = checkedTake(c, index);
                         if (!k) {
                             return std::unexpected(k.error());
                         }
                         const Audition& a = *c.audition();
                         if (*k == a.active) {
                             Clip renamed = c;
                             renamed.setName(name);
                             return renamed;
                         }
                         Audition next = a;
                         AuditionTake& t = next.others[*k < a.active ? *k : *k - 1];
                         ClipContent block = t.contentOrDefault();
                         block.name = name;
                         t.content = std::make_shared<const ClipContent>(std::move(block));
                         return withAudition(c, std::move(next));
                     });
}

EditOutcome duplicateAuditionTake(const ProjectState& state, SequenceId sequence, ClipId audition, std::size_t index) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto loc = (*seq)->findClip(audition);
    if (loc && takeCount(loc->clip()) + 1 > kMaxAuditionTakes) {
        return std::unexpected(invalidArgument("an audition holds at most " + std::to_string(kMaxAuditionTakes) + " takes"));
    }
    const Ticks frame = framesToTicks(1, (*seq)->frameRate);
    return editLanes(state, sequence, audition,
                     [&](const Clip& c, const Track& t, std::size_t at, SequenceEditor& ed) -> std::expected<Clip, Error> {
                         const auto k = checkedTake(c, index);
                         if (!k) {
                             return std::unexpected(k.error());
                         }
                         const Audition& a = *c.audition();
                         std::vector<AuditionTake> takes = allTakes(c);
                         AuditionTake copy = takes[*k];
                         copy.content = withNewEffectIds(copy.content, ed, copyName(copy.name()));
                         takes.insert(takes.begin() + static_cast<std::ptrdiff_t>(*k) + 1, std::move(copy));
                         if (*k == a.active) {
                             // The copy of the active take plays exactly what it plays: only the content changes.
                             Clip clip = withoutAudition(c);
                             clip.setContent(takes[*k + 1].content);
                             return restacked(clip, std::move(takes), *k + 1, a.headTrim, a.tailTrim);
                         }
                         return activate(state, t, at, c, std::move(takes), *k + 1, auditionRange(c), frame);
                     });
}

EditOutcome flattenAuditions(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    std::vector<ClipRef> lanes;
    for (const ClipId id : clips) {
        const auto loc = (*seq)->findClip(id);
        if (!loc) {
            return std::unexpected(notFound("clip " + std::to_string(id.value) + " is not in this sequence"));
        }
        if (!loc->clip().isAudition()) {
            continue;
        }
        const auto more = auditionLanes(**seq, id, false);
        if (!more) {
            return std::unexpected(more.error());
        }
        for (const ClipRef& r : *more) {
            if (std::none_of(lanes.begin(), lanes.end(), [&](const ClipRef& l) { return l.clip == r.clip; })) {
                lanes.push_back(r);
            }
        }
    }
    if (lanes.empty()) {
        return std::unexpected(invalidArgument("no selected clip is an audition"));
    }
    SequenceEditor ed(state, **seq);
    for (const ClipRef& r : lanes) {
        const auto index = ed.track(r.track).indexOf(r.clip);
        if (index) {
            Clip& c = ed.mutableClip(r.track, *index);
            c = withoutAudition(c);
        }
    }
    return ed.finish();
}

} // namespace edit

} // namespace wabcut
