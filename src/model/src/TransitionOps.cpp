// Transition operations: add / change / remove on edit points, and Premiere's default-transition commands.

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <tuple>

namespace wabcut::edit {

using detail::SequenceEditor;

namespace {

/// "1 frame" / "12 frames" at the sequence rate (rounded down), for messages.
std::string framesText(Ticks t, Rate rate) {
    const std::int64_t frames = ticksToFrames(t, rate);
    return std::to_string(frames) + (frames == 1 ? " frame" : " frames");
}

SourceBounds boundsOf(const ProjectState& state, const Clip& clip) {
    // Media: the stream's media, or a restricted subclip's range (M5k). Nested sequences: their duration (C.8).
    if (!clip.isNested() && !clip.hasUnknownSource()) {
        if (const MediaItem* media = state.findMedia(clip.media)) {
            return media->sourceBounds(clip.stream);
        }
    }
    return SourceBounds{Ticks{}, clipSourceExtent(state, clip)};
}

/// The track holding a transition, and its position in the track's list.
struct TransitionSite {
    TrackId track;
    std::size_t index = 0;
};

std::optional<TransitionSite> findTransition(const Sequence& seq, TransitionId id) {
    std::optional<TransitionSite> found;
    seq.forEachTrack([&](const Track& t) {
        for (std::size_t i = 0; i < t.transitions.size(); ++i) {
            if (t.transitions[i].id == id) {
                found = TransitionSite{t.id, i};
            }
        }
    });
    return found;
}

TimeRange regionOf(const Track& track, const Transition& t) {
    const Ticks cut = transitionEditPoint(track, t).value_or(Ticks{});
    return TimeRange::fromStartEnd(cut - t.before, cut + t.after);
}

/// Checks the transition's effect and parameters, fits its sides to the handles and checks its shape,
/// handles and neighbours on `track` (ignoring the transition at `skip`), and settles its alignment. Returns the
/// transition to store.
std::expected<Transition, Error> prepare(const ProjectState& state, const Sequence& seq, const Track& track,
                                         Transition t, HandleFit fit, std::optional<std::size_t> skip) {
    const EffectKind kind = track.kind == TrackKind::Video ? EffectKind::VideoTransition : EffectKind::AudioTransition;
    const EffectDescriptor* d = EffectRegistry::builtin().find(t.effect);
    if (!d || (d->kind != EffectKind::VideoTransition && d->kind != EffectKind::AudioTransition)) {
        return std::unexpected(detail::notFound("unknown transition \"" + t.effect + "\""));
    }
    if (d->kind != kind) {
        return std::unexpected(detail::invalidArgument(t.effect + " is a " + std::string(toString(d->kind)) + "; " +
                                                       detail::trackLabel(seq, track.id) + " needs a " +
                                                       std::string(toString(kind))));
    }
    std::sort(t.params.begin(), t.params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    for (std::size_t i = 0; i < t.params.size(); ++i) {
        const EffectParam& p = t.params[i];
        const EffectParamInfo* info = d->param(p.key);
        if (!info) {
            return std::unexpected(detail::notFound(t.effect + " has no parameter \"" + p.key + "\""));
        }
        if ((i > 0 && t.params[i - 1].key == p.key) || !p.value.isConstant() ||
            !p.value.isValid(info->minValue, info->maxValue)) {
            return std::unexpected(detail::invalidArgument(t.effect + " " + p.key +
                                                           " must be a single constant within its range"));
        }
    }
    if (t.before.value < 0 || t.after.value < 0 || t.duration().value <= 0) {
        return std::unexpected(detail::invalidArgument("a transition needs a positive duration"));
    }
    // Fit to the media around the cut (two-sided only: a one-sided transition lies inside its clip).
    const Clip* a = t.outgoing.isValid() ? track.findClip(t.outgoing) : nullptr;
    const Clip* b = t.incoming.isValid() ? track.findClip(t.incoming) : nullptr;
    if ((t.outgoing.isValid() && !a) || (t.incoming.isValid() && !b)) {
        return std::unexpected(detail::notFound("the transition's clips are not on " + detail::trackLabel(seq, track.id)));
    }
    if (fit == HandleFit::UseAvailable) {
        // A side never covers more than its clip.
        if (a) {
            t.before = std::min(t.before, a->duration());
        }
        if (b) {
            t.after = std::min(t.after, b->duration());
        }
    }
    if (a && b) {
        const SourceBounds aBounds = boundsOf(state, *a);
        const SourceBounds bBounds = boundsOf(state, *b);
        const auto tailRoom = a->tailRoom(aBounds.start, aBounds.end);
        const auto headRoom = b->headRoom(bBounds.start, bBounds.end);
        const bool tailShort = tailRoom && t.after > *tailRoom;
        const bool headShort = headRoom && t.before > *headRoom;
        if ((tailShort || headShort) && fit == HandleFit::Exact) {
            std::string message = "not enough media for the transition: ";
            if (tailShort) {
                message += "'" + a->name() + "' has " + framesText(*tailRoom, seq.frameRate) + " after its Out point, " +
                           framesText(t.after, seq.frameRate) + " needed";
            }
            if (headShort) {
                message += std::string(tailShort ? "; " : "") + "'" + b->name() + "' has " +
                           framesText(*headRoom, seq.frameRate) + " before its In point, " +
                           framesText(t.before, seq.frameRate) + " needed";
            }
            return std::unexpected(detail::invalidArgument(message + " (or let it use the available handles)"));
        }
        // Premiere's "insufficient media" adjustment: shorten each side to whole frames of the media there is.
        if (tailShort) {
            t.after = snapToFrame(*tailRoom, seq.frameRate);
        }
        if (headShort) {
            t.before = snapToFrame(*headRoom, seq.frameRate);
        }
        if (t.duration().value <= 0) {
            return std::unexpected(detail::invalidArgument("no media beyond the cut: '" + a->name() + "' ends and '" +
                                                           b->name() + "' starts at the ends of their media"));
        }
    }
    // One-sided transitions end (or start) at their clip's edge; fitted sides keep the alignment only if they
    // still match it.
    const Ticks frame = framesToTicks(1, seq.frameRate);
    if (!b) {
        t.alignment = TransitionAlignment::EndAtCut;
    } else if (!a) {
        t.alignment = TransitionAlignment::StartAtCut;
    } else if (!alignmentFits(t.alignment, t.before, t.after, frame)) {
        t.alignment = alignmentOf(t.before, t.after, frame);
    }
    if (const auto problem = transitionProblem(state, track, t, Validation::Strict)) {
        return std::unexpected(detail::invalidArgument("the transition " + *problem));
    }
    const TimeRange region = regionOf(track, t);
    const Ticks cut = *transitionEditPoint(track, t);
    for (std::size_t i = 0; i < track.transitions.size(); ++i) {
        if (skip && *skip == i) {
            continue;
        }
        const Transition& other = track.transitions[i];
        const bool sameEdit = transitionEditPoint(track, other) == cut &&
                              (other.outgoing == t.outgoing || other.incoming == t.incoming);
        if (sameEdit) {
            return std::unexpected(detail::invalidArgument("that edit point already has transition " +
                                                           std::to_string(other.id.value)));
        }
        if (regionOf(track, other).overlaps(region)) {
            return std::unexpected(detail::invalidArgument("the transition would overlap transition " +
                                                           std::to_string(other.id.value)));
        }
    }
    return t;
}

void insertSorted(Track& track, Transition t) {
    const Ticks cut = transitionEditPoint(track, t).value_or(Ticks{});
    const auto it = std::find_if(track.transitions.begin(), track.transitions.end(), [&](const Transition& other) {
        return transitionEditPoint(track, other).value_or(Ticks{}) > cut;
    });
    track.transitions.insert(it, std::move(t));
}

Ticks oneSecond(Rate rate) {
    const std::int64_t nominal = (static_cast<std::int64_t>(rate.num) + rate.den / 2) / rate.den;
    return framesToTicks(std::max<std::int64_t>(nominal, 1), rate);
}

/// The result of several edits in a row, with one ChangeSet against the first state.
EditResult combined(const ProjectState& first, EditResult last, std::vector<TransitionId> created) {
    last.changes = diff(first, last.state);
    last.createdTransitions = std::move(created);
    return last;
}

} // namespace

EditOutcome addTransition(const ProjectState& state, SequenceId sequenceId, TrackId trackId, const TransitionSpec& spec,
                          HandleFit fit) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Track* track = (*seq)->findTrack(trackId);
    if (!track) {
        return std::unexpected(detail::notFound("track " + std::to_string(trackId.value) + " does not exist"));
    }
    if (track->locked) {
        return std::unexpected(detail::invalidArgument("track " + detail::trackLabel(**seq, trackId) + " is locked"));
    }
    Transition t;
    t.outgoing = spec.outgoing;
    t.incoming = spec.incoming;
    t.before = spec.before;
    t.after = spec.after;
    t.effect = spec.effect;
    t.params = spec.params;
    if (!spec.alignment) {
        t.alignment = alignmentOf(t.before, t.after, framesToTicks(1, (*seq)->frameRate));
    } else {
        t.alignment = *spec.alignment;
        if (spec.outgoing.isValid() && spec.incoming.isValid() && *spec.alignment != TransitionAlignment::Custom) {
            std::tie(t.before, t.after) = splitTransition(t.duration(), *spec.alignment, t.before, (*seq)->frameRate);
        }
    }
    auto ready = prepare(state, **seq, *track, std::move(t), fit, std::nullopt);
    if (!ready) {
        return std::unexpected(ready.error());
    }
    SequenceEditor ed(state, **seq);
    ready->id = ed.newTransitionId();
    insertSorted(ed.mutableTrack(trackId), std::move(*ready));
    return ed.finish();
}

EditOutcome setTransition(const ProjectState& state, SequenceId sequenceId, TransitionId id,
                          const TransitionChange& change, HandleFit fit) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto site = findTransition(**seq, id);
    if (!site) {
        return std::unexpected(detail::notFound("transition " + std::to_string(id.value) + " does not exist"));
    }
    const Track& track = *(*seq)->findTrack(site->track);
    if (track.locked) {
        return std::unexpected(detail::invalidArgument("track " + detail::trackLabel(**seq, site->track) + " is locked"));
    }
    Transition t = track.transitions[site->index];
    const Rate rate = (*seq)->frameRate;
    if (change.before || change.after) {
        t.before = change.before.value_or(t.before);
        t.after = change.after.value_or(t.after);
        t.alignment = change.alignment == TransitionAlignment::Custom
                          ? TransitionAlignment::Custom
                          : alignmentOf(t.before, t.after, framesToTicks(1, rate));
    } else if (change.duration || change.alignment) {
        t.alignment = change.alignment.value_or(t.alignment);
        const Ticks duration = change.duration.value_or(t.duration());
        if (!t.outgoing.isValid() || !t.incoming.isValid()) {
            // One-sided: the length stays on the clip's side of the cut.
            t.before = t.incoming.isValid() ? Ticks{} : duration;
            t.after = duration - t.before;
        } else {
            std::tie(t.before, t.after) = splitTransition(duration, t.alignment, t.before, rate);
        }
    }
    t.effect = change.effect.value_or(t.effect);
    if (change.params) {
        t.params = *change.params;
    }
    auto ready = prepare(state, **seq, track, std::move(t), fit, site->index);
    if (!ready) {
        return std::unexpected(ready.error());
    }
    if (*ready == track.transitions[site->index]) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    Track& mt = ed.mutableTrack(site->track);
    mt.transitions.erase(mt.transitions.begin() + static_cast<std::ptrdiff_t>(site->index));
    insertSorted(mt, std::move(*ready));
    return ed.finish();
}

