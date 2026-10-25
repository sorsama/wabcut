// Moving clips (overwrite, insert and, 0.4, rearrange modes, across tracks), copying them (0.4, Alt-drag) and A/V
// linking.

#include <wabcut/model/EditOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace wabcut::edit {

using detail::ClipRef;
using detail::invalidArgument;
using detail::SequenceEditor;

namespace {

/// Where a move (or a copy) takes the clips: the destination track of every clip of the selection (resolveClips
/// order) and the earliest start among them. Errors: no such track, a locked destination.
struct MovePlan {
    std::vector<TrackId> destination;
    Ticks earliest;
};

std::expected<MovePlan, Error> planMove(const Sequence& s, std::span<const ClipRef> selection, TrackShift shift) {
    MovePlan plan;
    plan.destination.reserve(selection.size());
    plan.earliest = s.findTrack(selection.front().track)->clips[selection.front().index].start();
    for (const ClipRef& ref : selection) {
        const auto pos = s.trackPosition(ref.track);
        const auto& list = s.tracks(pos->kind);
        const int offset = pos->kind == TrackKind::Video ? shift.video : shift.audio;
        const auto target = static_cast<std::ptrdiff_t>(pos->index) + offset;
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(list.size())) {
            return std::unexpected(invalidArgument(std::string("there is no ") +
                                                   (pos->kind == TrackKind::Video ? "V" : "A") +
                                                   std::to_string(target + 1) + " track to move to"));
        }
        const Track& dest = *list[static_cast<std::size_t>(target)];
        if (dest.locked) {
            return std::unexpected(invalidArgument("cannot move clips onto locked track " +
                                                   detail::trackLabel(s, dest.id)));
        }
        plan.destination.push_back(dest.id);
        plan.earliest = std::min(plan.earliest, s.findTrack(ref.track)->clips[ref.index].start());
    }
    return plan;
}

bool noTrackShift(TrackShift shift) { return shift.video == 0 && shift.audio == 0; }

