// The AI video effects (0.7): descriptors and the chain of a clip's source-stage effects (AiEffects.h).

#include "EffectsAi.h"

#include <wabcut/model/AiEffects.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut {

namespace detail {

namespace {

using C = EffectCapability;
using U = ParamUnit;

// Restore. Scale / Quality / Deblur / Denoise / Preserve Grain / Tile Size are constants (they decide what the models
// make, which is kept in the clip's cache); Amount mixes the processed picture with the unprocessed one and can be
// keyframed.
constexpr std::array<EffectParamInfo, 7> kRestoreParams{{
    {"scale", "Scale", U::Amount, 0.0, 0.0, 3.0, false, "Auto|1x (no upscale)|2x|4x"},
    {"quality", "Upscaler", U::Amount, 0.0, 0.0, 1.0, false, "Fast|High quality"},
    {"deblur", "Deblur", U::Amount, 50.0, 0.0, 100.0, false},
    {"denoise", "Light Denoise", U::Amount, 0.0, 0.0, 100.0, false},
    {"grain", "Preserve Grain", U::Amount, 0.0, 0.0, 100.0, false},
    {"tile", "Tile Size", U::Amount, 0.0, 0.0, 3.0, false, "Auto|128|256|512"},
    {"amount", "Amount", U::Amount, 100.0, 0.0, 100.0},
}};

constexpr std::array<EffectParamInfo, 4> kDenoiseParams{{
    {"strength", "Strength", U::Amount, 40.0, 0.0, 100.0, false},
    {"grain", "Preserve Grain", U::Amount, 15.0, 0.0, 100.0, false},
    {"tile", "Tile Size", U::Amount, 0.0, 0.0, 3.0, false, "Auto|128|256|512"},
    {"amount", "Amount", U::Amount, 100.0, 0.0, 100.0},
}};

// Refocus (card #181). The depth maps are made once (a depth model, kept in the processed cache); every setting below only
// changes how the compositor blurs, so all of them are cheap and can be keyframed (a focus pull). Distances are 0 = the nearest
// part of the picture, 100 = the farthest.
constexpr std::array<EffectParamInfo, 6> kRefocusParams{{
    {"focus", "Focus", U::Amount, 50.0, 0.0, 100.0},
    {"range", "Sharp Zone", U::Amount, 20.0, 0.0, 100.0},
    {"amount", "Blur Size", U::Amount, 50.0, 0.0, 100.0},
    {"foreground", "Near Blur", U::Amount, 100.0, 0.0, 100.0},
    {"background", "Far Blur", U::Amount, 100.0, 0.0, 100.0},
    {"view", "Show", U::Amount, 0.0, 0.0, 2.0, false, "Result|Depth Map|Blur Map"},
}};

// Face Retouch (card #132). The face maps are made once (a face-parsing model, kept in the processed cache); every setting below only
// changes what the compositor does with them, so all of them are cheap and can be keyframed. Sizes are sequence pixels. With every
// amount at 0 the picture is left exactly as it is. The labels are short: the Effect Controls column cuts a name past about 12 letters.
constexpr std::array<EffectParamInfo, 8> kFaceRetouchParams{{
    {"smoothing", "Smoothing", U::Amount, 50.0, 0.0, 100.0},
    {"detail", "Keep Detail", U::Amount, 30.0, 0.0, 100.0},
    {"size", "Smooth Size", U::Pixels, 6.0, 0.0, 60.0},
    {"blemish", "Blemishes", U::Amount, 50.0, 0.0, 100.0},
    {"blemishSize", "Blemish Size", U::Pixels, 10.0, 0.0, 60.0},
    {"teeth", "Whiten Teeth", U::Amount, 40.0, 0.0, 100.0},
    {"eyes", "Bright Eyes", U::Amount, 30.0, 0.0, 100.0},
    {"view", "Show", U::Amount, 0.0, 0.0, 3.0, false, "Result|Skin Map|Mouth Map|Eye Map"},
}};

// Eye Contact (card #72). The eye measurements are made once (a landmark model, kept in the processed cache); every setting below only changes
// what the compositor does with them (EyeContact.h), so all of them are cheap and can be keyframed. Gaze offsets are percent of an eye's width.
// Amount 0 leaves the picture exactly as it is. The labels are short: the Effect Controls column cuts a name past about 12 letters.
constexpr std::array<EffectParamInfo, 6> kEyeContactParams{{
    {"amount", "Amount", U::Amount, 100.0, 0.0, 100.0},
    {"downFrom", "Down From", U::Amount, 5.0, 0.0, 100.0},
    {"downTo", "Down To", U::Amount, 45.0, 0.0, 100.0},
    {"sideLimit", "Side Limit", U::Amount, 25.0, 0.0, 100.0},
    {"faces", "Faces", U::Amount, 0.0, 0.0, 1.0, false, "All|Largest"},
    {"view", "Show", U::Amount, 0.0, 0.0, 1.0, false, "Result|Eye Map"},
}};

} // namespace

void appendAiDescriptors(std::vector<EffectDescriptor>& out) {
    const auto video = EffectKind::Video;
    out.push_back({kAiRestoreEffect, 1, "AI Restore", "AI", video, false, kRestoreParams, C::None,
                   "Upscales, deblurs and lightly denoises the footage with local AI models (downloaded on request, run on "
                   "this computer). It works on the source picture, before the clip's other effects and its colour: Process "
                   "makes the pictures ahead of time and keeps them for the clip; playback shows them in place of the "
                   "decoded ones (unprocessed frames are shown as they are, with a badge). Amount mixes the processed "
                   "picture with the unprocessed one."});
    out.push_back({kAiDenoiseEffect, 1, "AI Denoise", "AI", video, false, kDenoiseParams, C::None,
                   "Removes sensor and compression noise from low-light and high-ISO footage with a local AI model "
                   "(downloaded on request, run on this computer). Strength is the noise level the model assumes; Preserve "
                   "Grain puts part of the removed noise back so the picture does not turn waxy. Process makes the pictures "
                   "ahead of time and keeps them for the clip."});
    out.push_back({kAiRefocusEffect, 1, "AI Refocus", "AI", video, false, kRefocusParams, C::None,
                   "Moves the focus after the shot. A local AI model (downloaded on request, run on this computer) estimates how "
                   "far each part of the picture is; the effect blurs the picture like a lens by the distance from Focus "
                   "(0 = nearest, 100 = farthest): Sharp Zone is how deep the in-focus zone is, Blur Size the largest blur, "
                   "Near Blur and Far Blur choose the side. Focus can be keyframed for a focus pull. Process "
                   "makes the depth maps ahead of time. Put it first in the clip's effects: it works on the picture as the "
                   "footage has it."});
    out.push_back({kAiFaceRetouchEffect, 1, "AI Face Retouch", "AI", video, false, kFaceRetouchParams, C::None,
                   "Retouches faces with a local AI model (downloaded on request, run on this computer) that finds each face and "
                   "tells skin from eyes, mouth, hair and background. Smoothing evens the skin and Keep Detail leaves part of "
                   "its texture; Blemishes heals small spots in the skin; Whiten Teeth and Bright Eyes act only on the "
                   "teeth and the whites of the eyes. Everything at 0 leaves the picture as it is. The settings can be keyframed. "
                   "Process makes the face maps ahead of time; frames without one are shown as they are."});
    out.push_back({kAiEyeContactEffect, 1, "AI Eye Contact", "AI", video, false, kEyeContactParams, C::None,
                   "Makes a speaker who reads notes below the camera look into it. A local AI model (the face landmarks, downloaded on "
                   "request, run on this computer) finds each eye and its iris; the effect moves the iris towards where it sits when the "
                   "person looks into the lens, inside the opening of the eye: the lids never move, the sclera fills what the iris "
                   "left and nothing outside the eyes is touched. Amount is how far the iris goes. Down From and Down To are the "
                   "range of looking down (percent of the eye's width) that is corrected, Side Limit stops it for a deliberate glance "
                   "to a side; blinks and heads turned away are left alone, and every face is corrected on its own. Faces can limit it to the "
                   "largest one. Process makes the measurements ahead of time; frames without them are shown as they are."});
}

} // namespace detail

