// Parameter, keyframe and effect-stack operations. Each touches one clip's content block (copy-on-write), one
// track's stack or one of the sequence's master stacks.

#include <wabcut/model/FinishingOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

namespace wabcut::edit {

using detail::ClipRef;
using detail::SequenceEditor;

namespace {

constexpr double kUnbounded = std::numeric_limits<double>::max();

/// A clip on an unlocked track of the sequence.
std::expected<ClipRef, Error> editableClip(const Sequence& seq, ClipId clip) {
    const auto refs = detail::resolveClips(seq, std::span<const ClipId>(&clip, 1), LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    return refs->front();
}

bool isVideoParam(ParamId id) {
    const PropertyRole role = parameterInfo(id).role;
    return role == PropertyRole::Motion || role == PropertyRole::Opacity;
}

/// The range and animatability of a parameter of `content` on a clip of `kind`.
struct ParamSlot {
    double minValue = -kUnbounded;
    double maxValue = kUnbounded;
    bool animatable = true;
    ParamValue current;
};

std::expected<ParamSlot, Error> effectSlotOf(const EffectStack& stack, const ParamRef& ref, std::string_view owner);

std::expected<ParamSlot, Error> slotOf(const ClipContent& content, TrackKind kind, const ParamRef& ref) {
    if (ref.intrinsic) {
        const ParameterInfo& info = parameterInfo(*ref.intrinsic);
        if (isVideoParam(*ref.intrinsic) != (kind == TrackKind::Video)) {
            return std::unexpected(detail::invalidArgument(
                std::string(info.key) + " is a " + (isVideoParam(*ref.intrinsic) ? "video" : "audio") +
                " parameter; the clip is a " + (kind == TrackKind::Video ? "video" : "audio") + " clip"));
        }
        return ParamSlot{info.minValue, info.maxValue, true, content.properties[*ref.intrinsic]};
    }
    return effectSlotOf(content.effects, ref, "the clip");
}

std::expected<ParamSlot, Error> effectSlotOf(const EffectStack& stack, const ParamRef& ref, std::string_view owner) {
    const auto it =
        std::find_if(stack.begin(), stack.end(), [&](const EffectInstance& e) { return e.id == ref.effect; });
    if (it == stack.end()) {
        return std::unexpected(detail::notFound(std::string(owner) + " has no effect " + std::to_string(ref.effect.value)));
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(it->effect);
    if (!d || it->version > d->version) {
        return std::unexpected(detail::invalidArgument(
            "effect " + it->effect + " is unknown to this version of Wabcut; its parameters are kept as they are"));
    }
    const EffectParamInfo* info = d->param(ref.key);
    if (!info) {
        return std::unexpected(detail::notFound(it->effect + " has no parameter \"" + ref.key + "\""));
    }
    const ParamValue* v = it->find(ref.key);
    return ParamSlot{info->minValue, info->maxValue, info->animatable, v ? *v : ParamValue(info->defaultValue)};
}

void storeEffectParam(EffectStack& stack, const ParamRef& ref, ParamValue value) {
    for (EffectInstance& e : stack) {
        if (e.id == ref.effect) {
            e.set(ref.key, std::move(value));
            return;
        }
    }
}

void storeParam(ClipContent& content, const ParamRef& ref, ParamValue value) {
    if (ref.intrinsic) {
        content.properties[*ref.intrinsic] = std::move(value);
        return;
    }
    storeEffectParam(content.effects, ref, std::move(value));
}

std::string paramName(const ParamRef& ref) {
    return ref.intrinsic ? std::string(parameterInfo(*ref.intrinsic).key)
                         : "effect " + std::to_string(ref.effect.value) + " " + ref.key;
}

using ValueEdit = std::function<std::expected<ParamValue, Error>(const ParamValue& current)>;

/// Reads a parameter of one clip, applies `edit`, validates and stores the result.
EditOutcome editParam(const ProjectState& state, SequenceId sequenceId, ClipId clipId, const ParamRef& ref,
                      const ValueEdit& edit) {
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
    const auto slot = slotOf(clip.content(), track.kind, ref);
    if (!slot) {
        return std::unexpected(slot.error());
    }
    auto next = edit(slot->current);
    if (!next) {
        return std::unexpected(next.error());
    }
    if (!next->isValid(slot->minValue, slot->maxValue)) {
        return std::unexpected(detail::invalidArgument(paramName(ref) + " must stay within its range"));
    }
    if (next->isCurve() && !slot->animatable) {
        return std::unexpected(detail::invalidArgument(paramName(ref) + " cannot be animated"));
    }
    if (*next == slot->current) {
        return detail::unchanged(state);
    }
    ClipContent content = clip.content();
    storeParam(content, ref, std::move(*next));
    ed.mutableClip(site->track, site->index).setContent(std::move(content));
    return ed.finish();
}

std::expected<Curve, Error> curveOf(const ParamValue& v, const ParamRef& ref) {
    if (const Curve* c = v.curve()) {
        return *c;
    }
    return std::unexpected(detail::invalidArgument(paramName(ref) + " is not animated (it has no keyframes)"));
}

std::expected<void, Error> checkIndex(const Curve& c, std::size_t index) {
    if (index >= c.keys.size()) {
        return std::unexpected(detail::notFound("there is no keyframe " + std::to_string(index) + " (the curve has " +
                                                std::to_string(c.keys.size()) + ")"));
    }
    return {};
}

/// The clip's content after applying `edit` to its stack.
using StackEdit = std::function<std::expected<void, Error>(EffectStack& stack, TrackKind kind)>;

EditOutcome editStack(const ProjectState& state, SequenceId sequenceId, ClipId clipId, const StackEdit& edit) {
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
    EffectStack stack = clip.effects();
    if (auto ok = edit(stack, track.kind); !ok) {
        return std::unexpected(ok.error());
    }
    if (stack == clip.effects()) {
        return detail::unchanged(state);
    }
    ed.mutableClip(site->track, site->index).setEffects(std::move(stack));
    return ed.finish();
}

EffectStack::iterator findInstance(EffectStack& stack, EffectInstanceId id) {
    return std::find_if(stack.begin(), stack.end(), [&](const EffectInstance& e) { return e.id == id; });
}

Error noSuchEffect(EffectInstanceId id, std::string_view owner = "the clip") {
    return detail::notFound(std::string(owner) + " has no effect " + std::to_string(id.value));
}

/// Checks parameters for a known effect: known keys, in range, animatable when curves.
std::expected<void, Error> checkEffectParams(const EffectDescriptor& d, const std::vector<EffectParam>& params) {
    for (const EffectParam& p : params) {
        const EffectParamInfo* info = d.param(p.key);
        if (!info) {
            return std::unexpected(detail::notFound(std::string(d.id) + " has no parameter \"" + p.key + "\""));
        }
        if (!p.value.isValid(info->minValue, info->maxValue)) {
            return std::unexpected(detail::invalidArgument(std::string(d.id) + " " + p.key + " must stay within " +
                                                           std::to_string(info->minValue) + ".." +
                                                           std::to_string(info->maxValue)));
        }
        if (p.value.isCurve() && !info->animatable) {
            return std::unexpected(detail::invalidArgument(std::string(d.id) + " " + p.key + " cannot be animated"));
        }
    }
    return {};
}

/// The descriptor of an effect that may be added to a stack, with `params` sorted and checked.
std::expected<const EffectDescriptor*, Error> checkNewEffect(const std::string& effect, std::vector<EffectParam>& params) {
    const EffectDescriptor* d = EffectRegistry::builtin().find(effect);
    if (!d || d->kind == EffectKind::VideoTransition || d->kind == EffectKind::AudioTransition) {
        return std::unexpected(detail::notFound("unknown effect \"" + effect + "\""));
    }
    if (d->intrinsic) {
        return std::unexpected(detail::invalidArgument(
            effect + " is built into every clip; set its parameters instead of adding it"));
    }
    std::sort(params.begin(), params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    if (std::adjacent_find(params.begin(), params.end(),
                           [](const EffectParam& a, const EffectParam& b) { return a.key == b.key; }) != params.end()) {
        return std::unexpected(detail::invalidArgument("a parameter is given twice"));
    }
    if (auto ok = checkEffectParams(*d, params); !ok) {
        return std::unexpected(ok.error());
    }
    return d;
}

/// A track or master stack being edited: a copy of it, its kind and what it is called in messages.
struct StackSite {
    EffectStack stack;
    TrackKind kind = TrackKind::Video;
    std::string owner;
};

std::expected<StackSite, Error> stackSite(const Sequence& seq, const EffectTarget& target) {
    switch (target.kind) {
    case EffectTarget::Kind::Track: {
        const Track* t = seq.findTrack(target.track);
        if (!t) {
            return std::unexpected(detail::notFound("track " + std::to_string(target.track.value) + " does not exist"));
        }
        const std::string label = detail::trackLabel(seq, t->id);
        if (t->locked) {
            return std::unexpected(detail::invalidArgument("track " + label + " is locked"));
        }
        return StackSite{t->effects, t->kind, "track " + label};
    }
    case EffectTarget::Kind::MasterVideo:
        return StackSite{seq.effects, TrackKind::Video, "the video master"};
    case EffectTarget::Kind::MasterAudio:
        return StackSite{seq.audioEffects, TrackKind::Audio, "the audio master"};
    case EffectTarget::Kind::Clip:
        break;
    }
    return std::unexpected(detail::invalidArgument("not a track or master target"));
}

/// Edits a track or master stack: `edit` gets the site and the editor (for new instance ids).
using SiteEdit = std::function<std::expected<void, Error>(StackSite& site, SequenceEditor& ed)>;

EditOutcome editSite(const ProjectState& state, SequenceId sequenceId, const EffectTarget& target, const SiteEdit& edit) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    auto site = stackSite(**seq, target);
    if (!site) {
        return std::unexpected(site.error());
    }
    SequenceEditor ed(state, **seq);
    const EffectStack before = site->stack;
    if (auto ok = edit(*site, ed); !ok) {
        return std::unexpected(ok.error());
    }
    if (site->stack == before) {
        return detail::unchanged(state);
    }
    switch (target.kind) {
    case EffectTarget::Kind::Track:
        ed.mutableTrack(target.track).effects = std::move(site->stack);
        break;
    case EffectTarget::Kind::MasterVideo:
        ed.header().effects = std::move(site->stack);
        break;
    case EffectTarget::Kind::MasterAudio:
        ed.header().audioEffects = std::move(site->stack);
        break;
    case EffectTarget::Kind::Clip:
        break;
    }
    return ed.finish();
}

/// A.3: the fader of an audio track (volume, pan: Track::volume / Track::pan) or of the audio master (volume:
/// Sequence::masterVolume), edited like an intrinsic clip parameter. Keyframe times are timeline times.
EditOutcome editFaderParam(const ProjectState& state, SequenceId sequenceId, const EffectTarget& target, ParamId id,
                           const ValueEdit& edit) {
    const ParameterInfo& info = parameterInfo(id);
    if (id != ParamId::VolumeDb && id != ParamId::Pan) {
        return std::unexpected(detail::invalidArgument(std::string(info.key) +
                                                       " is a clip parameter; tracks have volume.level and "
                                                       "panner.balance, the audio master volume.level"));
    }
    if (target.kind == EffectTarget::Kind::MasterVideo) {
        return std::unexpected(detail::invalidArgument("the video master has no " + std::string(info.key)));
    }
    if (target.kind == EffectTarget::Kind::MasterAudio && id == ParamId::Pan) {
        return std::unexpected(detail::invalidArgument("the audio master has no panner"));
    }
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    ParamValue current = (*seq)->masterVolume;
    if (target.kind == EffectTarget::Kind::Track) {
        const Track* t = (*seq)->findTrack(target.track);
        if (!t) {
            return std::unexpected(detail::notFound("track " + std::to_string(target.track.value) + " does not exist"));
        }
        const std::string label = detail::trackLabel(**seq, t->id);
        if (t->kind != TrackKind::Audio) {
            return std::unexpected(detail::invalidArgument("track " + label + " is a video track: faders are on audio tracks"));
        }
        if (t->locked) {
            return std::unexpected(detail::invalidArgument("track " + label + " is locked"));
        }
        current = id == ParamId::VolumeDb ? t->volume : t->pan;
    }
    auto next = edit(current);
    if (!next) {
        return std::unexpected(next.error());
    }
    if (!next->isValid(info.minValue, info.maxValue)) {
        return std::unexpected(detail::invalidArgument(std::string(info.key) + " must stay within " +
                                                       std::to_string(info.minValue) + ".." + std::to_string(info.maxValue)));
    }
    if (*next == current) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    if (target.kind == EffectTarget::Kind::Track) {
        Track& t = ed.mutableTrack(target.track);
        (id == ParamId::VolumeDb ? t.volume : t.pan) = std::move(*next);
    } else {
        ed.header().masterVolume = std::move(*next);
    }
    return ed.finish();
}

/// A parameter edit on a track or master effect (or, A.3, fader).
EditOutcome editSiteParam(const ProjectState& state, SequenceId sequenceId, const EffectTarget& target,
                          const ParamRef& ref, const ValueEdit& edit) {
    if (ref.intrinsic) {
        return editFaderParam(state, sequenceId, target, *ref.intrinsic, edit);
    }
    return editSite(state, sequenceId, target, [&](StackSite& site, SequenceEditor&) -> std::expected<void, Error> {
        const auto slot = effectSlotOf(site.stack, ref, site.owner);
        if (!slot) {
            return std::unexpected(slot.error());
        }
        auto next = edit(slot->current);
        if (!next) {
            return std::unexpected(next.error());
        }
        if (!next->isValid(slot->minValue, slot->maxValue)) {
            return std::unexpected(detail::invalidArgument(paramName(ref) + " must stay within its range"));
        }
        if (next->isCurve() && !slot->animatable) {
            return std::unexpected(detail::invalidArgument(paramName(ref) + " cannot be animated"));
        }
        if (*next != slot->current) {
            storeEffectParam(site.stack, ref, std::move(*next));
        }
        return {};
    });
}

/// Runs a value edit on any target.
EditOutcome editTargetParam(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                            const ParamRef& ref, const ValueEdit& edit) {
    if (target.kind == EffectTarget::Kind::Clip) {
        return editParam(state, sequence, target.clip, ref, edit);
    }
    return editSiteParam(state, sequence, target, ref, edit);
}

} // namespace

// --- The edits themselves, shared by the clip and the target overloads ------------------------------------

namespace {

ValueEdit replaceWith(ParamValue value) {
    return [value = std::move(value)](const ParamValue&) -> std::expected<ParamValue, Error> { return value; };
}

ValueEdit addKey(Keyframe key) {
    return [key](const ParamValue& current) -> std::expected<ParamValue, Error> {
        Curve curve = current.isCurve() ? *current.curve() : Curve{};
        if (const auto at = curve.keyAt(key.time)) {
            curve.keys[*at] = key;
        } else {
            curve.keys.insert(curve.keys.begin() + static_cast<std::ptrdiff_t>(curve.insertionIndex(key.time)), key);
        }
        return ParamValue(std::move(curve));
    };
}

ValueEdit moveKey(ParamRef param, std::size_t index, Ticks time) {
    return [param = std::move(param), index, time](const ParamValue& current) -> std::expected<ParamValue, Error> {
        auto curve = curveOf(current, param);
        if (!curve) {
            return std::unexpected(curve.error());
        }
        if (auto ok = checkIndex(*curve, index); !ok) {
            return std::unexpected(ok.error());
        }
        if (const auto other = curve->keyAt(time); other && *other != index) {
            return std::unexpected(detail::invalidArgument("there is already a keyframe at that time"));
        }
        Keyframe key = curve->keys[index];
        curve->keys.erase(curve->keys.begin() + static_cast<std::ptrdiff_t>(index));
        key.time = time;
        curve->keys.insert(curve->keys.begin() + static_cast<std::ptrdiff_t>(curve->insertionIndex(time)), key);
        return ParamValue(std::move(*curve));
    };
}

ValueEdit removeKey(ParamRef param, std::size_t index) {
    return [param = std::move(param), index](const ParamValue& current) -> std::expected<ParamValue, Error> {
        auto curve = curveOf(current, param);
        if (!curve) {
            return std::unexpected(curve.error());
        }
        if (auto ok = checkIndex(*curve, index); !ok) {
            return std::unexpected(ok.error());
        }
        if (curve->keys.size() == 1) {
            return ParamValue(curve->keys.front().value);
        }
        curve->keys.erase(curve->keys.begin() + static_cast<std::ptrdiff_t>(index));
        return ParamValue(std::move(*curve));
    };
}

ValueEdit changeKey(ParamRef param, std::size_t index, KeyframeChange change) {
    return [param = std::move(param), index, change](const ParamValue& current) -> std::expected<ParamValue, Error> {
        auto curve = curveOf(current, param);
        if (!curve) {
            return std::unexpected(curve.error());
        }
        if (auto ok = checkIndex(*curve, index); !ok) {
            return std::unexpected(ok.error());
        }
        Keyframe& k = curve->keys[index];
        if (change.value) {
            k.value = *change.value;
        }
        if (change.interpolation) {
            k.interpolation = *change.interpolation;
        }
        if (change.in) {
            k.in = *change.in;
        }
        if (change.out) {
            k.out = *change.out;
        }
        if (!curve->isWellFormed()) {
            return std::unexpected(detail::invalidArgument("Bezier handles need a finite slope and an influence in 0..1"));
        }
        return ParamValue(std::move(*curve));
    };
}

ValueEdit animate(bool animated, Ticks curveTime) {
    return [animated, curveTime](const ParamValue& current) -> std::expected<ParamValue, Error> {
        if (animated == current.isCurve()) {
            return current;
        }
        if (animated) {
            return ParamValue(Curve{{Keyframe{curveTime, current.constant()}}});
        }
        return ParamValue(current.valueAt(curveTime));
    };
}

/// A change to one stack, given what the stack is called in messages.
using InstanceEdit = std::function<std::expected<void, Error>(EffectStack& stack, std::string_view owner)>;

InstanceEdit removeInstance(EffectInstanceId effect) {
    return [effect](EffectStack& stack, std::string_view owner) -> std::expected<void, Error> {
        const auto it = findInstance(stack, effect);
        if (it == stack.end()) {
            return std::unexpected(noSuchEffect(effect, owner));
        }
        stack.erase(it);
        return {};
    };
}

InstanceEdit moveInstance(EffectInstanceId effect, std::size_t index) {
    return [effect, index](EffectStack& stack, std::string_view owner) -> std::expected<void, Error> {
        const auto it = findInstance(stack, effect);
        if (it == stack.end()) {
            return std::unexpected(noSuchEffect(effect, owner));
        }
        EffectInstance moved = std::move(*it);
        stack.erase(it);
        const std::size_t at = std::min(index, stack.size());
        stack.insert(stack.begin() + static_cast<std::ptrdiff_t>(at), std::move(moved));
        return {};
    };
}

InstanceEdit changeInstance(EffectInstanceId effect, EffectChange change) {
    return [effect, change](EffectStack& stack, std::string_view owner) -> std::expected<void, Error> {
        const auto it = findInstance(stack, effect);
        if (it == stack.end()) {
            return std::unexpected(noSuchEffect(effect, owner));
        }
        if (change.enabled) {
            it->enabled = *change.enabled;
        }
        if (!change.data.empty()) {
            const EffectDescriptor* known = EffectRegistry::builtin().find(it->effect);
            if (!known || it->version > known->version) {
                return std::unexpected(detail::invalidArgument(
                    "effect " + it->effect + " is unknown to this version of Wabcut; its data is kept as it is"));
            }
            for (const EffectData& entry : change.data) {
                if (const auto problem = effectDataProblem(entry)) {
                    return std::unexpected(detail::invalidArgument(*problem));
                }
                it->setData(entry);
            }
            if (it->data.size() > kMaxEffectDataEntries) {
                return std::unexpected(detail::invalidArgument("an effect may hold at most " +
                                                               std::to_string(kMaxEffectDataEntries) + " data entries"));
            }
        }
        if (change.params.empty()) {
            return {};
        }
        const EffectDescriptor* d = EffectRegistry::builtin().find(it->effect);
        if (!d || it->version > d->version) {
            return std::unexpected(detail::invalidArgument(
                "effect " + it->effect + " is unknown to this version of Wabcut; its parameters are kept as they are"));
        }
        if (auto ok = checkEffectParams(*d, change.params); !ok) {
            return std::unexpected(ok.error());
        }
        for (const EffectParam& p : change.params) {
            it->set(p.key, p.value);
        }
        return {};
    };
}

EditOutcome editClipStack(const ProjectState& state, SequenceId sequence, ClipId clip, const InstanceEdit& edit) {
    return editStack(state, sequence, clip,
                     [&](EffectStack& stack, TrackKind) -> std::expected<void, Error> { return edit(stack, "the clip"); });
}

EditOutcome editTargetStack(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                            const InstanceEdit& edit) {
    if (target.kind == EffectTarget::Kind::Clip) {
        return editClipStack(state, sequence, target.clip, edit);
    }
    return editSite(state, sequence, target,
                    [&](StackSite& site, SequenceEditor&) -> std::expected<void, Error> { return edit(site.stack, site.owner); });
}

} // namespace

// --- Parameters and keyframes ----------------------------------------------------------------------------

EditOutcome setParameter(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                         ParamValue value) {
    return editParam(state, sequence, clip, param, replaceWith(std::move(value)));
}

EditOutcome addKeyframe(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                        Keyframe key) {
    return editParam(state, sequence, clip, param, addKey(key));
}

EditOutcome moveKeyframe(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                         std::size_t index, Ticks time) {
    return editParam(state, sequence, clip, param, moveKey(param, index, time));
}

EditOutcome moveKeyframes(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                          std::span<const std::size_t> indices, Ticks delta) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto loc = (*seq)->findClip(clip);
    if (!loc) {
        return std::unexpected(detail::notFound("clip " + std::to_string(clip.value) + " is not in this sequence"));
    }
    // The clip's media range in curve time (reversed clips run it backwards).
    const Clip& c = loc->clip();
    const Ticks a = c.curveTimeAt(c.start());
    const Ticks b = c.curveTimeAt(c.end());
    const Ticks lo = std::min(a, b);
    const Ticks hi = std::max(a, b);
    Ticks applied;
    auto out = editParam(state, sequence, clip, param, [&](const ParamValue& current) -> std::expected<ParamValue, Error> {
        auto curve = curveOf(current, param);
        if (!curve) {
            return std::unexpected(curve.error());
        }
        if (indices.empty()) {
            return std::unexpected(detail::invalidArgument("no keyframes to move"));
        }
        std::vector<bool> moving(curve->keys.size(), false);
        Ticks first{std::numeric_limits<std::int64_t>::max()};
        Ticks last{std::numeric_limits<std::int64_t>::min()};
        for (const std::size_t i : indices) {
            if (auto ok = checkIndex(*curve, i); !ok) {
                return std::unexpected(ok.error());
            }
            if (moving[i]) {
                return std::unexpected(detail::invalidArgument("keyframe " + std::to_string(i) + " is listed twice"));
            }
            moving[i] = true;
            first = std::min(first, curve->keys[i].time);
            last = std::max(last, curve->keys[i].time);
        }
        applied = std::clamp(delta, std::min(Ticks{}, lo - first), std::max(Ticks{}, hi - last));
        if (applied.value == 0) {
            return current;
        }
        std::vector<Keyframe> moved;
        std::vector<Keyframe> kept;
        for (std::size_t i = 0; i < curve->keys.size(); ++i) {
            Keyframe k = curve->keys[i];
            if (moving[i]) {
                k.time += applied;
                moved.push_back(k);
            } else {
                kept.push_back(k);
            }
        }
        // Moved keys keep their spacing, so they never meet each other; a kept key where one lands goes.
        std::erase_if(kept, [&](const Keyframe& k) {
            return std::any_of(moved.begin(), moved.end(), [&](const Keyframe& m) { return m.time == k.time; });
        });
        Curve next;
        next.keys.reserve(moved.size() + kept.size());
        std::merge(moved.begin(), moved.end(), kept.begin(), kept.end(), std::back_inserter(next.keys),
                   [](const Keyframe& x, const Keyframe& y) { return x.time < y.time; });
        return ParamValue(std::move(next));
    });
    if (out) {
        out->appliedDelta = applied;
    }
    return out;
}

EditOutcome removeKeyframe(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                           std::size_t index) {
    return editParam(state, sequence, clip, param, removeKey(param, index));
}

EditOutcome setKeyframe(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                        std::size_t index, const KeyframeChange& change) {
    return editParam(state, sequence, clip, param, changeKey(param, index, change));
}

EditOutcome setAnimated(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                        bool animated, Ticks curveTime) {
    return editParam(state, sequence, clip, param, animate(animated, curveTime));
}

// --- Effect stacks --------------------------------------------------------------------------------------

EditOutcome addEffect(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips,
                      const std::string& effect, std::vector<EffectParam> params, std::optional<std::size_t> index) {
    const auto descriptor = checkNewEffect(effect, params);
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    const EffectDescriptor* d = *descriptor;
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto refs = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    SequenceEditor ed(state, **seq);
    const TrackKind wanted = d->kind == EffectKind::Video ? TrackKind::Video : TrackKind::Audio;
    for (const ClipRef& r : *refs) {
        if (ed.track(r.track).kind != wanted) {
            return std::unexpected(detail::invalidArgument(effect + " is a " + std::string(toString(d->kind)) +
                                                           " effect; clip " + std::to_string(r.clip.value) +
                                                           " is on " + detail::trackLabel(**seq, r.track)));
        }
    }
    // Instance ids follow the order the clips were named in.
    for (const ClipId id : clips) {
        const auto it = std::find_if(refs->begin(), refs->end(), [&](const ClipRef& r) { return r.clip == id; });
        if (it == refs->end()) {
            continue;
        }
        Clip& clip = ed.mutableClip(it->track, it->index);
        EffectStack stack = clip.effects();
        EffectInstance instance;
        instance.id = ed.newEffectId();
        instance.effect = effect;
        instance.version = d->version;
        instance.params = params;
        const std::size_t at = std::min(index.value_or(stack.size()), stack.size());
        stack.insert(stack.begin() + static_cast<std::ptrdiff_t>(at), std::move(instance));
        clip.setEffects(std::move(stack));
    }
    return ed.finish();
}

EditOutcome removeEffect(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect) {
    return editClipStack(state, sequence, clip, removeInstance(effect));
}

EditOutcome moveEffect(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect,
                       std::size_t index) {
    return editClipStack(state, sequence, clip, moveInstance(effect, index));
}

EditOutcome setEffect(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect,
                      const EffectChange& change) {
    return editClipStack(state, sequence, clip, changeInstance(effect, change));
}

// --- Any target ---------------------------------------------------------------------------------------------

EditOutcome addEffect(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                      const std::string& effect, std::vector<EffectParam> params, std::optional<std::size_t> index) {
    if (target.kind == EffectTarget::Kind::Clip) {
        return addEffect(state, sequence, std::span<const ClipId>(&target.clip, 1), effect, std::move(params), index);
    }
    const auto descriptor = checkNewEffect(effect, params);
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    const EffectDescriptor* d = *descriptor;
    return editSite(state, sequence, target, [&](StackSite& site, SequenceEditor& ed) -> std::expected<void, Error> {
        const TrackKind wanted = d->kind == EffectKind::Video ? TrackKind::Video : TrackKind::Audio;
        if (site.kind != wanted) {
            return std::unexpected(detail::invalidArgument(effect + " is a " + std::string(toString(d->kind)) +
                                                           " effect; " + site.owner + " takes " +
                                                           (site.kind == TrackKind::Video ? "video" : "audio") +
                                                           " effects"));
        }
        EffectInstance instance;
        instance.id = ed.newEffectId();
        instance.effect = effect;
        instance.version = d->version;
        instance.params = params;
        const std::size_t at = std::min(index.value_or(site.stack.size()), site.stack.size());
        site.stack.insert(site.stack.begin() + static_cast<std::ptrdiff_t>(at), std::move(instance));
        return {};
    });
}

EditOutcome removeEffect(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                         EffectInstanceId effect) {
    return editTargetStack(state, sequence, target, removeInstance(effect));
}

EditOutcome moveEffect(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                       EffectInstanceId effect, std::size_t index) {
    return editTargetStack(state, sequence, target, moveInstance(effect, index));
}

EditOutcome setEffect(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                      EffectInstanceId effect, const EffectChange& change) {
    return editTargetStack(state, sequence, target, changeInstance(effect, change));
}

EditOutcome setParameter(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                         const ParamRef& param, ParamValue value) {
    return editTargetParam(state, sequence, target, param, replaceWith(std::move(value)));
}

EditOutcome addKeyframe(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                        const ParamRef& param, Keyframe key) {
    return editTargetParam(state, sequence, target, param, addKey(key));
}

EditOutcome moveKeyframe(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                         const ParamRef& param, std::size_t index, Ticks time) {
    return editTargetParam(state, sequence, target, param, moveKey(param, index, time));
}

EditOutcome removeKeyframe(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                           const ParamRef& param, std::size_t index) {
    return editTargetParam(state, sequence, target, param, removeKey(param, index));
}

EditOutcome setKeyframe(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                        const ParamRef& param, std::size_t index, const KeyframeChange& change) {
    return editTargetParam(state, sequence, target, param, changeKey(param, index, change));
}

EditOutcome setAnimated(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                        const ParamRef& param, bool animated, Ticks curveTime) {
    return editTargetParam(state, sequence, target, param, animate(animated, curveTime));
}

const EffectStack* effectStackOf(const Sequence& sequence, const EffectTarget& target) {
    switch (target.kind) {
    case EffectTarget::Kind::Clip: {
        const auto location = sequence.findClip(target.clip);
        return location ? &location->clip().effects() : nullptr;
    }
    case EffectTarget::Kind::Track: {
        const Track* track = sequence.findTrack(target.track);
        return track ? &track->effects : nullptr;
    }
    case EffectTarget::Kind::MasterVideo:
        return &sequence.effects;
    case EffectTarget::Kind::MasterAudio:
        return &sequence.audioEffects;
    }
    return nullptr;
}

} // namespace wabcut::edit