/// 0.4, MoveMode::Rearrange (Premiere's Ctrl+Alt-drag): the clips' time is ripple-removed at their old place and opened
/// at the drop, ONLY on the tracks that hold a dragged clip and the destination tracks (lead decision 2026-10-24: other
/// tracks, sync-locked or not, the captions and the sequence markers are untouched, so a music bed under rearranged
/// shots stays whole). Clips selected apart (gaps or unselected clips between them) are packed at the drop in time
/// order: only the time they leave opens there, so no gap appears between them and a same-track rearrange keeps the
/// sequence length. `d` is the clamped delta; `dropClip` is the clip whose destination track the drop is read on (the
/// first one listed: the clip a drag grabbed).
EditOutcome rearrange(const ProjectState& state, const Sequence& s, std::span<const ClipRef> selection,
                      const MovePlan& plan, Ticks d, TrackShift shift, ClipId dropClip) {
    // 1. The removed time: the merged union of the dragged clips' ranges. Closing it must not take an unselected clip off
    //    a track the selection is on (Ripple Delete's rule; keep in step with rippleDeleteClips, EditOps.cpp).
    std::vector<TimeRange> ranges;
    std::unordered_set<ClipId> selected;
    std::unordered_set<TrackId> origin;
    for (const ClipRef& ref : selection) {
        const Clip& c = s.findTrack(ref.track)->clips[ref.index];
        ranges.push_back(c.timelineRange);
        selected.insert(ref.clip);
        origin.insert(ref.track);
    }
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> removed;
    for (const TimeRange& r : ranges) {
        if (!removed.empty() && r.start <= removed.back().end()) {
            removed.back() = TimeRange::fromStartEnd(removed.back().start, std::max(removed.back().end(), r.end()));
        } else {
            removed.push_back(r);
        }
    }
    for (const TrackId id : origin) {
        const Track& t = *s.findTrack(id);
        for (const TimeRange& r : removed) {
            for (std::size_t i = t.clipIndexAt(r.start).value_or(t.firstClipAtOrAfter(r.start));
                 i < t.clips.size() && t.clips[i].start() < r.end(); ++i) {
                if (!selected.contains(t.clips[i].id)) {
                    return std::unexpected(invalidArgument("rearrange would also remove unselected clip '" +
                                                           t.clips[i].name() + "' on " + detail::trackLabel(s, id)));
                }
            }
        }
    }

    // 2. The packing (step 5) keeps the time inside each merged range, not the gaps between them, so the members of a
    //    link group must lie in one range or their sync would break: refused then (a sound placed apart from its
    //    picture). Every clip start lies in the last range starting at or before it.
    const auto rangeOf = [&](Ticks start) {
        const auto after = std::upper_bound(removed.begin(), removed.end(), start,
                                            [](Ticks t, const TimeRange& r) { return t < r.start; });
        return static_cast<std::size_t>(after - removed.begin()) - 1;
    };
    std::unordered_map<LinkGroupId, std::size_t> groupRange;
    for (const ClipRef& ref : selection) {
        const Clip& c = s.findTrack(ref.track)->clips[ref.index];
        if (!c.linkGroup.isValid()) {
            continue;
        }
        const std::size_t k = rangeOf(c.start());
        if (const auto [it, added] = groupRange.emplace(c.linkGroup, k); !added && it->second != k) {
            return std::unexpected(
                invalidArgument("rearrange works on clips that are side by side; use Ctrl-drag to insert"));
        }
    }
    if (d.value == 0 && noTrackShift(shift)) {
        return detail::unchanged(state); // not dragged: nothing packs either
    }

    // 3. The drop time is read on the timeline as it was before the drag, on the dragged clip's destination track. When
    //    that track is one the clips leave, the removed time before the drop comes off, so a clip dropped at the cut
    //    after its neighbour swaps with it. A track they do not leave loses no time, so the drop stays where it is
    //    there (dragged up to V2 after Y, it lands after Y). One drop time for every track keeps linked partners in
    //    sync.
    const Ticks newStart = plan.earliest + d;
    std::size_t dropIndex = 0;
    for (std::size_t i = 0; i < selection.size(); ++i) {
        if (selection[i].clip == dropClip) {
            dropIndex = i;
            break;
        }
    }
    Ticks at = newStart;
    if (origin.contains(plan.destination[dropIndex])) {
        for (const TimeRange& r : removed) {
            if (r.start < newStart) {
                at = at - (std::min(r.end(), newStart) - r.start);
            }
        }
    }
    // Back into its own place: no change (no undo step). Clips selected apart still pack there, which is a change.
    if (removed.size() == 1 && at == plan.earliest && noTrackShift(shift)) {
        return detail::unchanged(state);
    }

    // 4. Take the clips and close their time on the tracks that held them (from the back, so earlier ranges stay put).
    SequenceEditor ed(state, s);
    std::vector<Clip> taken = ed.takeClips(selection);
    for (auto r = removed.rbegin(); r != removed.rend(); ++r) {
        for (const TrackId id : ed.trackIds()) {
            if (origin.contains(id) && !ed.track(id).locked) {
                ed.rippleRemove(id, *r);
            }
        }
    }
    // 5. Pack the merged ranges at the drop in time order, one after another, each clip keeping its place inside its
    //    range and its own destination track (a linked pair, a J or L cut keep their offsets). Only the ranges' total
    //    opens, on the destination tracks only: the clips take the time they left, so no gap opens between them and a
    //    rearrange on the same tracks keeps the sequence length.
    std::vector<Ticks> packed; // where each range starts in the packed block
    packed.reserve(removed.size());
    Ticks total{};
    for (const TimeRange& r : removed) {
        packed.push_back(total);
        total += r.duration;
    }
    const std::unordered_set<TrackId> destinations(plan.destination.begin(), plan.destination.end());
    for (const TrackId id : ed.trackIds()) {
        if (destinations.contains(id) && !ed.track(id).locked) {
            ed.rippleInsert(id, at, total);
        }
    }
    for (std::size_t i = 0; i < taken.size(); ++i) {
        Clip clip = std::move(taken[i]);
        const std::size_t k = rangeOf(clip.start());
        clip.timelineRange.start = at + packed[k] + (clip.start() - removed[k].start);
        ed.placeClip(plan.destination[i], std::move(clip));
    }
    return ed.finish(std::nullopt, d);
}

} // namespace

EditOutcome moveClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips, Ticks delta,
                      TrackShift shift, MoveMode mode, LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Sequence& s = **seq;
    const auto selection = detail::resolveClips(s, clips, links);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    if (selection->empty()) {
        return detail::unchanged(state);
    }

    // Destination track of every moving clip.
    const auto plan = planMove(s, *selection, shift);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    const std::vector<TrackId>& destination = plan->destination;
    const Ticks d = std::max(delta, -plan->earliest);
    if (mode == MoveMode::Rearrange) {
        // Its refusals first: a selection that cannot be rearranged is refused at its own place too, so a drag held
        // there keeps saying why.
        return rearrange(state, s, *selection, *plan, d, shift, clips.front());
    }
    if (d.value == 0 && noTrackShift(shift)) {
        return detail::unchanged(state);
    }

    SequenceEditor ed(state, s);
    std::vector<Clip> taken = ed.takeClips(*selection);
    std::vector<std::pair<TrackId, Clip>> moving;
    moving.reserve(taken.size());
    for (std::size_t i = 0; i < taken.size(); ++i) {
        taken[i].timelineRange.start += d;
        moving.emplace_back(destination[i], std::move(taken[i]));
    }

    if (mode == MoveMode::Insert) {
        Ticks first = moving.front().second.start();
        Ticks last = moving.front().second.end();
        for (const auto& [track, clip] : moving) {
            first = std::min(first, clip.start());
            last = std::max(last, clip.end());
        }
        const std::unordered_set<TrackId> destinations(destination.begin(), destination.end());
        for (const TrackId id : ed.trackIds()) {
            const Track& t = ed.track(id);
            if (!t.locked && (destinations.contains(id) || t.syncLocked)) {
                ed.rippleInsert(id, first, last - first);
            }
        }
        ed.captionsInsert(first, last - first);
    } else {
        for (const auto& [track, clip] : moving) {
            ed.clearRange(track, clip.timelineRange);
        }
    }
    for (auto& [track, clip] : moving) {
        ed.placeClip(track, std::move(clip));
    }
    return ed.finish(std::nullopt, d);
}

