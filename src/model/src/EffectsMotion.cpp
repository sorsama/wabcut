// The motion features' effect descriptors (P.2, docs/motion.md): Morph Cut, Warp Stabilizer, Rolling Shutter Repair.

#include "EffectsMotion.h"

#include <wabcut/model/MotionBlur.h>
#include <wabcut/model/MotionOps.h>

#include <array>

namespace wabcut::detail {

namespace {

using C = EffectCapability;
using U = ParamUnit;

// Warp Stabilizer, in the Effect Controls order of Premiere's (Stabilization, Borders, Advanced). The settings are
// constants, except Smoothness, which can be keyframed (0.7: steady one part of a shot and leave another as it was shot);
// the analysis writes the results (auto-scale, crop) and keys the compensating transform per analysed frame
// (transform.* for Position and Position, Scale, Rotation; corner.* for Perspective), in the clip's curve time.
constexpr std::array<EffectParamInfo, 34> kStabilizerParams{{
    {"result", "Result (0 Smooth Motion, 1 No Motion)", U::Amount, 0.0, 0.0, 1.0, false},
    {"smoothness", "Smoothness (%)", U::Amount, 50.0, 0.0, 1000.0, true},
    // 0.7 (card #130): "Gyro (camera metadata)" takes the camera's recorded rotation instead of the picture's motion; the
    // Effect Controls list offers it for footage that has gyro data (or that already uses it).
    {"method", "Method", U::Amount, 3.0, 0.0, 4.0, false,
     "Position|Position, Scale, Rotation|Perspective|Subspace Warp|Gyro (camera metadata)"},
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
    // 0.7 (card #130): the Gyro method's settings (shown only with that Method). The picture's motion measures what the file
    // does not say: the offset between the gyro's clock and the picture's, and the lens's field of view.
    {"gyroSync", "Gyro: Sync", U::Amount, 1.0, 0.0, 1.0, false, "Off|Auto-sync with the picture"},
    {"gyroOffset", "Gyro: Offset (ms)", U::Amount, 0.0, -2000.0, 2000.0, false},
    {"gyroFov", "Gyro: Field of View (degrees; 0 Auto)", U::Amount, 0.0, 0.0, 175.0, false},
    {"gyroReadout", "Gyro: Rolling Shutter Read-out (ms; 0 Auto)", U::Amount, 0.0, 0.0, 200.0, false},
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

// 0.7: Motion Blur (MotionBlur.h). A marker on the clip: the evaluator draws the layer at several instants of the shutter.
constexpr std::array<EffectParamInfo, 4> kMotionBlurParams{{
    {"shutterAngle", "Shutter Angle", U::Degrees, 180.0, 0.0, 720.0},
    {"shutterPhase", "Shutter Phase", U::Degrees, -90.0, -360.0, 360.0},
    {"samples", "Export Samples", U::Amount, 16.0, 2.0, 64.0, false},
    {"previewSamples", "Preview Samples", U::Amount, 8.0, 2.0, 64.0, false},
}};

// Morph Cut (0.7): Face Aware aligns the flow inside the faces both sides show by the faces' landmarks (motion/FaceAlign.h).
constexpr std::array<EffectParamInfo, 1> kMorphCutParams{{
    {"faceAware", "Face Aware", U::Amount, 1.0, 0.0, 1.0, false},
}};

} // namespace

void appendMotionDescriptors(std::vector<EffectDescriptor>& out) {
    const auto video = EffectKind::Video;
    out.push_back({kMotionBlurEffect, 1, "Motion Blur", "Blur & Sharpen", video, false, kMotionBlurParams,
                   C::Neighborhood | C::ExtendsBounds,
                   "Smears the clip's Motion (position, scale, rotation: keyframes, Animations, Zoom Punch, Shake, tracked "
                   "moves, and the speed ramps that make them race) over the time a camera shutter stays open, so fast "
                   "moves blur like a real camera. Shutter Angle is the share of the frame the shutter is open (180 degrees "
                   "= half a frame; 0 = no blur), Shutter Phase where it opens relative to the frame (-90 centres a "
                   "180-degree shutter on the frame), Export Samples and Preview Samples the most pictures it averages in an export and in "
                   "the monitors."});
    out.push_back({kMorphCutTransition, 1, "Morph Cut", "Dissolve", EffectKind::VideoTransition, false, kMorphCutParams, C::TwoInputs,
                   "Smooths a jump cut in a talking-head interview: the outgoing clip's picture at the transition's start "
                   "morphs into the incoming clip's picture at its end along their motion (optical flow, analysed in "
                   "the background) instead of dissolving. Works best on a single subject before a still background. "
                   "Face Aware (on by default) lines the two sides' faces up by their landmarks, so a head that moved, "
                   "turned or leaned between the takes morphs as one face instead of ghosting; it needs the face detector "
                   "of Auto Reframe (0.2 MB, downloaded from Settings > AI > Models) and falls back to the plain "
                   "morph when it is missing or no face is found on both sides."});
    out.push_back({kWarpStabilizerEffect, 1, "Warp Stabilizer", "Distort", video, false, kStabilizerParams,
                   C::Neighborhood | C::ExtendsBounds,
                   "Stabilizes shaky footage: Analyze (effect.analyzeStabilization) tracks the camera motion in the "
                   "background and keys the compensating transform per frame (position, scale and rotation, or a "
                   "corner pin for Perspective), applied to the source picture before the clip's Motion; Framing "
                   "crops, scales or fills the moving borders. Changing a setting re-smooths from the cached analysis. "
                   "Method Gyro (camera metadata) uses the rotation GoPro and Sony cameras record in the file instead "
                   "of the picture's motion."});
    out.push_back({kRollingShutterRepairEffect, 1, "Rolling Shutter Repair", "Distort", video, false, kRollingShutterParams,
                   C::Neighborhood,
                   "Removes the skew and wobble of a rolling-shutter camera (phones, DSLRs, action cameras) during fast "
                   "pans: each line of the picture moves back to where it was when the first line was read, from the "
                   "picture's motion (effect.analyzeRollingShutter) and the scan's share of the frame time."});
}

} // namespace wabcut::detail
