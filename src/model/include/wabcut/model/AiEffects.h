#pragma once

// AI video effects (0.7, docs/ai.md "AI video effects"): Restore (upscale, deblur, light denoise) and Denoise. They are
// ordinary clip effects: parameters in Effect Controls, keyframes on Amount, saved with the project, one undo step per
// edit. What sets them apart is where they run. A model needs seconds per picture, so the compositor never runs them:
// they work at the *source stage*, before the clip's other effects and its colour. A clip's AI effects form a chain
// (stack order); the chain's processed pictures are made ahead of time by "Process" (or in the background), kept in a
// per-clip cache (wabcut/ai/video/ProcessedCache.h) and shown in place of the decoded picture. Where a picture is not
// processed yet, the unprocessed one is shown and the monitor says so.
//
// This header is the data form shared by the evaluator, the runner and the app: the effect ids, the chain of a clip with
// "Auto" resolved against the media and the sequence, and the hash of everything that changes the processed pixels
// (Amount is not part of it: it mixes the processed picture with the unprocessed one when the picture is shown).

#include <wabcut/model/Effects.h>
#include <wabcut/model/EyeContact.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

inline constexpr std::string_view kAiRestoreEffect = "wabcut.ai.restore";
inline constexpr std::string_view kAiDenoiseEffect = "wabcut.ai.denoise";
/// 0.7 (AI Refocus, card #181): a compositor effect, not a source-stage one. A depth model makes a depth map of each frame (kept
/// in the processed cache like the source-stage pictures); the effect blurs the picture by how far each part lies from the
/// focus distance, which can be keyframed. Its stage in a chain makes the depth map; it is never part of a clip's source chain.
inline constexpr std::string_view kAiRefocusEffect = "wabcut.ai.refocus";
/// 0.7 (AI Face Retouch, card #132): a compositor effect like Refocus. A face-parsing model makes a map of each frame (which pixels
/// are skin, the inside of the mouth, the eyes; kept in the processed cache); the effect smooths the skin, heals blemishes, whitens
/// teeth and brightens the eyes only where the map says they belong. Its stage makes the face maps and is never part of a clip's
/// source chain.
inline constexpr std::string_view kAiFaceRetouchEffect = "wabcut.ai.faceRetouch";
/// 0.7 (AI Eye Contact, card #72; kAiEyeContactEffect in EyeContact.h): a compositor effect like Face Retouch. A landmark model measures the
/// eyes of every face of each frame (kept in the processed cache); the effect moves each iris towards the camera inside the opening of its
/// eye where the gaze is in the effect's ranges. Its stage makes the measurements and is never part of a clip's source chain.
/// 0.7 (AI Frame Interpolation, card #133): the pseudo-stage of the chain a clip's AI Frame Interpolation (Time Interpolation)
/// makes. It names a job and its cache, no effect has this id.
inline constexpr std::string_view kAiInterpolateStage = "wabcut.ai.interpolate";

/// One of the two AI source-stage effects (Restore, Denoise: they replace the decoded picture).
bool isAiVideoEffect(std::string_view effect);
/// AI Refocus.
inline bool isAiRefocusEffect(std::string_view effect) { return effect == kAiRefocusEffect; }
/// AI Face Retouch.
inline bool isAiFaceRetouchEffect(std::string_view effect) { return effect == kAiFaceRetouchEffect; }

/// One stage of a chain, as the models see it (every choice resolved).
struct AiStage {
    std::string effect;  ///< kAiRestoreEffect, kAiDenoiseEffect, kAiRefocusEffect (depth maps), kAiFaceRetouchEffect (face maps) or kAiInterpolateStage
    int scale = 1;       ///< Restore: the picture grows by 1, 2 or 4 (Auto resolved)
    int quality = 0;     ///< Restore: 0 Fast, 1 High (which upscaler)
    int deblur = 0;      ///< Restore: 0..100 (0 = off)
    int denoise = 0;     ///< Restore: light denoise 0..100; Denoise: strength 0..100 (0 = off)
    int grain = 0;       ///< 0..100: how much of the noise the denoise took out is put back
    int tile = 0;        ///< 0 = automatic, else the tile's size in pixels (128, 256, 512)
    bool operator==(const AiStage&) const = default;

