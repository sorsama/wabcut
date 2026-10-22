// Trims: Q/W ripple trim to playhead, regular trim, ripple edit (B), rolling edit (N), slip (Y), slide (U).

#include <wabcut/model/ClipGroups.h>
#include <wabcut/model/EditOps.h>

#include "SequenceEditor.h"

#include <algorithm>

namespace wabcut::edit {

using detail::ClipRef;
using detail::DeltaBounds;
using detail::invalidArgument;
using detail::SequenceEditor;

// --- Q / W --------------------------------------------------------------------------------------------

namespace {

EditOutcome rippleTrimToPlayhead(const ProjectState& state, SequenceId sequenceId, Ticks playhead, bool previous) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Sequence& s = **seq;
    const std::vector<TrackId> targets = detail::unlockedTargets(s);
    if (targets.empty()) {
        return std::unexpected(invalidArgument("no unlocked track is targeted"));
    }

    // The edit point: start (Q) or end (W) of the clip or gap under the playhead, nearest to the
    // playhead across the targeted tracks.
    std::optional<Ticks> edit;
    for (const TrackId id : targets) {
        const Track& t = *s.findTrack(id);
        std::optional<Ticks> candidate;
        if (const auto i = t.clipIndexAt(playhead)) {
            candidate = previous ? t.clips[*i].start() : t.clips[*i].end();
        } else {
            const std::size_t next = t.firstClipAtOrAfter(playhead);
            if (previous && !t.clips.empty()) {
                candidate = next > 0 ? t.clips[next - 1].end() : Ticks{};
            } else if (!previous && next < t.clips.size()) {
                candidate = t.clips[next].start();
            }
        }
        if (candidate) {
            edit = !edit ? *candidate : previous ? std::max(*edit, *candidate) : std::min(*edit, *candidate);
        }
    }
    if (!edit) {
        return std::unexpected(invalidArgument(previous ? "there is no edit before the playhead"
                                                        : "there is no edit after the playhead"));
    }
    const TimeRange removed = previous ? TimeRange::fromStartEnd(*edit, playhead)
                                       : TimeRange::fromStartEnd(playhead, *edit);
    if (removed.isEmpty()) {
        return detail::unchanged(state);
    }

    SequenceEditor ed(state, s);
    for (const TrackId id : ed.trackIds()) {
        const Track& t = ed.track(id);
        if (!t.locked && (t.targeted || t.syncLocked)) {
            ed.rippleRemove(id, removed);
        }
    }
    ed.captionsRemove(removed);
    return ed.finish(previous ? *edit : playhead);
}

} // namespace

EditOutcome rippleTrimPreviousEditToPlayhead(const ProjectState& state, SequenceId sequence, Ticks playhead) {
    return rippleTrimToPlayhead(state, sequence, playhead, true);
}

EditOutcome rippleTrimNextEditToPlayhead(const ProjectState& state, SequenceId sequence, Ticks playhead) {
    return rippleTrimToPlayhead(state, sequence, playhead, false);
}

// --- Regular trim -------------------------------------------------------------------------------------

EditOutcome trimEdge(const ProjectState& state, SequenceId sequenceId, ClipId clipId, Edge edge, Ticks delta,
                     LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto parts = detail::edgeParticipants(**seq, clipId, edge, links);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    SequenceEditor ed(state, **seq);
    const Ticks minDuration = ed.oneFrame();
    DeltaBounds bounds;
    for (const ClipRef& p : *parts) {
        const Track& t = ed.track(p.track);
        const Clip& c = t.clips[p.index];
        if (edge == Edge::Head) {
            bounds.atMost(c.duration() - minDuration);
            if (const auto room = ed.headRoom(c)) {
                bounds.atLeast(-*room);
            }
            bounds.atLeast((p.index > 0 ? t.clips[p.index - 1].end() : Ticks{}) - c.start());
        } else {
            bounds.atLeast(minDuration - c.duration());
            if (const auto max = ed.maxDuration(c)) {
                bounds.atMost(*max - c.duration());
            }
            if (p.index + 1 < t.clips.size()) {
                bounds.atMost(t.clips[p.index + 1].start() - c.end());
            }
        }
    }
    const Ticks d = bounds.clamp(delta);
    if (d.value == 0) {
        return detail::unchanged(state);
    }
    for (const ClipRef& p : *parts) {
        Clip& c = ed.mutableClip(p.track, p.index);
        if (edge == Edge::Head) {
            c.moveHead(d);
        } else {
            c.moveTail(d);
        }
    }
    return ed.finish(std::nullopt, d);
}

// --- Ripple edit (B) ----------------------------------------------------------------------------------

