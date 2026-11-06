#include <wabcut/ai/video/VideoModels.h>

#include <wabcut/ai/video/ModelKeys.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::ai::video {

namespace {

constexpr std::array<ModelDescription, 10> kModels{{
    {Model::Denoise, "drunet-color", "video-denoise-drunet", 1, 8, 0, "DRUNet denoiser"},
    {Model::Deblur, "nafnet-deblur", "video-deblur-nafnet", 1, 16, 384, "NAFNet deblurring"},
    {Model::UpscaleFast4x, "realesr-general-x4v3", "video-upscale-general-x4v3", 4, 1, 0, "Real-ESRGAN general x4 v3"},
    {Model::Upscale2x, "realesrgan-x2plus", "video-upscale-x2plus", 2, 1, 0, "Real-ESRGAN x2plus"},
    {Model::Upscale4x, "realesrgan-x4plus", "video-upscale-x4plus", 4, 1, 0, "Real-ESRGAN x4plus"},
    {Model::Interpolate, kKeyInterpolate, "video-interpolate-rife", 1, kInterpolateMultiple, 0, "RIFE frame interpolation"},
    {Model::Depth, kKeyDepth, "video-depth-anything-v2-small", 1, kDepthMultiple, 0, "Depth Anything V2 Small"},
    {Model::FaceDetect, kKeyFaceDetect, "yunet-2023mar", 1, 1, 0, "YuNet face detector"},
    {Model::FaceParse, kKeyFaceParse, "video-face-parsing-bisenet", 1, 1, 0, "BiSeNet face parsing"},
    {Model::FaceLandmarks, kKeyFaceLandmarks, "video-face-landmarks", 1, 1, 0, "Face landmarks"},
}};

/// Bumped when the pre/post-processing changes what the same models make (it is part of the cache identity).
constexpr std::string_view kPipelineVersion = "p1";

/// The noise levels the sliders reach, over 255: Denoise at 100 and Restore's light denoise at 100.
constexpr double kDenoiseSigmaMax = 40.0 / 255.0;
constexpr double kRestoreDenoiseSigmaMax = 25.0 / 255.0;

/// Milliseconds per input megapixel (with the tiles' overlap) on a mid-range GPU and on a desktop CPU - first guesses.
struct Speed {
    double gpu;
    double cpu;
};
constexpr Speed speedOf(Model model) {
    switch (model) {
    case Model::Denoise: return {1300.0, 9000.0};
    case Model::Deblur: return {1800.0, 12000.0};
    case Model::UpscaleFast4x: return {160.0, 1100.0};
    case Model::Upscale2x: return {2200.0, 22000.0};
    case Model::Upscale4x: return {2200.0, 22000.0};
    case Model::Interpolate: return {450.0, 3000.0}; // per megapixel of the picture the model runs on (one pair)
    case Model::Depth: return {350.0, 1800.0};       // per megapixel of the 518-line working picture
    case Model::FaceDetect: return {15.0, 60.0};     // included in FaceParse's figure
    case Model::FaceParse: return {90.0, 330.0};     // per megapixel of the working picture (detector + one face)
    case Model::FaceLandmarks: return {25.0, 80.0};  // per megapixel of the working picture (detector + one face: the network is 5 MB)
    }
    return {1000.0, 10000.0};
}

} // namespace

const ModelDescription& describe(Model model) {
    return kModels[static_cast<std::size_t>(model)];
}

int tileFor(Model model, int requested) {
    const int least = describe(model).minWindow > 0 ? describe(model).minWindow - 2 * kTilePad : 0;
    const int tile = std::max(requested, least);
    return (tile + 7) / 8 * 8;
}

std::vector<Step> stepsOf(const AiStage& stage) {
    std::vector<Step> steps;
    const int requested = stage.tile > 0 ? stage.tile : 256;
    if (stage.effect == kAiRestoreEffect) {
        if (stage.denoise > 0) {
            Step s;
            s.model = Model::Denoise;
            s.sigma = kRestoreDenoiseSigmaMax * stage.denoise / 100.0;
            s.grain = stage.grain / 100.0;
            s.tile = tileFor(s.model, requested);
            steps.push_back(s);
        }
        if (stage.deblur > 0) {
            Step s;
            s.model = Model::Deblur;
            s.strength = stage.deblur / 100.0;
            s.tile = tileFor(s.model, requested);
            steps.push_back(s);
        }
        if (stage.scale > 1) {
            Step s;
            if (stage.quality == 1) {
                s.model = stage.scale == 2 ? Model::Upscale2x : Model::Upscale4x;
            } else {
                s.model = Model::UpscaleFast4x;
                s.resizeDivisor = stage.scale == 2 ? 2 : 1;
            }
            s.tile = tileFor(s.model, requested);
            steps.push_back(s);
        }
    } else if (stage.effect == kAiRefocusEffect) {
        Step s;
        s.model = Model::Depth;
        s.tile = tileFor(s.model, requested);
        steps.push_back(s);
    } else if (stage.effect == kAiFaceRetouchEffect) {
        Step s;
        s.model = Model::FaceParse;
        s.tile = tileFor(s.model, requested);
        steps.push_back(s);
    } else if (stage.effect == kAiEyeContactEffect) {
        Step s;
        s.model = Model::FaceLandmarks;
        s.tile = tileFor(s.model, requested);
        steps.push_back(s);
    } else if (stage.effect == kAiInterpolateStage) {
        Step s;
        s.model = Model::Interpolate;
        s.tile = tileFor(s.model, requested);
        steps.push_back(s);
    } else if (stage.effect == kAiDenoiseEffect && stage.denoise > 0) {
        Step s;
        s.model = Model::Denoise;
        s.sigma = kDenoiseSigmaMax * stage.denoise / 100.0;
        s.grain = stage.grain / 100.0;
        s.tile = tileFor(s.model, requested);
        steps.push_back(s);
    }
    return steps;
}

