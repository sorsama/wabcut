// The motion features (P.2, docs/motion.md): Time Interpolation. Morph Cut is a transition (transition.add with
// "morphCut"); Warp Stabilizer and Rolling Shutter Repair are effects whose Analyze commands live with the analysis
// (src/ai/stabilize).

#include "Catalog.h"

#include <wabcut/model/MotionOps.h>
#include <wabcut/model/TimeInterpolation.h>

#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

std::vector<std::string> interpolationNames() {
    return {std::string(toString(TimeInterpolation::FrameSampling)), std::string(toString(TimeInterpolation::FrameBlending)),
            std::string(toString(TimeInterpolation::OpticalFlow))};
}

} // namespace

Json timeInterpolationParam(std::string description) {
    return schema::enumeration(interpolationNames(), std::move(description));
}

TimeInterpolation timeInterpolationValue(ExecutionContext& c, std::string_view key) {
    const std::string name = c.string(key);
    const auto mode = timeInterpolationFromString(name);
    if (!mode) {
        fail(ErrorCode::InvalidParams, "unknown time interpolation \"" + name.substr(0, 64) + "\"",
             Json{{"path", "/" + std::string(key)}});
    }
    return *mode;
}

void registerMotionCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        {"clip.setTimeInterpolation", 1, "Time Interpolation",
         "Clip > Video Options > Time Interpolation: how retimed video clips make the frames between their media's "
         "frames (a speed other than 100 %, reverse, time remapping, Rate Stretch, or footage at another frame rate "
         "than the sequence's). \"frameSampling\" (the default) repeats or drops whole frames; \"frameBlending\" mixes "
         "the two frames around each moment; \"opticalFlow\" warps both along their motion and blends them (smoothest; "
         "the render bar shows those sections red until rendered). Linked audio partners are left alone.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("The video clips.")},
                       {"method", timeInterpolationParam("Frame Sampling, Frame Blending or Optical Flow.")},
                       {"linked", linkedParam(true)}},
                      {"clips", "method"}),
         Json{{"clips", Json::array({42})}, {"method", "opticalFlow"}}, {},
         [](ExecutionContext& c) {
             const TimeInterpolation mode = timeInterpolationValue(c, "method");
             const std::vector<ClipId> clips = c.clips("clips");
             return edited(c.take(edit::setTimeInterpolation(c.state(), c.sequenceId(), clips, mode, c.linkMode())),
                           Json{{"method", std::string(toString(mode))}, {"clips", clips.size()}});
         }});
}

} // namespace wabcut::command::detail