namespace {

/// The ripple edit of `parts` (one edge per track; the first is the edit point other sync-locked tracks ripple at).
EditOutcome rippleParts(const ProjectState& state, const Sequence& sequence, const std::vector<ClipRef>* parts,
                        Edge edge, Ticks delta) {
    SequenceEditor ed(state, sequence);
    const Ticks minDuration = ed.oneFrame();
    DeltaBounds bounds;
    for (const ClipRef& p : *parts) {
        const Clip& c = ed.track(p.track).clips[p.index];
        if (edge == Edge::Head) {
            // Positive delta moves the In point later: the clip gets shorter, its start stays put.
            bounds.atMost(c.duration() - minDuration);
            if (const auto room = ed.headRoom(c)) {
                bounds.atLeast(-*room);
            }
        } else {
            bounds.atLeast(minDuration - c.duration());
            if (const auto max = ed.maxDuration(c)) {
                bounds.atMost(*max - c.duration());
            }
        }
    }
    const Ticks d = bounds.clamp(delta);
    if (d.value == 0) {
        return detail::unchanged(state);
    }

    const Clip& primary = ed.track((*parts)[0].track).clips[(*parts)[0].index];
    const Ticks editPoint = edge == Edge::Head ? primary.start() : primary.end();
    const Ticks growth = edge == Edge::Head ? -d : d; // change in the sequence's length

    for (const ClipRef& p : *parts) {
        Clip& c = ed.mutableClip(p.track, p.index);
        const Ticks oldEnd = c.end();
        if (edge == Edge::Head) {
            const Ticks start = c.start();
            c.moveHead(d); // the In point moves; a ripple keeps the clip's start where it is
            c.timelineRange.start = start;
        } else {
            c.moveTail(growth);
        }
        ed.shiftFrom(p.track, oldEnd, growth);
    }

    // Keep other sync-locked tracks in sync at the edit point.
    for (const TrackId id : ed.trackIds()) {
        const bool participant = std::any_of(parts->begin(), parts->end(), [&](const ClipRef& p) { return p.track == id; });
        const Track& t = ed.track(id);
        if (participant || t.locked || !t.syncLocked) {
            continue;
        }
        if (growth.value > 0) {
            ed.rippleInsert(id, editPoint, growth);
        } else if (edge == Edge::Head) {
            ed.rippleRemove(id, {editPoint, -growth});
        } else {
            ed.rippleRemove(id, TimeRange::fromStartEnd(editPoint + growth, editPoint));
        }
    }
    if (growth.value > 0) {
        ed.captionsInsert(editPoint, growth);
    } else if (edge == Edge::Head) {
        ed.captionsRemove({editPoint, -growth});
    } else {
        ed.captionsRemove(TimeRange::fromStartEnd(editPoint + growth, editPoint));
    }
    return ed.finish(std::nullopt, d);
}

} // namespace

EditOutcome rippleTrim(const ProjectState& state, SequenceId sequenceId, ClipId clipId, Edge edge, Ticks delta,
                       LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto parts = detail::edgeParticipants(**seq, clipId, edge, links);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    return rippleParts(state, **seq, &*parts, edge, delta);
}

EditOutcome rippleTrimEdges(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips, Edge edge,
                            Ticks delta, LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (clips.empty()) {
        return std::unexpected(invalidArgument("no clip to trim"));
    }
    std::vector<ClipRef> parts;
    for (const ClipId clip : clips) {
        const auto own = detail::edgeParticipants(**seq, clip, edge, links);
        if (!own) {
            return std::unexpected(own.error());
        }
        for (const ClipRef& p : *own) {
            const auto same = std::ranges::find_if(parts, [&](const ClipRef& q) { return q.track == p.track; });
            if (same == parts.end()) {
                parts.push_back(p);
            } else if (same->clip != p.clip) {
                return std::unexpected(invalidArgument("two of the edges are on one track"));
            }
        }
    }
    return rippleParts(state, **seq, &parts, edge, delta);
}

// --- Rolling edit (N) ---------------------------------------------------------------------------------

