// Leaf media of nested and multicam clips (Resolve.h).

#include <wabcut/model/Resolve.h>

#include <wabcut/model/RedactionMedia.h>
#include <wabcut/model/Synthetic.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace wabcut {

namespace {

struct Active {
    const Clip* clip = nullptr;
    TransitionUse transition;
};

/// The clip of `track` with id `id`, looked up near time `near` (a transition's edit point).
const Clip* clipNear(const Track& track, ClipId id, Ticks near) {
    const std::size_t upper = track.clips.upperBoundStart(near);
    const std::size_t first = upper >= 2 ? upper - 2 : 0;
    for (std::size_t i = first; i < std::min(track.clips.size(), upper + 1); ++i) {
        if (track.clips[i].id == id) {
            return &track.clips[i];
        }
    }
    return nullptr;
}

/// The clips of `track` active at `t`: both clips of a transition region that holds `t`, else the clip holding it.
std::vector<Active> activeAt(const Track& track, Ticks t) {
    std::vector<Active> out;
    for (const Transition& tr : track.transitions) {
        const auto cut = transitionEditPoint(track, tr);
        if (!cut || tr.duration().value <= 0 || t < *cut - tr.before || t >= *cut + tr.after) {
            continue;
        }
        const double progress = static_cast<double>((t - (*cut - tr.before)).value) / static_cast<double>(tr.duration().value);
        for (const auto& [id, incoming] : {std::pair{tr.outgoing, false}, std::pair{tr.incoming, true}}) {
            if (const Clip* c = id.isValid() ? clipNear(track, id, *cut) : nullptr; c && c->enabled) {
                out.push_back({c, {&tr, incoming, progress}});
            }
        }
        return out;
    }
    const std::size_t upper = track.clips.upperBoundStart(t);
    if (upper > 0) {
        const Clip& c = track.clips[upper - 1];
        if (t < c.end() && c.enabled) {
            out.push_back({&c, {}});
        }
    }
    return out;
}

/// Whether a track's output is heard / seen (Resolve.h).
bool plays(const Track& track, bool anySolo) {
    if (track.kind == TrackKind::Video) {
        return track.enabled;
    }
    return track.enabled && !track.muted && (!anySolo || track.solo);
}

bool anySoloed(const std::vector<TrackPtr>& tracks) {
    return std::any_of(tracks.begin(), tracks.end(), [](const TrackPtr& t) { return t->kind == TrackKind::Audio && t->solo; });
}

std::vector<RedactionId> redactionsFor(const ProjectState& state, const ResolvedLeaf& leaf) {
    std::vector<RedactionId> out;
    const Clip& clip = *leaf.clip;
    const bool video = clip.stream.kind == StreamKind::Video;
    // Subclips and stems share their master's / source's redactions (RedactionMedia.h).
    for (const Redaction* covering : redactionsCovering(state, clip.media)) {
        const Redaction& r = *covering;
        if (!r.enabled || !r.appliesTo(clip.id) ||
            std::any_of(leaf.path.begin(), leaf.path.end(), [&](const NestLevel& l) { return !r.appliesTo(l.clip->id); })) {
            continue;
        }
        bool hit = false;
        if (video) {
            hit = r.source.contains(leaf.sourceTime);
        } else if (r.audio != RedactionAudio::None) {
            hit = r.audioRanges.empty() ? r.source.contains(leaf.sourceTime)
                                        : std::any_of(r.audioRanges.begin(), r.audioRanges.end(),
                                                      [&](const TimeRange& w) { return w.contains(leaf.sourceTime); });
        }
        if (hit) {
            out.push_back(r.id);
        }
    }
    return out;
}

/// Clips one resolution may visit, nested ones included: far beyond any real timeline. Past it nothing more resolves (a
/// crafted project of doubling nests stops here instead of expanding 2^depth leaves; review wave 5 E5-P2-9).
constexpr std::size_t kMaxResolveVisits = 4096;

void descend(const ProjectState& state, SequenceId sequence, const Track& track, std::size_t trackIndex, const Clip& clip,
             Ticks t, TransitionUse use, std::vector<NestLevel>& path, std::vector<ResolvedLeaf>& out, std::size_t& visits) {
    if (!clip.enabled || clip.hasUnknownSource() || ++visits > kMaxResolveVisits) {
        return;
    }
    if (!clip.isNested()) {
        ResolvedLeaf leaf;
        leaf.sequence = sequence;
        leaf.track = track.id;
        leaf.trackIndex = trackIndex;
        leaf.clip = &clip;
        leaf.time = t;
        leaf.sourceTime = clip.sourceTimeAt(t);
        leaf.curveTime = clip.curveTimeAt(t);
        leaf.transition = use;
        leaf.path = path;
        leaf.redactions = redactionsFor(state, leaf);
        out.push_back(std::move(leaf));
        return;
    }
    if (path.size() >= kMaxNestingDepth) {
        return;
    }
    const Sequence* nested = state.findSequence(clip.sequence);
    if (!nested) {
        return;
    }
    const Ticks inner = clip.sourceTimeAt(t);
    if (inner.value < 0 || inner >= nested->duration) {
        return; // past the nested sequence's end: nothing
    }
    path.push_back({sequence, track.id, trackIndex, &clip, t, inner, clip.curveTimeAt(t), use});
    const TrackKind kind = clip.stream.kind == StreamKind::Video ? TrackKind::Video : TrackKind::Audio;
    const std::vector<TrackPtr>& tracks = nested->tracks(kind);
    for (const std::size_t i : nestedTracksRead(*nested, clip)) {
        const Track& inside = *tracks[i];
        for (const Active& a : activeAt(inside, inner)) {
            descend(state, nested->id, inside, i, *a.clip, inner, a.transition, path, out, visits);
        }
    }
    path.pop_back();
}

} // namespace