void depthWorkingSize(int width, int height, int& workWidth, int& workHeight) {
    const int shorter = std::max(1, std::min(width, height));
    const double k = static_cast<double>(kDepthShortSide) / shorter;
    workWidth = std::max(2, static_cast<int>(std::lround(width * k)));
    workHeight = std::max(2, static_cast<int>(std::lround(height * k)));
}

void faceWorkingSize(int width, int height, int& workWidth, int& workHeight) {
    const int longest = std::max(1, std::max(width, height));
    const double k = longest > kFaceMaxSide ? static_cast<double>(kFaceMaxSide) / longest : 1.0;
    workWidth = std::max(2, static_cast<int>(std::lround(width * k)));
    workHeight = std::max(2, static_cast<int>(std::lround(height * k)));
}

std::vector<Step> stepsOf(const AiChain& chain) {
    std::vector<Step> steps;
    for (const AiStage& stage : chain.stages) {
        for (const Step& s : stepsOf(stage)) {
            steps.push_back(s);
        }
    }
    return steps;
}

std::vector<Model> modelsOf(const AiChain& chain) {
    std::vector<Model> models;
    for (const Step& s : stepsOf(chain)) {
        if ((s.model == Model::FaceParse || s.model == Model::FaceLandmarks) && std::ranges::find(models, Model::FaceDetect) == models.end()) {
            models.push_back(Model::FaceDetect); // the parser and the landmark network look at the faces the detector finds
        }
        if (std::ranges::find(models, s.model) == models.end()) {
            models.push_back(s.model);
        }
    }
    return models;
}

std::vector<const ModelInfo*> modelFilesOf(const AiChain& chain) {
    std::vector<const ModelInfo*> files;
    for (const Model m : modelsOf(chain)) {
        files.push_back(findModel(describe(m).storeId));
    }
    return files;
}

bool modelsInstalled(const AiChain& chain, const ModelStore& store) {
    const auto files = modelFilesOf(chain);
    return !files.empty() && std::ranges::all_of(files, [&](const ModelInfo* f) { return f != nullptr && store.isInstalled(*f); });
}

std::uint64_t missingBytes(const AiChain& chain, const ModelStore& store) {
    std::uint64_t bytes = 0;
    for (const ModelInfo* f : modelFilesOf(chain)) {
        if (f != nullptr && !store.isInstalled(*f)) {
            bytes += f->size;
        }
    }
    return bytes;
}

std::string missingModelsText(const AiChain& chain, const ModelStore& store) {
    std::string text;
    for (const Model m : modelsOf(chain)) {
        const ModelInfo* info = findModel(describe(m).storeId);
        if (info != nullptr && store.isInstalled(*info)) {
            continue;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += std::string(describe(m).name);
        if (info != nullptr) {
            text += " (" + std::to_string((info->size + 500'000) / 1'000'000) + " MB)";
        }
    }
    return text;
}

std::string downloadCard(const AiChain& chain, const ModelStore& store) {
    std::string card;
    for (const ModelInfo* f : modelFilesOf(chain)) {
        if (f == nullptr || store.isInstalled(*f)) {
            continue;
        }
        if (!card.empty()) {
            card += "\n\n";
        }
        card += f->description + " - " + f->license + ".\n" + f->licenseCard;
    }
    return card;
}

std::string modelVersion(const AiChain& chain) {
    std::string version;
    for (const Model m : modelsOf(chain)) {
        const ModelInfo* info = findModel(describe(m).storeId);
        if (!version.empty()) {
            version += '+';
        }
        version += std::string(describe(m).key) + "-" + (info != nullptr ? info->sha256.substr(0, 8) : std::string("unknown"));
    }
    return version.empty() ? std::string() : version + "-" + std::string(kPipelineVersion);
}

double estimateStepMilliseconds(const Step& step, int width, int height, bool gpu) {
    // The tiles overlap by the pad on every side: more pixels go through the model than the picture has.
    const Speed speed = speedOf(step.model);
    if (step.model == Model::Depth) { // the model looks at a 518-line picture whatever the frame's size
        int w = 0;
        int h = 0;
        depthWorkingSize(width, height, w, h);
        return static_cast<double>(w) * h / 1e6 * (gpu ? speed.gpu : speed.cpu);
    }
    if (step.model == Model::FaceParse || step.model == Model::FaceLandmarks) { // the detector and the models look at the picture at most kFaceMaxSide on the long side
        int w = 0;
        int h = 0;
        faceWorkingSize(width, height, w, h);
        return static_cast<double>(w) * h / 1e6 * (gpu ? speed.gpu : speed.cpu);
    }
    if (step.model == Model::Interpolate) { // one pair at most 1920 on the long side
        const double k = std::min(1.0, static_cast<double>(kInterpolateMaxSide) / std::max(1, std::max(width, height)));
        return static_cast<double>(width) * height * k * k / 1e6 * (gpu ? speed.gpu : speed.cpu);
    }
    const int tile = tileFor(step.model, step.tile);
    const double overlap = std::pow((tile + 2.0 * kTilePad) / tile, 2.0);
    const double megapixels = static_cast<double>(width) * height / 1e6 * overlap;
    return megapixels * (gpu ? speed.gpu : speed.cpu);
}

double estimateFrameMilliseconds(const AiChain& chain, int width, int height, bool gpu) {
    double total = 0.0;
    int w = width;
    int h = height;
    for (const Step& s : stepsOf(chain)) {
        total += estimateStepMilliseconds(s, w, h, gpu);
        w *= s.outputScale();
        h *= s.outputScale();
    }
    return total;
}

} // namespace wabcut::ai::video
