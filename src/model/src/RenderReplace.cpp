// Render and Replace for nested sequences, and Restore Unrendered (RenderReplace.h).

#include <wabcut/model/RenderReplace.h>

#include <wabcut/model/Effects.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/ProjectOps.h>
#include "SequenceEditor.h"

#include <algorithm>
#include <optional>
#include <string>
#include <variant>

namespace wabcut::edit {

using detail::SequenceEditor;

namespace {

std::string clipLabel(const Clip& clip) {
    return clip.name().empty() ? "clip " + std::to_string(clip.id.value) : "'" + clip.name() + "'";
}

bool hasMatteMask(const Clip& clip) {
    for (const EffectInstance& effect : clip.content().effects) {
        for (const Mask& mask : effect.masks) {
            if (mask.shape == MaskShape::Matte) {
                return true;
            }
        }
    }
    return false;
}

/// Why Render and Replace cannot take `clip` (a nested clip), or nullopt.
std::optional<std::string> refusal(const ProjectState& state, const Clip& clip) {
    const Sequence* nested = state.findSequence(clip.sequence);
    if (!nested) {
        return clipLabel(clip) + " plays a sequence that is not in the project";
    }
    if (clip.audition()) {
        return clipLabel(clip) + " is an audition: use Flatten Audition first";
    }
    if (nested->multicam) {
        return clipLabel(clip) + " is a multi-camera clip: it shows one angle of its source";
    }
    if (clip.isRemapped()) {
        return clipLabel(clip) + " has Time Remapping: remove it before rendering";
    }
    if (hasMatteMask(clip)) {
        return clipLabel(clip) + " has an object mask: remove it before rendering";
    }
    if (!sequenceHasStream(*nested, clip.stream)) {
        return clipLabel(clip) + (clip.stream.kind == StreamKind::Video ? " has no picture to render in its nested sequence"
                                                                         : " has no sound to render in its nested sequence");
    }
    return std::nullopt;
}

/// What the clip plays of its nested sequence: [sourceIn, sourceOut), at least one frame (a freeze frame holds one).
TimeRange usedRange(const Clip& clip, Ticks frame) {
    return TimeRange::fromStartEnd(clip.sourceIn, std::max(clip.sourceOut(), clip.sourceIn + frame));
}

/// Moves the key times of a keyframed value `shift` earlier (a constant is left alone).
void shiftValue(ParamValue& value, Ticks shift) {
    if (auto* curve = std::get_if<Curve>(&value.data)) {
        for (Keyframe& key : curve->keys) {
            key.time = key.time - shift;
        }
    }
}

void shiftMask(Mask& mask, Ticks shift) {
    for (MaskParam& param : mask.params) {
        shiftValue(param.value, shift);
    }
    for (MaskPathKey& key : mask.path) {
        key.time = key.time - shift;
    }
}

/// Everything of the clip keyed to its source's own time (Parameters.h: curve time is media time) moves `shift`
/// earlier: its In point, the keyframes of its parameters and effects and their masks' keys. Remapped clips are refused
/// before this (their curve values are source times too).
void shiftSourceTime(Clip& clip, Ticks shift) {
    ClipContent content = clip.content();
    for (ParamValue& value : content.properties.values) {
        shiftValue(value, shift);
    }
    for (EffectInstance& effect : content.effects) {
        for (EffectParam& param : effect.params) {
            shiftValue(param.value, shift);
        }
        for (Mask& mask : effect.masks) {
            shiftMask(mask, shift);
        }
    }
    clip.setContent(std::move(content));
    clip.sourceIn = clip.sourceIn - shift;
}

Ticks floorToFrame(Ticks t, Rate rate) { return snapToFrame(t, rate); }

Ticks ceilToFrame(Ticks t, Rate rate) {
    const Ticks floored = snapToFrame(t, rate);
    return floored == t ? t : floored + framesToTicks(1, rate);
}

} // namespace

std::expected<std::vector<NestRenderUnit>, Error> planRenderAndReplace(const ProjectState& state, SequenceId outerId,
                                                                       std::span<const ClipId> ids, Ticks handle) {
    const auto found = detail::findSequence(state, outerId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const Sequence& outer = **found;
    if (handle.value < 0) {
        handle = Ticks{};
    }
    const auto refs = detail::resolveClips(outer, ids, LinkMode::FollowLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    std::vector<NestRenderUnit> units;
    for (const detail::ClipRef& ref : *refs) {
        const Clip& clip = outer.findTrack(ref.track)->clips[ref.index];
        if (!clip.isNested()) {
            continue; // a linked partner that is plain media: left alone
        }
        if (const auto why = refusal(state, clip)) {
            return std::unexpected(detail::invalidArgument(*why));
        }
        const Sequence& nested = *state.findSequence(clip.sequence);
        const Ticks frame = framesToTicks(1, nested.frameRate);
        const TimeRange used = usedRange(clip, frame);
        auto unit = std::find_if(units.begin(), units.end(), [&](const NestRenderUnit& u) {
            return u.nested == clip.sequence && u.used.start == used.start && u.used.duration == used.duration;
        });
        if (unit == units.end()) {
            NestRenderUnit fresh;
            fresh.nested = clip.sequence;
            fresh.used = used;
            const Ticks from = floorToFrame(std::max(used.start - handle, Ticks{}), nested.frameRate);
            const Ticks to = ceilToFrame(std::min(used.end() + handle, nested.duration), nested.frameRate);
            if (!(from < to)) {
                return std::unexpected(detail::invalidArgument(clipLabel(clip) + " plays nothing: it lies past the end of '" +
                                                               nested.name + "'"));
            }
            fresh.range = TimeRange::fromStartEnd(from, to);
            units.push_back(std::move(fresh));
            unit = units.end() - 1;
        }
        unit->clips.push_back(clip.id);
        (clip.stream.kind == StreamKind::Video ? unit->video : unit->audio) = true;
    }
    if (units.empty()) {
        return std::unexpected(detail::invalidArgument("select a nested sequence clip first"));
    }
    // A picture clip first, then the sound, in the order they were found otherwise.
    for (NestRenderUnit& unit : units) {
        std::stable_sort(unit.clips.begin(), unit.clips.end(), [&](ClipId a, ClipId b) {
            const auto kindOf = [&](ClipId id) { return outer.findClip(id)->clip().stream.kind; };
            return kindOf(a) == StreamKind::Video && kindOf(b) != StreamKind::Video;
        });
    }
    return units;
}

bool nestMayBeTransparent(const ProjectState& state, SequenceId nestedId, TimeRange range) {
    const Sequence* nested = state.findSequence(nestedId);
    if (!nested) {
        return false;
    }
    const Track* base = nullptr; // the lowest enabled video track that holds anything
    for (const TrackPtr& track : nested->videoTracks) {
        if (track && track->enabled && !track->clips.empty()) {
            base = track.get();
            break;
        }
    }
    if (!base) {
        return !nested->videoTracks.empty(); // a picture with nothing in it is all transparent
    }
    Ticks at = range.start;
    for (const Clip& clip : base->clips) {
        if (clip.end() <= at) {
            continue;
        }
        if (clip.start() > at) {
            return true; // a gap shows what is below: nothing
        }
        if (!clip.enabled || clip.graphic() || clip.content().blendMode != BlendMode::Normal) {
            return true;
        }
        at = clip.end();
        if (at >= range.end()) {
            return false;
        }
    }
    return at < range.end();
}

EditOutcome renderAndReplace(const ProjectState& state, SequenceId outerId, std::span<const ClipId> ids, MediaItem rendered,
                             Ticks origin) {
    const auto found = detail::findSequence(state, outerId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const Sequence& outer = **found;
    const auto refs = detail::resolveClips(outer, ids, LinkMode::FollowLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    // Every nested clip must play the same nest and be held by the file.
    SequenceId nestedId;
    std::vector<detail::ClipRef> targets;
    for (const detail::ClipRef& ref : *refs) {
        const Clip& clip = outer.findTrack(ref.track)->clips[ref.index];
        if (!clip.isNested()) {
            continue;
        }
        if (nestedId.isValid() && clip.sequence != nestedId) {
            return std::unexpected(detail::invalidArgument(clipLabel(clip) + " plays another sequence than the other clips"));
        }
        if (const auto why = refusal(state, clip)) {
            return std::unexpected(detail::invalidArgument(*why));
        }
        nestedId = clip.sequence;
        const Sequence& nested = *state.findSequence(nestedId);
        const Ticks frame = framesToTicks(1, nested.frameRate);
        const TimeRange used = usedRange(clip, frame);
        if (!rendered.hasStream(clip.stream)) {
            return std::unexpected(detail::invalidArgument(std::string("the rendered file has no ") +
                                                           (clip.stream.kind == StreamKind::Video ? "picture" : "sound")));
        }
        const Ticks first = used.start - origin;
        if (first.value < 0) {
            return std::unexpected(detail::invalidArgument(clipLabel(clip) + " starts before the rendered file does"));
        }
        if (const auto extent = rendered.sourceExtent(clip.stream); extent && used.end() - origin > *extent + frame) {
            return std::unexpected(detail::invalidArgument(clipLabel(clip) + " reaches past the end of the rendered file"));
        }
        targets.push_back(ref);
    }
    if (targets.empty()) {
        return std::unexpected(detail::invalidArgument("select a nested sequence clip first"));
    }
    const Sequence& nested = *state.findSequence(nestedId);
    if (!rendered.bin.isValid()) {
        rendered.bin = nested.bin;
    }
    auto added = addMedia(state, std::move(rendered));
    if (!added) {
        return std::unexpected(added.error());
    }
    SequenceEditor ed(added->state, outer);
    for (const detail::ClipRef& ref : targets) {
        Clip& clip = ed.mutableClip(ref.track, ref.index);
        shiftSourceTime(clip, origin);
        ClipContent content = clip.content();
        content.renderedNest = RenderedNest{nestedId, origin};
        clip.setContent(std::move(content));
        clip.media = added->id;
        clip.sequence = SequenceId{};
        clip.angles = AngleSet{};
    }
    EditResult result = ed.finish();
    result.createdMedia = added->id;
    return result;
}

EditOutcome restoreUnrendered(const ProjectState& state, SequenceId outerId, std::span<const ClipId> ids) {
    const auto found = detail::findSequence(state, outerId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const Sequence& outer = **found;
    const auto refs = detail::resolveClips(outer, ids, LinkMode::FollowLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    SequenceEditor ed(state, outer);
    bool any = false;
    for (const detail::ClipRef& ref : *refs) {
        const Clip& clip = outer.findTrack(ref.track)->clips[ref.index];
        const RenderedNest* from = clip.renderedNest();
        if (!from) {
            continue;
        }
        const Sequence* nested = state.findSequence(from->sequence);
        if (!nested) {
            return std::unexpected(detail::notFound(clipLabel(clip) + " was rendered from a sequence that is not in the project any more"));
        }
        if (sequenceContains(state, from->sequence, outerId)) {
            return std::unexpected(detail::invalidArgument("'" + nested->name + "' would contain itself if " + clipLabel(clip) +
                                                           " played it again"));
        }
        if (!sequenceHasStream(*nested, clip.stream)) {
            return std::unexpected(detail::invalidArgument("'" + nested->name + "' has no " +
                                                           (clip.stream.kind == StreamKind::Video ? "picture" : "sound") +
                                                           " any more"));
        }
        Clip& target = ed.mutableClip(ref.track, ref.index);
        const SequenceId restored = from->sequence;
        const Ticks origin = from->origin;
        shiftSourceTime(target, Ticks{} - origin); // later by `origin`
        ClipContent content = target.content();
        content.renderedNest.reset();
        target.setContent(std::move(content));
        target.media = MediaId{};
        target.sequence = restored;
        any = true;
    }
    if (!any) {
        return std::unexpected(detail::invalidArgument("none of the clips was rendered and replaced"));
    }
    return ed.finish();
}

} // namespace wabcut::edit