EditOutcome rollEdit(const ProjectState& state, SequenceId sequenceId, ClipId clipId, Edge edge, Ticks delta,
                     LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto parts = detail::edgeParticipants(**seq, clipId, edge, links);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    SequenceEditor ed(state, **seq);
    const Ticks minDuration = ed.oneFrame();
    DeltaBounds bounds;
    std::vector<bool> adjacent(parts->size(), false);
    for (std::size_t k = 0; k < parts->size(); ++k) {
        const ClipRef& p = (*parts)[k];
        const Track& t = ed.track(p.track);
        const Clip& c = t.clips[p.index];
        if (edge == Edge::Tail) {
            bounds.atLeast(minDuration - c.duration());
            if (const auto max = ed.maxDuration(c)) {
                bounds.atMost(*max - c.duration());
            }
            if (p.index + 1 < t.clips.size()) {
                const Clip& next = t.clips[p.index + 1];
                adjacent[k] = next.start() == c.end();
                if (adjacent[k]) {
                    bounds.atMost(next.duration() - minDuration);
                    if (const auto room = ed.headRoom(next)) {
                        bounds.atLeast(-*room);
                    }
                } else {
                    bounds.atMost(next.start() - c.end());
                }
            }
        } else {
            bounds.atMost(c.duration() - minDuration);
            if (const auto room = ed.headRoom(c)) {
                bounds.atLeast(-*room);
            }
            if (p.index > 0) {
                const Clip& prev = t.clips[p.index - 1];
                adjacent[k] = prev.end() == c.start();
                if (adjacent[k]) {
                    bounds.atLeast(minDuration - prev.duration());
                    if (const auto max = ed.maxDuration(prev)) {
                        bounds.atMost(*max - prev.duration());
                    }
                } else {
                    bounds.atLeast(prev.end() - c.start());
                }
            } else {
                bounds.atLeast(-c.start());
            }
        }
    }
    const Ticks d = bounds.clamp(delta);
    if (d.value == 0) {
        return detail::unchanged(state);
    }
    for (std::size_t k = 0; k < parts->size(); ++k) {
        const ClipRef& p = (*parts)[k];
        Clip& c = ed.mutableClip(p.track, p.index);
        if (edge == Edge::Tail) {
            c.moveTail(d);
            if (adjacent[k]) {
                ed.mutableClip(p.track, p.index + 1).moveHead(d);
            }
        } else {
            c.moveHead(d);
            if (adjacent[k]) {
                ed.mutableClip(p.track, p.index - 1).moveTail(d);
            }
        }
    }
    return ed.finish(std::nullopt, d);
}

// --- Slip (Y) -----------------------------------------------------------------------------------------

EditOutcome slip(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips, Ticks sourceDelta,
                 LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto selection = detail::resolveClips(**seq, clips, links);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    SequenceEditor ed(state, **seq);
    DeltaBounds bounds;
    for (const ClipRef& p : *selection) {
        const Clip& c = ed.track(p.track).clips[p.index];
        if (c.isRemapped()) {
            // Along the remap curve (speed 100 %): as far as the head and tail may move.
            bounds.atLeast(-*ed.headRoom(c));
            const SourceBounds source = ed.sourceBounds(c);
            if (const auto room = c.tailRoom(source.start, source.end)) {
                bounds.atMost(*room);
            }
            continue;
        }
        const SourceBounds source = ed.sourceBounds(c);
        bounds.atLeast(-(c.sourceIn - source.start));
        if (const auto extent = source.end) {
            // A freeze frame holds sourceIn itself, which must stay a frame of the media.
            bounds.atMost(*extent - c.sourceOut() - Ticks{c.isFreeze() ? 1 : 0});
        }
    }
    const Ticks d = bounds.clamp(sourceDelta);
    if (d.value == 0) {
        return detail::unchanged(state);
    }
    for (const ClipRef& p : *selection) {
        ed.mutableClip(p.track, p.index).sourceIn += d;
    }
    return ed.finish(std::nullopt, d);
}

// --- Slide (U) ----------------------------------------------------------------------------------------

EditOutcome slide(const ProjectState& state, SequenceId sequenceId, ClipId clipId, Ticks delta, LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto parts = detail::edgeParticipants(**seq, clipId, Edge::Head, links);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    SequenceEditor ed(state, **seq);
    const Ticks minDuration = ed.oneFrame();
    DeltaBounds bounds;
    std::vector<std::pair<bool, bool>> adjacent(parts->size(), {false, false}); // (previous, next)
    for (std::size_t k = 0; k < parts->size(); ++k) {
        const ClipRef& p = (*parts)[k];
        const Track& t = ed.track(p.track);
        const Clip& c = t.clips[p.index];
        if (p.index > 0) {
            const Clip& prev = t.clips[p.index - 1];
            adjacent[k].first = prev.end() == c.start();
            if (adjacent[k].first) {
                bounds.atLeast(minDuration - prev.duration());
                if (const auto max = ed.maxDuration(prev)) {
                    bounds.atMost(*max - prev.duration());
                }
            } else {
                bounds.atLeast(prev.end() - c.start());
            }
        } else {
            bounds.atLeast(-c.start());
        }
        if (p.index + 1 < t.clips.size()) {
            const Clip& next = t.clips[p.index + 1];
            adjacent[k].second = next.start() == c.end();
            if (adjacent[k].second) {
                bounds.atMost(next.duration() - minDuration);
                if (const auto room = ed.headRoom(next)) {
                    bounds.atLeast(-*room);
                }
            } else {
                bounds.atMost(next.start() - c.end());
            }
        }
    }
    const Ticks d = bounds.clamp(delta);
    if (d.value == 0) {
        return detail::unchanged(state);
    }
    for (std::size_t k = 0; k < parts->size(); ++k) {
        const ClipRef& p = (*parts)[k];
        if (adjacent[k].first) {
            ed.mutableClip(p.track, p.index - 1).moveTail(d);
        }
        ed.mutableClip(p.track, p.index).timelineRange.start += d;
        if (adjacent[k].second) {
            ed.mutableClip(p.track, p.index + 1).moveHead(d);
        }
    }
    return ed.finish(std::nullopt, d);
}

} // namespace wabcut::edit