EditOutcome removeTransition(const ProjectState& state, SequenceId sequenceId, TransitionId id) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto site = findTransition(**seq, id);
    if (!site) {
        return std::unexpected(detail::notFound("transition " + std::to_string(id.value) + " does not exist"));
    }
    if ((*seq)->findTrack(site->track)->locked) {
        return std::unexpected(detail::invalidArgument("track " + detail::trackLabel(**seq, site->track) + " is locked"));
    }
    SequenceEditor ed(state, **seq);
    Track& mt = ed.mutableTrack(site->track);
    mt.transitions.erase(mt.transitions.begin() + static_cast<std::ptrdiff_t>(site->index));
    return ed.finish();
}

std::expected<EdgeTransition, Error> transitionAtEdge(const ProjectState& state, SequenceId sequenceId, ClipId clipId,
                                                      Edge edge, Ticks duration, TransitionAlignment alignment,
                                                      std::string effect) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto loc = (*seq)->findClip(clipId);
    if (!loc) {
        return std::unexpected(detail::notFound("clip " + std::to_string(clipId.value) + " is not in this sequence"));
    }
    if (duration.value <= 0) {
        return std::unexpected(detail::invalidArgument("a transition needs a positive duration"));
    }
    const Track& track = *loc->track;
    const Clip& clip = loc->clip();
    EdgeTransition out{track.id, {}};
    out.spec.effect = std::move(effect);
    const Clip* neighbour = nullptr;
    if (edge == Edge::Tail && loc->index + 1 < track.clips.size() && track.clips[loc->index + 1].start() == clip.end()) {
        neighbour = &track.clips[loc->index + 1];
    }
    if (edge == Edge::Head && loc->index > 0 && track.clips[loc->index - 1].end() == clip.start()) {
        neighbour = &track.clips[loc->index - 1];
    }
    if (!neighbour) {
        // One-sided, inside the clip: "to black" at its tail, "from black" at its head.
        if (edge == Edge::Tail) {
            out.spec.outgoing = clip.id;
            out.spec.before = duration;
        } else {
            out.spec.incoming = clip.id;
            out.spec.after = duration;
        }
        return out;
    }
    out.spec.outgoing = edge == Edge::Tail ? clip.id : neighbour->id;
    out.spec.incoming = edge == Edge::Tail ? neighbour->id : clip.id;
    out.spec.alignment = alignment;
    std::tie(out.spec.before, out.spec.after) =
        splitTransition(duration, alignment, snapToFrame(Ticks{duration.value / 2}, (*seq)->frameRate),
                        (*seq)->frameRate);
    return out;
}

