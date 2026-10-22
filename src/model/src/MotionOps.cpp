// Motion as one edit (P.2): each parameter goes through setParameter (its checks: the clip, its track, the range), the
// steps chained on one state and reported as one ChangeSet.

#include <wabcut/model/MotionOps.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace wabcut::edit {

MotionValues motionAt(const Clip& clip, Ticks curveTime) {
    const ClipProperties& p = clip.properties();
    MotionValues v;
    v.positionX = p.value(ParamId::PositionX, curveTime);
    v.positionY = p.value(ParamId::PositionY, curveTime);
    v.scale = p.value(ParamId::Scale, curveTime);
    v.rotation = p.value(ParamId::Rotation, curveTime);
    v.anchorX = p.value(ParamId::AnchorX, curveTime);
    v.anchorY = p.value(ParamId::AnchorY, curveTime);
    return v;
}

ParamValue valueSetAt(const ParamValue& value, Ticks t, double v) {
    const Curve* curve = value.curve();
    if (!curve) {
        return ParamValue(v);
    }
    Curve keys = *curve;
    if (const auto at = keys.keyAt(t)) {
        keys.keys[*at].value = v;
    } else {
        const std::size_t index = keys.insertionIndex(t);
        Keyframe key{t, v};
        if (index > 0) {
            key.interpolation = keys.keys[index - 1].interpolation; // continue the segment's shape
        }
        keys.keys.insert(keys.keys.begin() + static_cast<std::ptrdiff_t>(index), key);
    }
    return ParamValue(std::move(keys));
}

EditOutcome setMotion(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks curveTime,
                      const MotionChange& change) {
    if (change.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "no Motion parameter to set"});
    }
    const std::array<std::pair<ParamId, std::optional<double>>, 6> wanted{{
        {ParamId::PositionX, change.positionX},
        {ParamId::PositionY, change.positionY},
        {ParamId::Scale, change.scale},
        {ParamId::Rotation, change.rotation},
        {ParamId::AnchorX, change.anchorX},
        {ParamId::AnchorY, change.anchorY},
    }};
    ProjectState working = state;
    bool changed = false;
    for (const auto& [id, value] : wanted) {
        if (!value) {
            continue;
        }
        if (!std::isfinite(*value)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         std::string(parameterInfo(id).key) + " must be a finite number"});
        }
        const Sequence* seq = working.findSequence(sequence);
        const auto located = seq ? seq->findClip(clip) : std::nullopt;
        if (!located) {
            return std::unexpected(Error{ErrorCode::NotFound, "no clip " + std::to_string(clip.value) + " in the sequence"});
        }
        const ParamValue& current = located->clip().properties()[id];
        ParamValue next = valueSetAt(current, curveTime, *value);
        if (next == current) {
            continue;
        }
        auto step = setParameter(working, sequence, clip, ParamRef::of(id), std::move(next));
        if (!step) {
            return std::unexpected(std::move(step.error()));
        }
        working = std::move(step->state);
        changed = true;
    }
    EditResult result;
    if (!changed) {
        // Nothing differs: still check the clip can be edited, so a locked or audio clip is refused the same way.
        const auto first = std::find_if(wanted.begin(), wanted.end(), [](const auto& w) { return w.second.has_value(); });
        const Sequence* seq = state.findSequence(sequence);
        const auto located = seq ? seq->findClip(clip) : std::nullopt;
        if (!located) {
            return std::unexpected(Error{ErrorCode::NotFound, "no clip " + std::to_string(clip.value) + " in the sequence"});
        }
        auto check = setParameter(state, sequence, clip, ParamRef::of(first->first), located->clip().properties()[first->first]);
        if (!check) {
            return std::unexpected(std::move(check.error()));
        }
        result.state = state;
        return result;
    }
    result.changes = diff(state, working);
    result.state = std::move(working);
    return result;
}

} // namespace wabcut::edit