std::vector<std::size_t> nestedTracksRead(const Sequence& nested, const Clip& clip) {
    std::vector<std::size_t> read;
    if (!clip.isNested()) {
        return read;
    }
    const TrackKind kind = clip.stream.kind == StreamKind::Video ? TrackKind::Video : TrackKind::Audio;
    const std::vector<TrackPtr>& tracks = nested.tracks(kind);
    if (nested.multicam) {
        // Only the active angles' tracks, whatever their flags.
        for (std::size_t a = 0; a < nested.multicam->angles.size(); ++a) {
            if (!clip.angles.contains(a)) {
                continue;
            }
            const MulticamAngle& angle = nested.multicam->angles[a];
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                const TrackId id = tracks[i]->id;
                if (kind == TrackKind::Video ? id == angle.video
                                             : std::find(angle.audio.begin(), angle.audio.end(), id) != angle.audio.end()) {
                    read.push_back(i);
                }
            }
        }
        std::sort(read.begin(), read.end());
        read.erase(std::unique(read.begin(), read.end()), read.end());
    } else {
        const bool solo = kind == TrackKind::Audio && anySoloed(tracks);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (plays(*tracks[i], solo)) {
                read.push_back(i);
            }
        }
    }
    return read;
}

std::vector<ResolvedLeaf> resolveLeaves(const ProjectState& state, SequenceId sequence, const Clip& clip, Ticks t) {
    std::vector<ResolvedLeaf> out;
    const Sequence* seq = state.findSequence(sequence);
    const auto loc = seq ? seq->findClip(clip.id) : std::nullopt;
    if (!loc) {
        return out;
    }
    const auto pos = seq->trackPosition(loc->track->id);
    std::vector<NestLevel> path;
    std::size_t visits = 0;
    descend(state, sequence, *loc->track, pos->index, clip, t, {}, path, out, visits);
    return out;
}

std::vector<ResolvedLeaf> resolveSequenceLeaves(const ProjectState& state, SequenceId sequence, StreamKind kind, Ticks t) {
    std::vector<ResolvedLeaf> out;
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return out;
    }
    const std::vector<TrackPtr>& tracks = seq->tracks(kind == StreamKind::Video ? TrackKind::Video : TrackKind::Audio);
    const bool solo = kind == StreamKind::Audio && anySoloed(tracks);
    std::vector<NestLevel> path;
    std::size_t visits = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (!plays(*tracks[i], solo)) {
            continue;
        }
        for (const Active& a : activeAt(*tracks[i], t)) {
            descend(state, sequence, *tracks[i], i, *a.clip, t, a.transition, path, out, visits);
        }
    }
    return out;
}

// ---- The composite plan (C.15) -------------------------------------------------------------------------------------

namespace {

struct PlanContext {
    const ProjectState& state;
    std::size_t visits = 0;
    bool truncated = false;
};

/// The clip's first enabled, evaluable Track Matte Key.
const EffectInstance* trackMatteOf(const Clip& clip) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.enabled && e.effect == kTrackMatteEffect && isEvaluable(e, EffectKind::Video)) {
            return &e;
        }
    }
    return nullptr;
}

std::shared_ptr<const CompositePlan> planTracks(PlanContext& ctx, const Sequence& seq, Ticks t,
                                                std::span<const std::size_t> tracks, std::vector<NestLevel>& path,
                                                bool master, bool mattes);

