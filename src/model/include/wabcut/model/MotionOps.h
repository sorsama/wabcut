#pragma once

// Motion edits of the 0.2 round (P.2). Two features share this header (both branches named it MotionOps.h):
//
// * The motion features (docs/motion.md): Time Interpolation on retimed clips. Morph Cut is a transition (effect id
//   kMorphCutTransition, added with the transition ops of FinishingOps.h); Warp Stabilizer and Rolling Shutter Repair
//   are effects. Implemented in TimeInterpolation.cpp.
//
// * Premiere's Motion set as one edit (the Program Monitor's direct manipulation and clip.setMotion): Position, Scale,
//   Rotation and Anchor Point of a video clip changed together at one curve time. Each parameter given follows the
//   Effect Controls rule: a constant takes the new value; an animated parameter (stopwatch on) gets a keyframe there -
//   the key at that time updated, else a new key that continues the shape of the segment it lands in (the previous
//   key's interpolation), as Premiere keys a keyframed property you change at the playhead. Parameters left out keep
//   their values and keys. The whole change is one EditResult, so a drag in the Program Monitor (several parameters at
//   once, e.g. Anchor Point with the Position that keeps the picture in place) is one undo step. Implemented in
//   MotionOps.cpp.

#include <wabcut/model/Clip.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/TimeInterpolation.h>

#include <optional>
#include <span>
#include <string_view>

namespace wabcut {

/// Premiere's Video Transitions > Dissolve > Morph Cut: smooths a jump cut by morphing the outgoing clip's picture at
/// the transition's start into the incoming clip's picture at its end, along their motion (docs/motion.md).
inline constexpr std::string_view kMorphCutTransition = "morphCut";
/// Premiere's Distort > Warp Stabilizer (docs/motion.md "Warp Stabilizer"): effect.analyzeStabilization keys the
/// compensating transform into the instance.
inline constexpr std::string_view kWarpStabilizerEffect = "wabcut.stabilize.warp";
/// Premiere's Distort > Rolling Shutter Repair: effect.analyzeRollingShutter keys the per-frame motion the skew
/// correction needs into the instance.
inline constexpr std::string_view kRollingShutterRepairEffect = "wabcut.stabilize.rollingShutter";

namespace edit {

/// Clip > Video Options > Time Interpolation on `clips` (video clips; with LinkMode::FollowLinks their linked video
/// partners too: audio partners are left alone). One undo step; a clip already set keeps its content block. Errors:
/// NotFound (sequence, clip), InvalidArgument (no video clip among them), locked tracks as resolveClips reports them.
EditOutcome setTimeInterpolation(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                                 TimeInterpolation mode, LinkMode links = LinkMode::FollowLinks);

/// Motion parameters in the model's units (ClipProperties: position and anchor in pixels from the centre, scale as a
/// factor, rotation in degrees clockwise). Unset = unchanged.
struct MotionChange {
    std::optional<double> positionX;
    std::optional<double> positionY;
    std::optional<double> scale;
    std::optional<double> rotation;
    std::optional<double> anchorX;
    std::optional<double> anchorY;

    bool empty() const {
        return !positionX && !positionY && !scale && !rotation && !anchorX && !anchorY;
    }
};

/// The Motion values of a clip at a curve time (Clip::curveTimeAt).
struct MotionValues {
    double positionX = 0.0;
    double positionY = 0.0;
    double scale = 1.0;
    double rotation = 0.0;
    double anchorX = 0.0;
    double anchorY = 0.0;
};
MotionValues motionAt(const Clip& clip, Ticks curveTime);

/// `value` with `v` at curve time `t`: the constant replaced, or for a curve the key at `t` updated, else a new key
/// inserted there with the interpolation of the key before it (linear before the first key).
ParamValue valueSetAt(const ParamValue& value, Ticks t, double v);

/// Sets the parameters `change` names on the video clip `clip` at curve time `curveTime` (see the file comment).
/// Errors: NotFound (sequence, clip), InvalidArgument (an audio clip, a locked track, a value that is not finite or
/// outside the parameter's range, an empty change).
EditOutcome setMotion(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks curveTime,
                      const MotionChange& change);

} // namespace edit

} // namespace wabcut