bool isAiVideoEffect(std::string_view effect) { return effect == kAiRestoreEffect || effect == kAiDenoiseEffect; }

bool AiStage::isNoop() const {
    if (effect == kAiRestoreEffect) {
        return scale <= 1 && deblur <= 0 && denoise <= 0;
    }
    if (isSideProduct()) {
        return false; // depth maps, in-between frames: always work to do
    }
    return denoise <= 0;
}

int AiChain::outputScale() const {
    int scale = 1;
    for (const AiStage& stage : stages) {
        scale *= std::max(1, stage.outputScale());
    }
    return scale;
}

int aiAutoScale(int mediaHeight, int sequenceHeight) {
    if (mediaHeight <= 0 || sequenceHeight <= 0) {
        return 2;
    }
    if (mediaHeight >= sequenceHeight) {
        return 1;
    }
    return mediaHeight * 2 >= sequenceHeight * 0.9 ? 2 : 4;
}

namespace {

int whole(const EffectInstance& instance, std::string_view key, int fallback) {
    const double v = effectParameterAt(instance, key, Ticks{}, static_cast<double>(fallback));
    return static_cast<int>(std::lround(std::isfinite(v) ? v : static_cast<double>(fallback)));
}

int tileSize(int choice) {
    constexpr std::array<int, 4> kTiles{0, 128, 256, 512};
    return kTiles[static_cast<std::size_t>(std::clamp(choice, 0, 3))];
}

} // namespace