EditOutcome duplicateClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips, Ticks delta,
                           TrackShift shift, MoveMode mode, LinkMode links) {
    if (mode == MoveMode::Rearrange) {
        return std::unexpected(invalidArgument("a copy cannot be a rearrange edit"));
    }
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Sequence& s = **seq;
    const auto selection = detail::resolveClips(s, clips, links);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    if (selection->empty()) {
        return detail::unchanged(state);
    }
    const auto plan = planMove(s, *selection, shift);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    const Ticks d = std::max(delta, -plan->earliest);
    if (d.value == 0 && noTrackShift(shift)) {
        return detail::unchanged(state); // the copy would land exactly on its original
    }

    // A clip group gets a new group for its copies when two or more of its clips are copied (one copy is ungrouped).
    std::unordered_map<std::uint64_t, int> groupCopies;
    for (const ClipRef& ref : *selection) {
        if (const ClipGroupId g = s.findTrack(ref.track)->clips[ref.index].group; g.isValid()) {
            ++groupCopies[g.value];
        }
    }

    // The copies, built from the sequence as it is (the originals stay): new clip ids, one new link group per copied
    // link group, new clip groups, new effect instance ids (Clipboard's paste does the same).
    SequenceEditor ed(state, s);
    std::unordered_map<std::uint64_t, LinkGroupId> linkMap;
    std::unordered_map<std::uint64_t, ClipGroupId> groupMap;
    std::unordered_map<std::uint64_t, ClipId> clipMap; // original -> copy
    std::vector<std::pair<TrackId, Clip>> copies;
    copies.reserve(selection->size());
    for (std::size_t i = 0; i < selection->size(); ++i) {
        const ClipRef& ref = (*selection)[i];
        const Clip& c = s.findTrack(ref.track)->clips[ref.index];
        Clip n = c;
        n.id = ed.newClipId();
        n.timelineRange.start = c.start() + d;
        if (c.linkGroup.isValid()) {
            auto g = linkMap.find(c.linkGroup.value);
            if (g == linkMap.end()) {
                g = linkMap.emplace(c.linkGroup.value, ed.newLinkGroup()).first;
            }
            n.linkGroup = g->second;
            ed.touchGroup(n.linkGroup);
        }
        if (c.group.isValid()) {
            if (groupCopies[c.group.value] >= 2) {
                auto g = groupMap.find(c.group.value);
                if (g == groupMap.end()) {
                    g = groupMap.emplace(c.group.value, ed.newClipGroup()).first;
                }
                n.group = g->second;
            } else {
                n.group = {};
            }
        }
        if (!n.effects().empty()) {
            EffectStack effects = n.effects();
            for (EffectInstance& e : effects) {
                e.id = ed.newEffectId();
            }
            n.setEffects(std::move(effects));
        }
        clipMap.emplace(c.id.value, n.id);
        copies.emplace_back(plan->destination[i], std::move(n));
    }

    if (mode == MoveMode::Insert) {
        // Room first, at the copies' extent: the destination tracks and every unlocked sync-locked track, the captions.
        Ticks first = copies.front().second.start();
        Ticks last = copies.front().second.end();
        for (const auto& [track, clip] : copies) {
            first = std::min(first, clip.start());
            last = std::max(last, clip.end());
        }
        const std::unordered_set<TrackId> destinations(plan->destination.begin(), plan->destination.end());
        for (const TrackId id : ed.trackIds()) {
            const Track& t = ed.track(id);
            if (!t.locked && (destinations.contains(id) || t.syncLocked)) {
                ed.rippleInsert(id, first, last - first);
            }
        }
        ed.captionsInsert(first, last - first);
    } else {
        // Overwrite: each copy replaces what lies under it (its own original included).
        for (const auto& [track, clip] : copies) {
            ed.clearRange(track, clip.timelineRange);
        }
    }
    std::vector<ClipId> copyIds;
    copyIds.reserve(copies.size());
    for (auto& [track, clip] : copies) {
        copyIds.push_back(clip.id);
        ed.placeNewClip(track, std::move(clip), false);
    }

    // The transitions of the copied clips (both sides copied, or a one-sided one of a copied clip), on the copies'
    // track, in edit point order.
    const auto mapped = [&](ClipId id) -> std::optional<ClipId> {
        if (!id.isValid()) {
            return ClipId{};
        }
        const auto it = clipMap.find(id.value);
        return it == clipMap.end() ? std::nullopt : std::optional<ClipId>(it->second);
    };
    std::unordered_set<TrackId> sourceTracks;
    for (std::size_t i = 0; i < selection->size(); ++i) {
        const ClipRef& ref = (*selection)[i];
        if (!sourceTracks.insert(ref.track).second) {
            continue;
        }
        const TrackId dest = plan->destination[i];
        for (const Transition& tr : s.findTrack(ref.track)->transitions) {
            const auto outgoing = mapped(tr.outgoing);
            const auto incoming = mapped(tr.incoming);
            if (!outgoing || !incoming || (!outgoing->isValid() && !incoming->isValid())) {
                continue;
            }
            Transition t = tr;
            t.id = ed.newTransitionId();
            t.outgoing = *outgoing;
            t.incoming = *incoming;
            Track& track = ed.mutableTrack(dest);
            const Ticks cut = transitionEditPoint(track, t).value_or(Ticks{});
            const auto pos = std::find_if(track.transitions.begin(), track.transitions.end(), [&](const Transition& o) {
                return transitionEditPoint(track, o).value_or(Ticks{}) > cut;
            });
            track.transitions.insert(pos, std::move(t));
        }
    }

    EditResult result = ed.finish(std::nullopt, d);
    // The copies first, in resolveClips order (then any piece an overwrite split off), and as the placed clips.
    std::vector<ClipId> created = copyIds;
    for (const ClipId id : result.created) {
        if (std::find(copyIds.begin(), copyIds.end(), id) == copyIds.end()) {
            created.push_back(id);
        }
    }
    result.created = std::move(created);
    result.placed = std::move(copyIds);
    return result;
}

