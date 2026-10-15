// Masks and source-anchored redactions (I.6, Masks.h): the edit operations.

#include <wabcut/model/ProjectOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <span>
#include <string_view>

namespace wabcut::edit {

using detail::ClipRef;
using detail::invalidArgument;
using detail::notFound;
using detail::SequenceEditor;

namespace {

constexpr std::string_view kMaskEffect = "wabcut.mask";

/// Edits the masks of one effect instance of a clip on an unlocked video track. `effect` invalid: the clip's
/// "wabcut.mask" instance, appended to the stack when `create` and it has none. The masks must stay well formed.
template <class Edit>
std::expected<ProjectState, Error> editMasks(const ProjectState& state, SequenceId sequenceId, ClipId clipId,
                                             EffectInstanceId effect, bool create, Edit&& edit) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto refs = detail::resolveClips(**seq, std::span<const ClipId>(&clipId, 1), LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    const ClipRef site = refs->front();
    SequenceEditor ed(state, **seq);
    const Track& track = ed.track(site.track);
    if (track.kind != TrackKind::Video) {
        return std::unexpected(invalidArgument("masks apply to video clips"));
    }
    const Clip& clip = track.clips[site.index];
    EffectStack stack = clip.effects();
    auto target = stack.end();
    if (effect.isValid()) {
        target = std::ranges::find(stack, effect, &EffectInstance::id);
        if (target == stack.end()) {
            return std::unexpected(notFound("the clip has no effect " + std::to_string(effect.value)));
        }
    } else {
        target = std::ranges::find(stack, kMaskEffect, &EffectInstance::effect);
        if (target == stack.end()) {
            if (!create) {
                return std::unexpected(notFound("the clip has no opacity masks"));
            }
            EffectInstance instance;
            instance.id = ed.newEffectId();
            instance.effect = std::string(kMaskEffect);
            stack.push_back(std::move(instance));
            target = stack.end() - 1;
        }
    }
    if (auto ok = edit(target->masks); !ok) {
        return std::unexpected(ok.error());
    }
    if (const auto problem = masksProblem(target->masks)) {
        return std::unexpected(invalidArgument(*problem));
    }
    if (stack == clip.effects()) {
        return state;
    }
    ed.mutableClip(site.track, site.index).setEffects(std::move(stack));
    return std::move(ed.finish().state);
}

std::expected<void, Error> checkRedaction(const ProjectState& state, const Redaction& redaction) {
    if (!state.findMedia(redaction.media)) {
        return std::unexpected(notFound("media " + std::to_string(redaction.media.value) + " does not exist"));
    }
    if (const auto problem = redactionProblem(redaction)) {
        return std::unexpected(invalidArgument(*problem));
    }
    return {};
}

std::vector<Redaction>::iterator findRedaction(ProjectState& state, RedactionId id) {
    return std::ranges::find(state.redactions, id, &Redaction::id);
}

} // namespace

// ---- Masks ---------------------------------------------------------------------------------------------------------

std::expected<Created<std::uint32_t>, Error> addMask(const ProjectState& state, SequenceId sequence, ClipId clip,
                                                     EffectInstanceId effect, Mask mask) {
    std::uint32_t assigned = 0;
    auto out = editMasks(state, sequence, clip, effect, true, [&](std::vector<Mask>& masks) -> std::expected<void, Error> {
        if (masks.size() >= kMaxMasks) {
            return std::unexpected(invalidArgument("an effect holds at most " + std::to_string(kMaxMasks) + " masks"));
        }
        std::uint32_t highest = 0;
        for (const Mask& m : masks) {
            highest = std::max(highest, m.id);
        }
        assigned = highest + 1;
        mask.id = assigned;
        if (mask.name.empty()) {
            mask.name = "Mask (" + std::to_string(assigned) + ")";
        }
        masks.push_back(std::move(mask));
        return {};
    });
    if (!out) {
        return std::unexpected(out.error());
    }
    return Created<std::uint32_t>{std::move(*out), assigned};
}

std::expected<ProjectState, Error> updateMask(const ProjectState& state, SequenceId sequence, ClipId clip,
                                              EffectInstanceId effect, Mask mask) {
    return editMasks(state, sequence, clip, effect, false, [&](std::vector<Mask>& masks) -> std::expected<void, Error> {
        const auto it = std::ranges::find(masks, mask.id, &Mask::id);
        if (it == masks.end()) {
            return std::unexpected(notFound("no mask " + std::to_string(mask.id)));
        }
        *it = std::move(mask);
        return {};
    });
}

std::expected<ProjectState, Error> removeMask(const ProjectState& state, SequenceId sequence, ClipId clip,
                                              EffectInstanceId effect, std::uint32_t maskId) {
    return editMasks(state, sequence, clip, effect, false, [&](std::vector<Mask>& masks) -> std::expected<void, Error> {
        const auto it = std::ranges::find(masks, maskId, &Mask::id);
        if (it == masks.end()) {
            return std::unexpected(notFound("no mask " + std::to_string(maskId)));
        }
        masks.erase(it);
        return {};
    });
}

// ---- Redactions ----------------------------------------------------------------------------------------------------

std::expected<Created<RedactionId>, Error> addRedaction(const ProjectState& state, Redaction redaction) {
    if (state.redactions.size() >= kMaxRedactions) {
        return std::unexpected(invalidArgument("a project holds at most " + std::to_string(kMaxRedactions) + " redactions"));
    }
    std::ranges::sort(redaction.exceptions);
    redaction.exceptions.erase(std::unique(redaction.exceptions.begin(), redaction.exceptions.end()), redaction.exceptions.end());
    redaction.region.mode = MaskMode::Add;
    if (redaction.region.id == 0) {
        redaction.region.id = 1;
    }
    redaction.id = RedactionId{1}; // checked below with a placeholder id
    if (auto ok = checkRedaction(state, redaction); !ok) {
        return std::unexpected(ok.error());
    }
    Created<RedactionId> out{state, {}};
    out.id = out.state.ids.allocate<RedactionId>();
    redaction.id = out.id;
    out.state.redactions.push_back(std::move(redaction)); // ids only grow: still sorted
    return out;
}

std::expected<ProjectState, Error> updateRedaction(const ProjectState& state, Redaction redaction) {
    ProjectState out = state;
    const auto it = findRedaction(out, redaction.id);
    if (it == out.redactions.end()) {
        return std::unexpected(notFound("redaction " + std::to_string(redaction.id.value) + " does not exist"));
    }
    std::ranges::sort(redaction.exceptions);
    redaction.exceptions.erase(std::unique(redaction.exceptions.begin(), redaction.exceptions.end()), redaction.exceptions.end());
    redaction.region.mode = MaskMode::Add;
    if (redaction.region.id == 0) {
        redaction.region.id = 1;
    }
    if (auto ok = checkRedaction(state, redaction); !ok) {
        return std::unexpected(ok.error());
    }
    *it = std::move(redaction);
    return out;
}

std::expected<ProjectState, Error> removeRedaction(const ProjectState& state, RedactionId redaction) {
    ProjectState out = state;
    const auto it = findRedaction(out, redaction);
    if (it == out.redactions.end()) {
        return std::unexpected(notFound("redaction " + std::to_string(redaction.value) + " does not exist"));
    }
    out.redactions.erase(it);
    return out;
}

std::expected<ProjectState, Error> setRedactionOverride(const ProjectState& state, RedactionId redaction, ClipId clip,
                                                        bool redact) {
    if (!clip.isValid()) {
        return std::unexpected(invalidArgument("a clip is needed"));
    }
    ProjectState out = state;
    const auto it = findRedaction(out, redaction);
    if (it == out.redactions.end()) {
        return std::unexpected(notFound("redaction " + std::to_string(redaction.value) + " does not exist"));
    }
    std::vector<ClipId>& exceptions = it->exceptions;
    const auto at = std::lower_bound(exceptions.begin(), exceptions.end(), clip);
    const bool excepted = at != exceptions.end() && *at == clip;
    if (redact && excepted) {
        exceptions.erase(at);
    } else if (!redact && !excepted) {
        if (exceptions.size() >= kMaxRedactionRanges) {
            return std::unexpected(invalidArgument("too many exceptions"));
        }
        exceptions.insert(at, clip);
    }
    return out;
}

} // namespace wabcut::edit