std::optional<AiStage> aiStageOf(const EffectInstance& instance, int mediaHeight, int sequenceHeight) {
    if (!isAiVideoEffect(instance.effect)) {
        return std::nullopt;
    }
    AiStage stage;
    stage.effect = instance.effect;
    stage.tile = tileSize(whole(instance, "tile", 0));
    stage.grain = std::clamp(whole(instance, "grain", instance.effect == kAiDenoiseEffect ? 15 : 0), 0, 100);
    if (instance.effect == kAiRestoreEffect) {
        const int choice = std::clamp(whole(instance, "scale", 0), 0, 3);
        stage.scale = choice == 0 ? aiAutoScale(mediaHeight, sequenceHeight) : choice == 1 ? 1 : choice == 2 ? 2 : 4;
        stage.quality = std::clamp(whole(instance, "quality", 0), 0, 1);
        stage.deblur = std::clamp(whole(instance, "deblur", 50), 0, 100);
        stage.denoise = std::clamp(whole(instance, "denoise", 0), 0, 100);
    } else {
        stage.denoise = std::clamp(whole(instance, "strength", 40), 0, 100);
    }
    if (stage.denoise <= 0) {
        stage.grain = 0; // nothing was taken out, nothing to put back
    }
    return stage;
}

AiChain aiProcessingChainOf(const EffectStack& stack, int mediaHeight, int sequenceHeight) {
    AiChain chain;
    for (const EffectInstance& instance : stack) {
        if (!instance.enabled || !isAiVideoEffect(instance.effect)) {
            continue;
        }
        if (const auto stage = aiStageOf(instance, mediaHeight, sequenceHeight); stage && !stage->isNoop()) {
            chain.stages.push_back(*stage);
        }
    }
    return chain;
}

AiChain aiChainOf(const EffectStack& stack, Ticks curveTime, int mediaHeight, int sequenceHeight, const EffectRegistry& registry) {
    AiChain chain;
    for (const EffectInstance& instance : stack) {
        if (!instance.enabled || !isAiVideoEffect(instance.effect) || !isEvaluable(instance, EffectKind::Video, registry)) {
            continue;
        }
        const auto stage = aiStageOf(instance, mediaHeight, sequenceHeight);
        if (!stage || stage->isNoop()) {
            continue;
        }
        chain.stages.push_back(*stage);
        const double amount = std::clamp(effectParameterAt(instance, "amount", curveTime, 100.0, registry) / 100.0, 0.0, 1.0);
        chain.amount *= amount;
    }
    if (chain.amount <= 0.0) {
        chain.stages.clear(); // Amount 0: the picture as it is, nothing to process or to show
        chain.amount = 1.0;
    }
    return chain;
}

