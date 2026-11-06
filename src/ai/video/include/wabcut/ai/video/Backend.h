#pragma once

// One model run on one picture: what the Runner asks of whatever executes the models. HelperBackend (HelperBackend.h) is
// the real one - wabcut-embed, ONNX Runtime out of process; the unit tests use a fake that needs no model.

#include <wabcut/ai/video/Picture.h>
#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/model/Error.h>

#include <expected>
#include <stop_token>
#include <string>

namespace wabcut::ai::video {

struct StepResult {
    Picture picture;          ///< colour only, the input's size times the model's scale
    std::string device;       ///< what it ran on ("DirectML", "CPU")
    double milliseconds = 0.0;
    int tiles = 0;
};

class IVideoBackend {
public:
    virtual ~IVideoBackend() = default;
    /// Runs `step.model` over the colour of `in` (tiled by `step.tile`, noise level `step.sigma`); the strength, resize and
    /// grain of the step are the Runner's. Errors: NotFound (the model is not installed), Cancelled, DecoderError /
    /// IoError / VersionMismatch (the helper), InvalidArgument.
    virtual std::expected<StepResult, Error> run(const Picture& in, const Step& step, std::stop_token stop) = 0;
    /// 0.7 (AI Frame Interpolation): the frame between `a` and `b` (two pictures of one size) at `timestep` (0 = a, 1 = b) with
    /// Model::Interpolate; the result is the pictures' size. Errors as run(); a backend without the model says Unsupported.
    virtual std::expected<StepResult, Error> interpolate(const Picture& a, const Picture& b, double timestep, std::stop_token stop) {
        (void)a;
        (void)b;
        (void)timestep;
        (void)stop;
        return std::unexpected(Error(ErrorCode::Unsupported, "this backend cannot interpolate frames"));
    }
    /// What the models ran on so far ("" before the first run).
    virtual std::string device() const { return {}; }
    /// Frees the models' memory; the next run loads them again.
    virtual void release() {}
};

} // namespace wabcut::ai::video
