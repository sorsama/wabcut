// Moving clips (overwrite and insert modes, across tracks) and A/V linking.

#include <wabcut/model/EditOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace wabcut::edit {

using detail::ClipRef;
using detail::invalidArgument;
using detail::SequenceEditor;

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
    std::vector<TrackId> destination;
    destination.reserve(selection->size());
    Ticks earliest = s.findTrack(selection->front().track)->clips[selection->front().index].start();
    for (const ClipRef& ref : *selection) {
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
        destination.push_back(dest.id);
        earliest = std::min(earliest, s.findTrack(ref.track)->clips[ref.index].start());
    }
    const Ticks d = std::max(delta, -earliest);
    if (d.value == 0 && shift.video == 0 && shift.audio == 0) {
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