AiChain aiDepthChain() {
    AiChain chain;
    AiStage stage;
    stage.effect = std::string(kAiRefocusEffect);
    chain.stages.push_back(std::move(stage));
    return chain;
}

AiChain aiDepthChainOf(const EffectStack& stack) {
    for (const EffectInstance& instance : stack) {
        if (instance.enabled && isAiRefocusEffect(instance.effect) && isEvaluable(instance, EffectKind::Video)) {
            return aiDepthChain(); // one depth map serves every Refocus of the clip
        }
    }
    return AiChain{};
}

AiChain aiFaceChain() {
    AiChain chain;
    AiStage stage;
    stage.effect = std::string(kAiFaceRetouchEffect);
    chain.stages.push_back(std::move(stage));
    return chain;
}

AiChain aiFaceChainOf(const EffectStack& stack) {
    for (const EffectInstance& instance : stack) {
        if (instance.enabled && isAiFaceRetouchEffect(instance.effect) && isEvaluable(instance, EffectKind::Video)) {
            return aiFaceChain(); // one set of face maps serves every Face Retouch of the clip
        }
    }
    return AiChain{};
}

AiChain aiGazeChain() {
    AiChain chain;
    AiStage stage;
    stage.effect = std::string(kAiEyeContactEffect);
    chain.stages.push_back(std::move(stage));
    return chain;
}

AiChain aiGazeChainOf(const EffectStack& stack) {
    for (const EffectInstance& instance : stack) {
        if (instance.enabled && isAiEyeContactEffect(instance.effect) && isEvaluable(instance, EffectKind::Video)) {
            return aiGazeChain(); // one set of eye measurements serves every Eye Contact of the clip
        }
    }
    return AiChain{};
}

AiChain aiInterpolationChain() {
    AiChain chain;
    AiStage stage;
    stage.effect = std::string(kAiInterpolateStage);
    chain.stages.push_back(std::move(stage));
    return chain;
}

std::uint64_t aiChainHash(const AiChain& chain) {
    if (chain.stages.empty()) {
        return 0;
    }
    std::uint64_t h = 1469598103934665603ull; // FNV-1a
    const auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFFu;
            h *= 1099511628211ull;
        }
    };
    for (const AiStage& s : chain.stages) {
        for (const char c : s.effect) {
            mix(static_cast<unsigned char>(c));
        }
        mix(static_cast<std::uint64_t>(s.scale));
        mix(static_cast<std::uint64_t>(s.quality));
        mix(static_cast<std::uint64_t>(s.deblur));
        mix(static_cast<std::uint64_t>(s.denoise));
        mix(static_cast<std::uint64_t>(s.grain));
        mix(static_cast<std::uint64_t>(s.tile));
        mix(0xFFull); // stage boundary
    }
    return h == 0 ? 1 : h;
}

std::string aiChainLabel(const AiChain& chain) {
    std::string out;
    for (const AiStage& s : chain.stages) {
        if (!out.empty()) {
            out += ", ";
        }
        if (s.effect == kAiRestoreEffect) {
            out += s.scale > 1 ? "Restore " + std::to_string(s.scale) + "x" : "Restore";
        } else if (s.effect == kAiRefocusEffect) {
            out += "Depth maps";
        } else if (s.effect == kAiFaceRetouchEffect) {
            out += "Face maps";
        } else if (s.effect == kAiEyeContactEffect) {
            out += "Eye measurements";
        } else if (s.effect == kAiInterpolateStage) {
            out += "Frame interpolation";
        } else {
            out += "Denoise";
        }
    }
    return out;
}

} // namespace wabcut