void planClip(PlanContext& ctx, const Sequence& seq, const Track& track, std::size_t trackIndex, const Clip& clip, Ticks t,
              TransitionUse use, std::vector<NestLevel>& path, bool mattes, CompositePlan& plan) {
    if (!clip.enabled || clip.hasUnknownSource()) {
        return;
    }
    if (++ctx.visits > kMaxResolveVisits) {
        ctx.truncated = true;
        return;
    }
    CompositeStep step;
    step.sequence = seq.id;
    step.track = track.id;
    step.trackIndex = trackIndex;
    step.clip = &clip;
    step.time = t;
    step.sourceTime = clip.sourceTimeAt(t);
    step.curveTime = clip.curveTimeAt(t);
    step.transition = use;
    step.opacity = std::clamp(clip.properties().value(ParamId::Opacity, step.curveTime), 0.0, 1.0);
    step.blend = clip.blendMode();
    if (const MediaItem* item = clip.isNested() ? nullptr : ctx.state.findMedia(clip.media); item && isAdjustmentLayer(*item)) {
        step.kind = CompositeStepKind::Adjust;
        plan.steps.push_back(std::move(step));
        return;
    }
    if (clip.isNested()) {
        const Sequence* nested = ctx.state.findSequence(clip.sequence);
        if (!nested || path.size() >= kMaxNestingDepth || step.sourceTime.value < 0 || step.sourceTime >= nested->duration) {
            return; // past the nested sequence's end (or broken): nothing, as resolveLeaves
        }
        path.push_back({seq.id, track.id, trackIndex, &clip, t, step.sourceTime, step.curveTime, use});
        const std::vector<std::size_t> read = nestedTracksRead(*nested, clip);
        step.nested = planTracks(ctx, *nested, step.sourceTime, read, path, !nested->multicam.has_value(), true);
        path.pop_back();
    } else {
        ResolvedLeaf leaf;
        leaf.sequence = seq.id;
        leaf.track = track.id;
        leaf.trackIndex = trackIndex;
        leaf.clip = &clip;
        leaf.time = t;
        leaf.sourceTime = step.sourceTime;
        leaf.curveTime = step.curveTime;
        leaf.path = path;
        step.redactions = redactionsFor(ctx.state, leaf);
    }
    if (const EffectInstance* key = mattes ? trackMatteOf(clip) : nullptr) {
        MatteUse m;
        m.effect = key;
        const double number = effectParameterAt(*key, "matteTrack", step.curveTime, 0.0);
        m.trackNumber = std::isfinite(number) ? static_cast<int>(std::llround(std::clamp(number, 0.0, 999.0))) : 0;
        m.luma = effectParameterAt(*key, "compositeUsing", step.curveTime) >= 0.5;
        m.reverse = effectParameterAt(*key, "reverse", step.curveTime) >= 0.5;
        const auto index = static_cast<std::size_t>(std::max(m.trackNumber, 1) - 1);
        if (m.trackNumber >= 1 && index < seq.videoTracks.size() && index != trackIndex) {
            m.track = seq.videoTracks[index]->id;
            const std::array<std::size_t, 1> one{index};
            // The matte track's own clips; their own mattes are not followed (nest to stack mattes).
            m.plan = planTracks(ctx, seq, t, one, path, false, false);
        }
        step.matte = std::move(m);
    }
    plan.steps.push_back(std::move(step));
}

std::shared_ptr<const CompositePlan> planTracks(PlanContext& ctx, const Sequence& seq, Ticks t,
                                                std::span<const std::size_t> tracks, std::vector<NestLevel>& path,
                                                bool master, bool mattes) {
    auto plan = std::make_shared<CompositePlan>();
    plan->sequence = seq.id;
    plan->time = t;
    for (const std::size_t i : tracks) {
        if (i >= seq.videoTracks.size()) {
            continue;
        }
        const Track& track = *seq.videoTracks[i];
        for (const Active& a : activeAt(track, t)) {
            planClip(ctx, seq, track, i, *a.clip, t, a.transition, path, mattes, *plan);
        }
    }
    if (master && !seq.effects.empty()) {
        plan->masterEffects = &seq.effects;
    }
    plan->truncated = ctx.truncated;
    return plan;
}

} // namespace

std::size_t CompositePlan::stepCount() const {
    std::size_t n = 0;
    for (const CompositeStep& s : steps) {
        ++n;
        if (s.nested) {
            n += s.nested->stepCount();
        }
        if (s.matte && s.matte->plan) {
            n += s.matte->plan->stepCount();
        }
    }
    return n;
}

CompositePlan resolveComposite(const ProjectState& state, SequenceId sequence, Ticks t) {
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        CompositePlan empty;
        empty.sequence = sequence;
        empty.time = t;
        return empty;
    }
    std::vector<std::size_t> read;
    for (std::size_t i = 0; i < seq->videoTracks.size(); ++i) {
        if (seq->videoTracks[i]->enabled) {
            read.push_back(i);
        }
    }
    PlanContext ctx{state};
    std::vector<NestLevel> path;
    CompositePlan plan = *planTracks(ctx, *seq, t, read, path, false, true);
    plan.truncated = ctx.truncated;
    return plan;
}

} // namespace wabcut
