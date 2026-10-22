// Fade handles (ClipFades.h): a clip's fade is its one-sided transition at that edge.

#include <wabcut/model/ClipFades.h>

#include <wabcut/model/FinishingOps.h>

#include "SequenceEditor.h"

#include <algorithm>

namespace wabcut::edit {

namespace {

/// The transition touching the clip's edge (one- or two-sided), or null.
const Transition* transitionAtClipEdge(const Track& track, ClipId clip, Edge edge) {
    for (const Transition& t : track.transitions) {
        if ((edge == Edge::Head && t.incoming == clip) || (edge == Edge::Tail && t.outgoing == clip)) {
            return &t;
        }
    }
    return nullptr;
}

bool oneSided(const Transition& t) { return !t.outgoing.isValid() || !t.incoming.isValid(); }

} // namespace

std::optional<Ticks> clipFade(const Sequence& sequence, ClipId clip, Edge edge) {
    const auto loc = sequence.findClip(clip);
    if (!loc) {
        return std::nullopt;
    }
    const Transition* t = transitionAtClipEdge(*loc->track, clip, edge);
    if (!t || !oneSided(*t)) {
        return std::nullopt;
    }
    return t->duration();
}

EditOutcome setClipFade(const ProjectState& state, SequenceId sequenceId, ClipId clipId, Edge edge, Ticks duration) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto loc = (*seq)->findClip(clipId);
    if (!loc) {
        return std::unexpected(detail::notFound("clip " + std::to_string(clipId.value) + " is not in the sequence"));
    }
    const Track& track = *loc->track;
    if (track.locked) {
        return std::unexpected(detail::invalidArgument("track " + detail::trackLabel(**seq, track.id) + " is locked"));
    }
    if (duration.value < 0) {
        return std::unexpected(detail::invalidArgument("a fade cannot be shorter than 0"));
    }
    const Clip& clip = loc->clip();
    duration = std::min(duration, clip.duration());
    const Transition* existing = transitionAtClipEdge(track, clipId, edge);
    if (existing && !oneSided(*existing)) {
        return std::unexpected(detail::invalidArgument(
            std::string("the clip's ") + (edge == Edge::Head ? "In" : "Out") +
            " point has a transition with the next clip; change that transition instead"));
    }
    if (duration.value == 0) {
        return existing ? removeTransition(state, sequenceId, existing->id) : EditOutcome(detail::unchanged(state));
    }
    if (existing) {
        if (existing->duration() == duration) {
            return detail::unchanged(state);
        }
        TransitionChange change;
        if (edge == Edge::Head) {
            change.before = Ticks{};
            change.after = duration;
        } else {
            change.before = duration;
            change.after = Ticks{};
        }
        return setTransition(state, sequenceId, existing->id, change);
    }
    TransitionSpec spec;
    if (edge == Edge::Head) {
        spec.incoming = clipId;
        spec.after = duration;
    } else {
        spec.outgoing = clipId;
        spec.before = duration;
    }
    spec.effect = track.kind == TrackKind::Audio ? "constantPower" : "crossDissolve";
    return addTransition(state, sequenceId, track.id, spec);
}

} // namespace wabcut::edit
