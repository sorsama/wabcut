// Parameter, keyframe and effect-stack operations. Each touches one clip's content block (copy-on-write), one
// track's stack or one of the sequence's master stacks.

#include <wabcut/model/FinishingOps.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/KeyPins.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/TitleOps.h>

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
    if (ref.layer != 0) {
        // 0.6: a graphic layer's number (GraphicOps.h layerParam).
        const GraphicLayer* layer = content.graphic ? content.graphic->findLayer(ref.layer) : nullptr;
        if (!layer) {
            return std::unexpected(detail::notFound(content.graphic ? "the graphic has no layer " + std::to_string(ref.layer)
                                                                    : std::string("the clip is not a graphic")));
        }
        const auto info = layerParamInfo(*layer, ref.key);
        const auto value = layerParam(*layer, ref.key);
        if (!info || !value) {
            return std::unexpected(detail::notFound("layer " + std::to_string(ref.layer) + " has no parameter \"" + ref.key + "\""));
        }
        return ParamSlot{info->minValue, info->maxValue, true, *value};
    }
    if (ref.intrinsic) {
        const ParameterInfo& info = parameterInfo(*ref.intrinsic);
        if (isVideoParam(*ref.intrinsic) != (kind == TrackKind::Video)) {
            return std::unexpected(detail::invalidArgument(
                std::string(info.key) + " is a " + (isVideoParam(*ref.intrinsic) ? "video" : "audio") +
                " parameter; the clip is a " + (kind == TrackKind::Video ? "video" : "audio") + " clip"));
        }
        return ParamSlot{info.minValue, info.maxValue, info.animatable, content.properties[*ref.intrinsic]};
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
    if (ref.layer != 0) {
        if (!content.graphic) {
            return;
        }
        Graphic graphic = *content.graphic;
        if (GraphicLayer* layer = graphic.findLayer(ref.layer)) {
            setLayerParam(*layer, ref.key, std::move(value));
            content.graphic = std::make_shared<const Graphic>(std::move(graphic));
        }
        return;
    }
    if (ref.intrinsic) {
        content.properties[*ref.intrinsic] = std::move(value);
        return;
    }
    storeEffectParam(content.effects, ref, std::move(value));
}

std::string paramName(const ParamRef& ref) {
    if (ref.layer != 0) {
        return "layer " + std::to_string(ref.layer) + " " + ref.key;
    }
    return ref.intrinsic ? std::string(parameterInfo(*ref.intrinsic).key)
                         : "effect " + std::to_string(ref.effect.value) + " " + ref.key;
}

using ValueEdit = std::function<std::expected<ParamValue, Error>(const ParamValue& current)>;

EditOutcome editTargetParam(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                            const ParamRef& ref, const ValueEdit& edit);

/// 0.6 (card #71): the media item whose Master Clip Effects hold `effect` when the clip's own stack does not.
const MediaItem* masterHolderOf(const ProjectState& state, const Clip& clip, EffectInstanceId effect) {
    if (std::any_of(clip.effects().begin(), clip.effects().end(), [&](const EffectInstance& e) { return e.id == effect; })) {
        return nullptr;
    }
    return masterEffectHolder(state, clip.media, effect);
}

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
    if (!ref.intrinsic && ref.layer == 0) {
        // 0.6: a master clip effect of the clip's media is edited on the media item (every instance has it).
        if (const MediaItem* holder = masterHolderOf(state, clip, ref.effect)) {
            return editTargetParam(state, sequenceId, EffectTarget::ofMedia(holder->id), ref, edit);
        }
    }
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
    if (ref.layer != 0 && content.graphic) {
        if (const auto problem = graphicProblem(*content.graphic)) {
            return std::unexpected(detail::invalidArgument("the graphic " + *problem));
        }
        if (*content.graphic == *clip.content().graphic) {
            return detail::unchanged(state);
        }
    }
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
    /// 0.6: a media item's master stack takes the kinds of its streams: bit 0 video, bit 1 audio (0: `kind` only).
    std::uint8_t kinds = 0;
};