EditOutcome linkClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto selection = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    if (selection->size() < 2) {
        return std::unexpected(invalidArgument("select at least two clips to link"));
    }
    SequenceEditor ed(state, **seq);
    const LinkGroupId group = ed.newLinkGroup();
    for (const ClipRef& ref : *selection) {
        Clip& clip = ed.mutableClip(ref.track, ref.index);
        ed.touchGroup(clip.linkGroup);
        clip.linkGroup = group;
    }
    return ed.finish();
}

EditOutcome linkClipGroups(const ProjectState& state, SequenceId sequenceId, std::span<const std::vector<ClipId>> groups) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    std::vector<ClipId> all;
    for (const std::vector<ClipId>& g : groups) {
        if (g.size() < 2) {
            return std::unexpected(invalidArgument("a link group needs at least two clips"));
        }
        all.insert(all.end(), g.begin(), g.end());
    }
    if (all.empty()) {
        return detail::unchanged(state);
    }
    const auto selection = detail::resolveClips(**seq, all, LinkMode::IgnoreLinks);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    std::unordered_map<std::uint64_t, ClipRef> where;
    for (const ClipRef& ref : *selection) {
        where.emplace(ref.clip.value, ref);
    }
    std::unordered_set<std::uint64_t> seen;
    for (const ClipId id : all) {
        if (!seen.insert(id.value).second) {
            return std::unexpected(invalidArgument("clip " + std::to_string(id.value) + " is in two link groups"));
        }
        if (!where.contains(id.value)) {
            return std::unexpected(invalidArgument("clip " + std::to_string(id.value) + " is on a locked track"));
        }
    }
    SequenceEditor ed(state, **seq);
    for (const std::vector<ClipId>& g : groups) {
        const LinkGroupId group = ed.newLinkGroup();
        for (const ClipId id : g) {
            const ClipRef& ref = where.at(id.value);
            Clip& clip = ed.mutableClip(ref.track, ref.index);
            ed.touchGroup(clip.linkGroup);
            clip.linkGroup = group;
        }
    }
    return ed.finish();
}

EditOutcome unlinkClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto selection = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    const bool anyLinked = std::any_of(selection->begin(), selection->end(), [&](const ClipRef& ref) {
        return (*seq)->findTrack(ref.track)->clips[ref.index].linkGroup.isValid();
    });
    if (!anyLinked) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    for (const ClipRef& ref : *selection) {
        if (ed.track(ref.track).clips[ref.index].linkGroup.isValid()) {
            Clip& clip = ed.mutableClip(ref.track, ref.index);
            ed.touchGroup(clip.linkGroup);
            clip.linkGroup = {};
        }
    }
    return ed.finish();
}

} // namespace wabcut::edit