EditOutcome applyDefaultTransition(const ProjectState& state, SequenceId sequenceId, Ticks playhead, TrackKind kind,
                                   const DefaultTransitions& defaults) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const bool video = kind == TrackKind::Video;
    const Ticks duration = (video ? defaults.videoDuration : defaults.audioDuration).value_or(oneSecond((*seq)->frameRate));
    const std::string& effect = video ? defaults.video : defaults.audio;
    EditResult current = detail::unchanged(state);
    std::vector<TransitionId> created;
    std::string reasons;
    bool anyTrack = false;
    for (const TrackId id : detail::unlockedTargets(**seq)) {
        const Track& track = *(*seq)->findTrack(id);
        if (track.kind != kind || track.clips.empty()) {
            continue;
        }
        anyTrack = true;
        // The edit point (a clip start or end) nearest the playhead; the earlier one on a tie.
        std::optional<std::pair<Ticks, std::size_t>> best; // (edit point, index of a clip with an edge there)
        Edge bestEdge = Edge::Head;
        std::size_t i = 0;
        for (const Clip& c : track.clips) {
            for (const auto& [point, edge] : {std::pair{c.start(), Edge::Head}, std::pair{c.end(), Edge::Tail}}) {
                const auto distance = [&](Ticks p) { return p > playhead ? p - playhead : playhead - p; };
                if (!best || distance(point) < distance(best->first) ||
                    (distance(point) == distance(best->first) && point < best->first)) {
                    best = std::pair{point, i};
                    bestEdge = edge;
                }
            }
            ++i;
        }
        const Clip& clip = track.clips[best->second];
        auto edgeSpec = transitionAtEdge(current.state, sequenceId, clip.id, bestEdge, duration, defaults.alignment, effect);
        if (!edgeSpec) {
            reasons += "; " + detail::trackLabel(**seq, id) + ": " + edgeSpec.error().message;
            continue;
        }
        // A point where one clip ends and the next starts becomes a two-sided transition (transitionAtEdge).
        auto next = addTransition(current.state, sequenceId, id, edgeSpec->spec, HandleFit::UseAvailable);
        if (!next) {
            reasons += "; " + detail::trackLabel(**seq, id) + ": " + next.error().message;
            continue;
        }
        created.insert(created.end(), next->createdTransitions.begin(), next->createdTransitions.end());
        current = std::move(*next);
    }
    if (created.empty()) {
        if (!anyTrack) {
            return std::unexpected(detail::invalidArgument(std::string("no targeted ") + (video ? "video" : "audio") +
                                                           " track has clips"));
        }
        return std::unexpected(detail::invalidArgument("no transition was added" + reasons));
    }
    return combined(state, std::move(current), std::move(created));
}