std::expected<StackSite, Error> stackSite(const Sequence& seq, const EffectTarget& target) {
    switch (target.kind) {
    case EffectTarget::Kind::Track: {
        const Track* t = seq.findTrack(target.track);
        if (!t) {
            t = seq.findSubmix(target.track); // p02 audio: a submix track's effects are a track stack too
        }
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
    case EffectTarget::Kind::Media:
        break;
    }
    return std::unexpected(detail::invalidArgument("not a track or master target"));
}

/// Edits a track, master or (0.6) master clip stack: `edit` gets the site and where new instance ids come from.
using NewEffectId = std::function<EffectInstanceId()>;
using SiteEdit = std::function<std::expected<void, Error>(StackSite& site, const NewEffectId& newId)>;

/// 0.6 (card #71): a media item's Master Clip Effects, edited as one step on the item (the sequences stay as they are).
/// Bounded like a preset's effects (project files are untrusted input).
constexpr std::size_t kMaxMasterEffects = 64;

EditOutcome editMediaSite(const ProjectState& state, MediaId media, const SiteEdit& edit) {
    const auto it = std::lower_bound(state.media.begin(), state.media.end(), media,
                                     [](const MediaPtr& m, MediaId value) { return m->id < value; });
    if (it == state.media.end() || (*it)->id != media) {
        return std::unexpected(detail::notFound("media " + std::to_string(media.value) + " does not exist"));
    }
    const MediaItem& item = **it;
    StackSite site{item.masterEffects, TrackKind::Video, "the master clip of " + item.name, 0};
    site.kinds = static_cast<std::uint8_t>((item.info.video.empty() ? 0 : 1) | (item.info.audio.empty() ? 0 : 2));
    if (site.kinds == 0) {
        site.kinds = 3; // nothing probed yet (an offline item): either kind
    }
    EditResult result;
    result.state = state;
    const NewEffectId newId = [&] {
        const auto id = result.state.ids.allocate<EffectInstanceId>();
        result.createdEffects.push_back(id);
        return id;
    };
    if (auto ok = edit(site, newId); !ok) {
        return std::unexpected(ok.error());
    }
    if (site.stack == item.masterEffects) {
        return detail::unchanged(state);
    }
    if (site.stack.size() > kMaxMasterEffects) {
        return std::unexpected(detail::invalidArgument("a master clip may hold at most " + std::to_string(kMaxMasterEffects) +
                                                       " effects"));
    }
    MediaItem changed = item;
    changed.masterEffects = std::move(site.stack);
    result.state.media[static_cast<std::size_t>(it - state.media.begin())] = std::make_shared<const MediaItem>(std::move(changed));
    result.changes = diff(state, result.state);
    return result;
}

EditOutcome editSite(const ProjectState& state, SequenceId sequenceId, const EffectTarget& target, const SiteEdit& edit) {
    if (target.kind == EffectTarget::Kind::Media) {
        return editMediaSite(state, target.media, edit);
    }
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
    if (auto ok = edit(*site, [&] { return ed.newEffectId(); }); !ok) {
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
    case EffectTarget::Kind::Media:
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
            t = (*seq)->findSubmix(target.track); // p02 audio: a submix track's fader and panner
        }
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
    if (ref.intrinsic && target.kind == EffectTarget::Kind::Media) {
        return std::unexpected(detail::invalidArgument("a master clip has effects only; Motion, Opacity and Volume are each clip's"));
    }
    if (ref.intrinsic) {
        return editFaderParam(state, sequenceId, target, *ref.intrinsic, edit);
    }
    return editSite(state, sequenceId, target, [&](StackSite& site, const NewEffectId&) -> std::expected<void, Error> {
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

/// 0.7 (card 120): pins key `index` to a clip end whose curve time is `boundary` (the key moves there, replacing a key
/// already on it), or unpins it where it is.
ValueEdit pinKey(ParamRef param, std::size_t index, KeyPin pin, Ticks boundary) {
    return [param = std::move(param), index, pin, boundary](const ParamValue& current) -> std::expected<ParamValue, Error> {
        auto curve = curveOf(current, param);
        if (!curve) {
            return std::unexpected(curve.error());
        }
        if (auto ok = checkIndex(*curve, index); !ok) {
            return std::unexpected(ok.error());
        }
        Keyframe key = curve->keys[index];
        key.pin = pin;
        if (pin == KeyPin::None || key.time == boundary) {
            curve->keys[index] = key;
            return ParamValue(std::move(*curve));
        }
        curve->keys.erase(curve->keys.begin() + static_cast<std::ptrdiff_t>(index));
        if (const auto there = curve->keyAt(boundary)) {
            curve->keys.erase(curve->keys.begin() + static_cast<std::ptrdiff_t>(*there)); // the pinned key replaces it
        }
        key.time = boundary;
        curve->keys.insert(curve->keys.begin() + static_cast<std::ptrdiff_t>(curve->insertionIndex(boundary)), key);
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

/// 0.6: the master stack that holds `effect` for a clip whose own stack does not (the clip operations act there).
std::optional<MediaId> masterTargetOf(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect) {
    const auto seq = detail::findSequence(state, sequence);
    const auto loc = seq ? (*seq)->findClip(clip) : std::nullopt;
    if (!loc) {
        return std::nullopt;
    }
    const MediaItem* holder = masterHolderOf(state, loc->clip(), effect);
    return holder ? std::optional<MediaId>(holder->id) : std::nullopt;
}

EditOutcome editTargetStack(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                            const InstanceEdit& edit) {
    if (target.kind == EffectTarget::Kind::Clip) {
        return editClipStack(state, sequence, target.clip, edit);
    }
    return editSite(state, sequence, target,
                    [&](StackSite& site, const NewEffectId&) -> std::expected<void, Error> { return edit(site.stack, site.owner); });
}

} // namespace

// --- Parameters and keyframes ----------------------------------------------------------------------------

namespace {

/// 0.7 (card #12): switching Uniform Scale carries Scale Width along in the same step, so the picture does not jump: off
/// copies the Scale (keys too) to Scale Width; on puts Scale Width back to its default (the width follows the Scale again).
EditOutcome setUniformScaleParam(const ProjectState& state, SequenceId sequence, ClipId clip, ParamValue value) {
    const ParamRef uniform = ParamRef::of(ParamId::UniformScale);
    auto plain = editParam(state, sequence, clip, uniform, replaceWith(value)); // checks the clip, its track and the range
    if (!plain) {
        return plain;
    }
    const auto seq = detail::findSequence(state, sequence);
    const auto located = seq ? (*seq)->findClip(clip) : std::nullopt;
    if (!located) {
        return plain;
    }
    const ClipProperties& before = located->clip().properties();
    if (before[ParamId::UniformScale] == value) {
        return plain; // unchanged
    }
    const bool turningOff = value.constant() < 0.5;
    const ParamValue width = turningOff ? before[ParamId::Scale] : ParamValue(parameterInfo(ParamId::ScaleWidth).defaultValue);
    if (before[ParamId::ScaleWidth] == width) {
        return plain;
    }
    auto both = editParam(plain->state, sequence, clip, ParamRef::of(ParamId::ScaleWidth), replaceWith(width));
    if (!both) {
        return both;
    }
    both->changes = diff(state, both->state);
    return both;
}

} // namespace

EditOutcome setParameter(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                         ParamValue value) {
    if (param.intrinsic == ParamId::UniformScale && param.layer == 0 && value.isConstant()) {
        return setUniformScaleParam(state, sequence, clip, std::move(value));
    }
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

EditOutcome setKeyframePin(const ProjectState& state, SequenceId sequenceId, ClipId clipId, const ParamRef& param,
                           std::size_t index, KeyPin pin) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto site = editableClip(**seq, clipId);
    if (!site) {
        return std::unexpected(site.error());
    }
    const Clip& clip = (*seq)->findTrack(site->track)->clips[site->index];
    const Ticks boundary = pin == KeyPin::ClipEnd ? clip.endCurveTime() : clip.startCurveTime();
    return editParam(state, sequenceId, clipId, param, pinKey(param, index, pin, boundary));
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
    if (const auto media = masterTargetOf(state, sequence, clip, effect)) {
        return editTargetStack(state, sequence, EffectTarget::ofMedia(*media), removeInstance(effect));
    }
    return editClipStack(state, sequence, clip, removeInstance(effect));
}

EditOutcome moveEffect(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect,
                       std::size_t index) {
    if (const auto media = masterTargetOf(state, sequence, clip, effect)) {
        return editTargetStack(state, sequence, EffectTarget::ofMedia(*media), moveInstance(effect, index));
    }
    return editClipStack(state, sequence, clip, moveInstance(effect, index));
}

EditOutcome setEffect(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect,
                      const EffectChange& change) {
    if (const auto media = masterTargetOf(state, sequence, clip, effect)) {
        return editTargetStack(state, sequence, EffectTarget::ofMedia(*media), changeInstance(effect, change));
    }
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
    return editSite(state, sequence, target, [&](StackSite& site, const NewEffectId& newId) -> std::expected<void, Error> {
        const TrackKind wanted = d->kind == EffectKind::Video ? TrackKind::Video : TrackKind::Audio;
        if (site.kinds != 0 && (site.kinds & (wanted == TrackKind::Video ? 1 : 2)) == 0) {
            return std::unexpected(detail::invalidArgument(effect + " is a " + std::string(toString(d->kind)) + " effect; " +
                                                           site.owner + " has no " +
                                                           (wanted == TrackKind::Video ? "picture" : "sound")));
        }
        if (site.kinds == 0 && site.kind != wanted) {
            return std::unexpected(detail::invalidArgument(effect + " is a " + std::string(toString(d->kind)) +
                                                           " effect; " + site.owner + " takes " +
                                                           (site.kind == TrackKind::Video ? "video" : "audio") +
                                                           " effects"));
        }
        EffectInstance instance;
        instance.id = newId();
        instance.effect = effect;
        instance.version = d->version;
        instance.params = params;
        const std::size_t at = std::min(index.value_or(site.stack.size()), site.stack.size());
        site.stack.insert(site.stack.begin() + static_cast<std::ptrdiff_t>(at), std::move(instance));
        return {};
    });
}

EditOutcome appendEffectInstances(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                                  std::vector<EffectInstance> instances) {
    if (target.kind == EffectTarget::Kind::Clip || target.kind == EffectTarget::Kind::Media) {
        return std::unexpected(detail::invalidArgument("not a track or master target"));
    }
    return editSite(state, sequence, target, [&](StackSite& site, const NewEffectId& newId) -> std::expected<void, Error> {
        for (EffectInstance& e : instances) {
            const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
            const TrackKind wanted = d != nullptr && d->kind == EffectKind::Video ? TrackKind::Video : TrackKind::Audio;
            if (site.kind != wanted) {
                return std::unexpected(detail::invalidArgument(e.effect + " is a " + (wanted == TrackKind::Video ? "video" : "audio") +
                                                               " effect; " + site.owner + " takes " +
                                                               (site.kind == TrackKind::Video ? "video" : "audio") + " effects"));
            }
            e.id = newId();
            site.stack.push_back(std::move(e));
        }
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
        if (track == nullptr) {
            track = sequence.findSubmix(target.track);
        }
        return track ? &track->effects : nullptr;
    }
    case EffectTarget::Kind::MasterVideo:
        return &sequence.effects;
    case EffectTarget::Kind::MasterAudio:
        return &sequence.audioEffects;
    case EffectTarget::Kind::Media:
        break; // a media item's stack: the ProjectState overload
    }
    return nullptr;
}

// --- Master Clip Effects (0.6, card #71) ---------------------------------------------------------------------

const EffectStack* effectStackOf(const ProjectState& state, SequenceId sequence, const EffectTarget& target) {
    if (target.kind == EffectTarget::Kind::Media) {
        const MediaItem* item = state.findMedia(target.media);
        return item ? &item->masterEffects : nullptr;
    }
    const auto seq = detail::findSequence(state, sequence);
    return seq ? effectStackOf(**seq, target) : nullptr;
}

namespace {

/// The clip (on an unlocked track) and its media item for a master clip edit.
struct MasterClipSite {
    ClipRef ref;
    const Clip* clip = nullptr;
    const MediaItem* item = nullptr;
};

std::expected<MasterClipSite, Error> masterClipSite(const ProjectState& state, const Sequence& seq, ClipId clipId) {
    const auto site = editableClip(seq, clipId);
    if (!site) {
        return std::unexpected(site.error());
    }
    const Clip& clip = seq.findTrack(site->track)->clips[site->index];
    if (clip.isNested() || !clip.media.isValid()) {
        return std::unexpected(detail::invalidArgument(
            "nested sequences and multicam clips have no master clip effects yet: add the effect to the clip"));
    }
    const MediaItem* item = state.findMedia(clip.media);
    if (!item) {
        return std::unexpected(detail::notFound("the clip's media is not in the project"));
    }
    return MasterClipSite{*site, &clip, item};
}

} // namespace

EditOutcome moveEffectToMaster(const ProjectState& state, SequenceId sequence, ClipId clipId, EffectInstanceId effect) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto site = masterClipSite(state, **seq, clipId);
    if (!site) {
        return std::unexpected(site.error());
    }
    const EffectStack& own = site->clip->effects();
    const auto it = std::find_if(own.begin(), own.end(), [&](const EffectInstance& e) { return e.id == effect; });
    if (it == own.end()) {
        return std::unexpected(noSuchEffect(effect));
    }
    if (it->effect == kTitleStyleEffect) {
        return std::unexpected(detail::invalidArgument("a title's text style belongs to the clip"));
    }
    // The clip without it, then the master stack with it at the end (the same instance: its keys and masks are in
    // media time, which every instance shares).
    EffectInstance moved = *it;
    auto removed = editClipStack(state, sequence, clipId, removeInstance(effect));
    if (!removed) {
        return std::unexpected(removed.error());
    }
    auto added = editMediaSite(removed->state, site->item->id, [&](StackSite& media, const NewEffectId&) -> std::expected<void, Error> {
        const EffectDescriptor* d = EffectRegistry::builtin().find(moved.effect);
        const int bit = d && d->kind == EffectKind::Audio ? 2 : 1;
        if ((media.kinds & bit) == 0) {
            return std::unexpected(detail::invalidArgument(media.owner + " has no " + (bit == 1 ? "picture" : "sound")));
        }
        media.stack.push_back(moved);
        return {};
    });
    if (!added) {
        return std::unexpected(added.error());
    }
    added->changes = diff(state, added->state);
    return added;
}

EditOutcome moveEffectToClip(const ProjectState& state, SequenceId sequence, ClipId clipId, EffectInstanceId effect) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto site = masterClipSite(state, **seq, clipId);
    if (!site) {
        return std::unexpected(site.error());
    }
    const MediaItem* holder = masterEffectHolder(state, site->clip->media, effect);
    if (!holder) {
        return std::unexpected(detail::notFound("the master clip has no effect " + std::to_string(effect.value)));
    }
    const auto it = std::find_if(holder->masterEffects.begin(), holder->masterEffects.end(),
                                 [&](const EffectInstance& e) { return e.id == effect; });
    EffectInstance moved = *it;
    const EffectDescriptor* d = EffectRegistry::builtin().find(moved.effect);
    const TrackKind wanted = d && d->kind == EffectKind::Audio ? TrackKind::Audio : TrackKind::Video;
    if ((*seq)->findTrack(site->ref.track)->kind != wanted) {
        return std::unexpected(detail::invalidArgument(moved.effect + " is not an effect of this clip's kind; select the " +
                                                       (wanted == TrackKind::Video ? "video" : "audio") + " clip"));
    }
    auto removed = editMediaSite(state, holder->id, [&](StackSite& media, const NewEffectId&) -> std::expected<void, Error> {
        return removeInstance(effect)(media.stack, media.owner);
    });
    if (!removed) {
        return std::unexpected(removed.error());
    }
    // First in the clip's stack, so it still plays before the clip's own effects.
    auto added = editClipStack(removed->state, sequence, clipId, [&](EffectStack& stack, std::string_view) -> std::expected<void, Error> {
        stack.insert(stack.begin(), moved);
        return {};
    });
    if (!added) {
        return std::unexpected(added.error());
    }
    added->changes = diff(state, added->state);
    return added;
}

EditOutcome setMasterEffectsOff(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips, bool off) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto refs = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    SequenceEditor ed(state, **seq);
    for (const ClipRef& r : *refs) {
        const Clip& clip = ed.track(r.track).clips[r.index];
        if (clip.content().masterEffectsOff == off) {
            continue;
        }
        ClipContent content = clip.content();
        content.masterEffectsOff = off;
        ed.mutableClip(r.track, r.index).setContent(std::move(content));
    }
    return ed.finish();
}

} // namespace wabcut::edit
