#pragma once

// The models of the AI video effects (0.7) and how a stage of a chain uses them. The weights are never bundled: each is a
// ModelStore entry of kind Video (ModelStore.cpp: pinned Hugging Face revision, size, SHA-256, licence), downloaded on
// request with its licence card shown first (the effect's own button, AiVideoBridge).
//
//   Denoise   DRUNet colour (MIT): 4 channels in (RGB + a constant noise-level map, sigma / 255), 3 out. Strength is sigma.
//   Deblur    NAFNet GoPro (MIT): 3 in, 3 out, a multiple of 16 wide and high and at least 384 (its channel attention pools
//             over 384 pixel windows: a smaller input fails on the CPU and is wrong on DirectML), so its tiles are never small.
//   Interpolate  RIFE 4.25 lite (MIT, 0.7 AI Frame Interpolation): two frames and a timestep in (img0, img1 1x3xHxW float32 0..1,
//             H and W multiples of 64), the frame between them out. Works on the whole picture, at most 1920 on the long side.
//   Depth     Depth Anything V2 Small (Apache-2.0, 0.7 AI Refocus): pixel_values 1x3xHxW float32 (ImageNet mean and deviation, H and
//             W multiples of 14) in, predicted_depth (relative inverse depth) out, made at 518 on the shorter side. Kept as a map.
//   FaceParse BiSeNet face parsing, ResNet-18 (MIT, 0.7 AI Face Retouch): `input` 1 x 3 x 512 x 512 float32 (ImageNet mean and deviation)
//             in, `output` 1 x 19 x 512 x 512 class logits out (CelebAMask-HQ labels); run on a square crop round each face YuNet
//             (FaceDetect, MIT) finds, both inside one helper request. The result is a map: skin, mouth and eyes coverage.
//   FaceLandmarks MediaPipe face landmarks (Apache-2.0, 0.7 AI Eye Contact): `input_12` 1 x 256 x 256 x 3 float32 (NHWC, RGB 0..1) in, 478 landmarks
//             out; run on a rolled square crop round each face YuNet finds, both inside one helper request. The result is a small table of the
//             eyelid, iris, nose and cheek landmarks (Gaze.h), kept as the eye measurements.
//   Upscale   Real-ESRGAN: general-x4v3 (BSD-3-Clause, a compact 4x model: the Fast upscaler; 2x halves its result),
//             x2plus and x4plus (RRDBNet, BSD-3-Clause: the High quality ones).
//
// A stage is a few steps in a fixed order - denoise, deblur, upscale - each one model run on the whole picture (tiled by
// the helper) and mixed into its input by a strength; only the upscale changes the size.

#include <wabcut/ai/ModelStore.h>
#include <wabcut/model/AiEffects.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::video {

enum class Model { Denoise, Deblur, UpscaleFast4x, Upscale2x, Upscale4x, Interpolate, Depth, FaceDetect, FaceParse, FaceLandmarks };

struct ModelDescription {
    Model model;
    std::string_view key;     ///< what wabcut-embed names it by
    std::string_view storeId; ///< the ModelStore entry
    int scale;                ///< the output is scale x the input
    int multiple;             ///< the input's width and height are a multiple of this
    int minWindow;            ///< the input's width and height are at least this (0: no limit)
    std::string_view name;    ///< for people
};

const ModelDescription& describe(Model model);

/// The size a picture is looked at by the depth model: 518 on the shorter side (kDepthShortSide), the other side in proportion,
/// both at least 2. The depth map is stored at this size too.
void depthWorkingSize(int width, int height, int& workWidth, int& workHeight);

/// The size a picture is looked at by the face models: the longest side at most kFaceMaxSide (never enlarged), the other in
/// proportion, both at least 2. The face map is stored at this size too.
void faceWorkingSize(int width, int height, int& workWidth, int& workHeight);

/// The context a tile takes from its neighbours on every side (pixels): a tile's window is tile + 2 x kTilePad.
inline constexpr int kTilePad = 16;

/// The core size a model really uses for a requested one: the request, made a multiple of 8, and large enough that the window
/// reaches the model's minimum.
int tileFor(Model model, int requested);

/// One model run on a whole picture, and what is done with its result.
struct Step {
    Model model = Model::Denoise;
    double sigma = 0.0;       ///< Denoise: the noise level over 255 (0..1)
    double strength = 1.0;    ///< the result is mixed into the input: in + strength x (out - in); same-size steps only
    double grain = 0.0;       ///< Denoise: the share of the removed noise put back (0..1)
    int resizeDivisor = 1;    ///< the result is made this many times smaller (the fast 2x halves a 4x result)
    int tile = 256;           ///< the core size of the tiles

    int outputScale() const { return describe(model).scale / resizeDivisor; }
    bool operator==(const Step&) const = default;
};

/// The steps of a stage, in order. Empty for a no-op stage.
std::vector<Step> stepsOf(const AiStage& stage);
/// The steps of every stage of a chain, in order.
std::vector<Step> stepsOf(const AiChain& chain);

/// The distinct models a chain runs, in order of first use.
std::vector<Model> modelsOf(const AiChain& chain);
/// Their ModelStore entries (null for an id the manifest lacks - a test checks there is none).
std::vector<const ModelInfo*> modelFilesOf(const AiChain& chain);
/// Every model the chain needs is in the store.
bool modelsInstalled(const AiChain& chain, const ModelStore& store);
/// Bytes still to download for the chain.
std::uint64_t missingBytes(const AiChain& chain, const ModelStore& store);
/// The names, sizes and licences of the models still missing, as the download card shows them.
std::string downloadCard(const AiChain& chain, const ModelStore& store);

/// "Real-ESRGAN x2plus (67 MB), DRUNet denoiser (131 MB)": the models of the chain that are not in the store, for a message.
std::string missingModelsText(const AiChain& chain, const ModelStore& store);

/// A file-name safe identity of the chain's models and of the pre/post-processing (digests of the weights, pipeline
/// version): part of the cache's identity, so a new model or a changed pipeline never reads old pictures.
std::string modelVersion(const AiChain& chain);

/// A guess of the milliseconds one step takes on a `width` x `height` picture on the GPU or the CPU, before one was measured
/// (the first frames of a run replace it with the measurement), and of a whole chain's frame.
double estimateStepMilliseconds(const Step& step, int width, int height, bool gpu);
double estimateFrameMilliseconds(const AiChain& chain, int width, int height, bool gpu);

} // namespace wabcut::ai::video