EditOutcome applyDefaultTransitions(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips,
                                    const DefaultTransitions& defaults) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto refs = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    const Ticks second = oneSecond((*seq)->frameRate);
    EditResult current = detail::unchanged(state);
    std::vector<TransitionId> created;
    for (const detail::ClipRef& r : *refs) {
        const bool video = (*seq)->findTrack(r.track)->kind == TrackKind::Video;
        const Ticks duration = (video ? defaults.videoDuration : defaults.audioDuration).value_or(second);
        for (const Edge edge : {Edge::Head, Edge::Tail}) {
            auto spec = transitionAtEdge(current.state, sequenceId, r.clip, edge, duration, defaults.alignment,
                                         video ? defaults.video : defaults.audio);
            if (!spec) {
                continue;
            }
            // An edit point shared by two selected clips gets one transition: the second attempt is refused.
            auto next = addTransition(current.state, sequenceId, spec->track, spec->spec, HandleFit::UseAvailable);
            if (next) {
                created.insert(created.end(), next->createdTransitions.begin(), next->createdTransitions.end());
                current = std::move(*next);
            }
        }
    }
    if (created.empty()) {
        return std::unexpected(detail::invalidArgument("no transition could be added to the selected clips"));
    }
    return combined(state, std::move(current), std::move(created));
}

} // namespace wabcut::edit