    /// The stage changes nothing (Restore at 1x without deblur or denoise, Denoise at strength 0).
    bool isNoop() const;
    /// The picture's size after the stage.
    int outputScale() const { return effect == kAiRestoreEffect ? scale : 1; }
    /// The stage makes depth maps (AI Refocus), face maps (AI Face Retouch) or in-between frames (AI Frame Interpolation): a side
    /// product, the decoded picture is not replaced by it.
    bool isSideProduct() const {
        return effect == kAiRefocusEffect || effect == kAiFaceRetouchEffect || effect == kAiEyeContactEffect || effect == kAiInterpolateStage;
    }
};

/// A clip's AI stages in stack order and their Amount at one time (the product of the enabled stages' Amounts, 0..1).
struct AiChain {
    std::vector<AiStage> stages;
    double amount = 1.0;

    bool empty() const { return stages.empty(); }
    /// How much larger than the media the chain's pictures are.
    int outputScale() const;
    bool operator==(const AiChain&) const = default;
};

/// Restore's Auto scale: 1 when the media is as tall as the sequence (a 4K clip on a 1080p timeline is not upscaled),
/// 2 when 2x reaches it within 10 %, else 4.
int aiAutoScale(int mediaHeight, int sequenceHeight);

/// The stage of one enabled instance of an AI effect (its parameters are constants, so no time); nullopt for any other
/// effect. `mediaHeight` / `sequenceHeight` resolve Auto (0 = unknown: Auto means 2).
std::optional<AiStage> aiStageOf(const EffectInstance& instance, int mediaHeight, int sequenceHeight);

/// The chain of a stack: the enabled AI effects whose stage is not a no-op, in stack order, with Amount at `curveTime`.
/// An Amount of 0 at that time leaves the chain empty (the unprocessed picture, no badge).
AiChain aiChainOf(const EffectStack& stack, Ticks curveTime, int mediaHeight, int sequenceHeight,
                  const EffectRegistry& registry = EffectRegistry::builtin());

/// The chain with its Amount ignored (the cache's identity and the runner's work): all enabled stages whose stage is
/// not a no-op.
AiChain aiProcessingChainOf(const EffectStack& stack, int mediaHeight, int sequenceHeight);

/// 0.7: the chain whose one stage makes the depth maps of the clip's enabled AI Refocus (empty when it has none). Depth maps
/// belong to the picture, not to the effect's settings: every Refocus of a media shares them.
AiChain aiDepthChainOf(const EffectStack& stack);
/// The depth chain itself (one stage), for whoever looks a clip's depth maps up without its effects.
AiChain aiDepthChain();
/// 0.7: the chain whose one stage makes the face maps of the clip's enabled AI Face Retouch (empty when it has none). Face maps
/// belong to the picture, not to the effect's settings: every Face Retouch of a media shares them.
AiChain aiFaceChainOf(const EffectStack& stack);
/// The face chain itself (one stage), for whoever looks a clip's face maps up without its effects.
AiChain aiFaceChain();
/// 0.7: the chain whose one stage makes the eye measurements of the clip's enabled AI Eye Contact (empty when it has none). They belong to
/// the picture, not to the effect's settings: every Eye Contact of a media shares them.
AiChain aiGazeChainOf(const EffectStack& stack);
/// The gaze chain itself (one stage), for whoever looks a clip's eye measurements up without its effects.
AiChain aiGazeChain();
/// The chain of a clip's AI Frame Interpolation (one stage): what the interpolated pictures' cache is named by.
AiChain aiInterpolationChain();

/// A 64-bit hash of everything that changes the processed pixels: the stages, not the Amount. 0 for an empty chain.
std::uint64_t aiChainHash(const AiChain& chain);

/// A short name for people: "Restore 2x, Denoise".
std::string aiChainLabel(const AiChain& chain);

} // namespace wabcut
