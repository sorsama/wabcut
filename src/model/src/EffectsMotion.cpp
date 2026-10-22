// The motion features' effect descriptors (P.2, docs/motion.md): Morph Cut, Warp Stabilizer, Rolling Shutter Repair.

#include "EffectsMotion.h"

#include <wabcut/model/MotionOps.h>

#include <array>

namespace wabcut::detail {

namespace {

using C = EffectCapability;
using U = ParamUnit;

// Warp Stabilizer, in the Effect Controls order of Premiere's (Stabilization, Borders, Advanced). The settings are
// constants; the analysis writes the results (auto-scale, crop) and keys the compensating transform per analysed frame
// (transform.* for Position and Position, Scale, Rotation; corner.* for Perspective), in the clip's curve time.
constexpr std::array<EffectParamInfo, 30> kStabilizerParams{{
    {"result", "Result (0 Smooth Motion, 1 No Motion)", U::Amount, 0.0, 0.0, 1.0, false},
    {"smoothness", "Smoothness (%)", U::Amount, 50.0, 0.0, 1000.0, false},
    {"method", "Method (0 Position, 1 Position Scale Rotation, 2 Perspective, 3 Subspace Warp)", U::Amount, 3.0, 0.0, 3.0,
     false},
    {"preserveScale", "Preserve Scale", U::Amount, 0.0, 0.0, 1.0, false},
    {"framing", "Framing (0 Stabilize Only, 1 Stabilize Crop, 2 Stabilize Crop Auto-scale, 3 Stabilize Synthesize Edges)",
     U::Amount, 2.0, 0.0, 3.0, false},
    {"maxScale", "Auto-scale: Maximum Scale", U::Factor, 1.5, 1.0, 10.0, false},
    {"actionSafeMargin", "Auto-scale: Action-Safe Margin (%)", U::Amount, 0.0, 0.0, 49.0, false},
    {"additionalScale", "Additional Scale", U::Factor, 1.0, 0.1, 10.0, false},
    {"detailedAnalysis", "Advanced: Detailed Analysis", U::Amount, 0.0, 0.0, 1.0, false},
    {"rollingShutter", "Advanced: Rolling Shutter Ripple (0 Automatic Reduction, 1 Enhanced Reduction)", U::Amount, 0.0,
     0.0, 1.0, false},
    {"cropLessSmoothMore", "Advanced: Crop Less Smooth More (%)", U::Amount, 50.0, 0.0, 100.0, false},
    {"synthesisInputRange", "Advanced: Synthesis Input Range (seconds)", U::Amount, 3.0, 0.0, 30.0, false},
    {"synthesisEdgeFeather", "Advanced: Synthesis Edge Feather", U::Amount, 10.0, 0.0, 100.0, false},
    // Results of the last analysis.
    {"autoScale", "Auto-scale (analysed)", U::Factor, 1.0, 1.0, 10.0, false},
    {"crop.left", "Crop Left (analysed)", U::Fraction, 0.0, 0.0, 0.5, false},
    {"crop.top", "Crop Top (analysed)", U::Fraction, 0.0, 0.0, 0.5, false},
    {"crop.right", "Crop Right (analysed)", U::Fraction, 0.0, 0.0, 0.5, false},
    {"crop.bottom", "Crop Bottom (analysed)", U::Fraction, 0.0, 0.0, 0.5, false},
    {"transform.x", "Compensation X", U::Pixels, 0.0, -100000.0, 100000.0},
    {"transform.y", "Compensation Y", U::Pixels, 0.0, -100000.0, 100000.0},
    {"transform.rotation", "Compensation Rotation", U::Degrees, 0.0, -3600.0, 3600.0},
    {"transform.scale", "Compensation Scale", U::Factor, 1.0, 0.01, 100.0},
    {"corner.tl.x", "Corner Pin Top Left X", U::Pixels, 0.0, -100000.0, 100000.0},
    {"corner.tl.y", "Corner Pin Top Left Y", U::Pixels, 0.0, -100000.0, 100000.0},
    {"corner.tr.x", "Corner Pin Top Right X", U::Pixels, 0.0, -100000.0, 100000.0},
    {"corner.tr.y", "Corner Pin Top Right Y", U::Pixels, 0.0, -100000.0, 100000.0},
    {"corner.br.x", "Corner Pin Bottom Right X", U::Pixels, 0.0, -100000.0, 100000.0},
    {"corner.br.y", "Corner Pin Bottom Right Y", U::Pixels, 0.0, -100000.0, 100000.0},
    {"corner.bl.x", "Corner Pin Bottom Left X", U::Pixels, 0.0, -100000.0, 100000.0},
    {"corner.bl.y", "Corner Pin Bottom Left Y", U::Pixels, 0.0, -100000.0, 100000.0},
}};

// Rolling Shutter Repair: the settings are constants; effect.analyzeRollingShutter keys the picture's motion per
// analysed frame (motion.x, motion.y: source pixels per frame), in the clip's curve time.
constexpr std::array<EffectParamInfo, 7> kRollingShutterParams{{
    {"rate", "Rolling Shutter Rate (%)", U::Amount, 50.0, 0.0, 100.0, false},
    {"scanDirection", "Scan Direction (0 Top to Bottom, 1 Bottom to Top, 2 Left to Right, 3 Right to Left)", U::Amount, 0.0,
     0.0, 3.0, false},
    {"method", "Advanced: Method (0 Warp, 1 Pixel Motion)", U::Amount, 0.0, 0.0, 1.0, false},
    {"detailedAnalysis", "Advanced: Detailed Analysis", U::Amount, 0.0, 0.0, 1.0, false},
    {"pixelMotionDetail", "Advanced: Pixel Motion Detail", U::Amount, 20.0, 0.0, 100.0, false},
    {"motion.x", "Motion X (pixels per frame)", U::Pixels, 0.0, -100000.0, 100000.0},
    {"motion.y", "Motion Y (pixels per frame)", U::Pixels, 0.0, -100000.0, 100000.0},
}};

} // namespace

void appendMotionDescriptors(std::vector<EffectDescriptor>& out) {
    const auto video = EffectKind::Video;
    out.push_back({kMorphCutTransition, 1, "Morph Cut", "Dissolve", EffectKind::VideoTransition, false, {}, C::TwoInputs,
                   "Smooths a jump cut in a talking-head interview: the outgoing clip's picture at the transition's start "
                   "morphs into the incoming clip's picture at its end along their motion (optical flow, analysed in "
                   "the background) instead of dissolving. Works best on a single subject before a still background."});
    out.push_back({kWarpStabilizerEffect, 1, "Warp Stabilizer", "Distort", video, false, kStabilizerParams,
                   C::Neighborhood | C::ExtendsBounds,
                   "Stabilizes shaky footage: Analyze (effect.analyzeStabilization) tracks the camera motion in the "
                   "background and keys the compensating transform per frame (position, scale and rotation, or a "
                   "corner pin for Perspective), applied to the source picture before the clip's Motion; Framing "
                   "crops, scales or fills the moving borders. Changing a setting re-smooths from the cached analysis."});
    out.push_back({kRollingShutterRepairEffect, 1, "Rolling Shutter Repair", "Distort", video, false, kRollingShutterParams,
                   C::Neighborhood,
                   "Removes the skew and wobble of a rolling-shutter camera (phones, DSLRs, action cameras) during fast "
                   "pans: each line of the picture moves back to where it was when the first line was read, from the "
                   "picture's motion (effect.analyzeRollingShutter) and the scan's share of the frame time."});
}

} // namespace wabcut::detail
