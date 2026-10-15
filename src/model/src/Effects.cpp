#include <wabcut/model/Effects.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace wabcut {

namespace {

using C = EffectCapability;
using U = ParamUnit;

constexpr double kMaxPixels = 1e7;

// --- Intrinsic effects: their parameters are ClipProperties (keys from the parameter table). ---------------

constexpr std::array<EffectParamInfo, 6> kMotionParams{{
    {"motion.position.x", "Position X", U::Pixels, 0.0, -kMaxPixels, kMaxPixels},
    {"motion.position.y", "Position Y", U::Pixels, 0.0, -kMaxPixels, kMaxPixels},
    {"motion.scale", "Scale", U::Factor, 1.0, 0.0, 100.0},
    {"motion.rotation", "Rotation", U::Degrees, 0.0, -1e6, 1e6},
    {"motion.anchor.x", "Anchor Point X", U::Pixels, 0.0, -kMaxPixels, kMaxPixels},
    {"motion.anchor.y", "Anchor Point Y", U::Pixels, 0.0, -kMaxPixels, kMaxPixels},
}};
constexpr std::array<EffectParamInfo, 1> kOpacityParams{{{"opacity.opacity", "Opacity", U::Fraction, 1.0, 0.0, 1.0}}};
constexpr std::array<EffectParamInfo, 1> kVolumeParams{{{"volume.level", "Level", U::Decibels, 0.0, -144.0, 24.0}}};
constexpr std::array<EffectParamInfo, 1> kPannerParams{{{"panner.balance", "Balance", U::Balance, 0.0, -1.0, 1.0}}};

// --- Video stack effects (the finishing shelf) ---------------------------------------------------------

constexpr std::array<EffectParamInfo, 5> kCropParams{{
    {"left", "Left", U::Fraction, 0.0, 0.0, 1.0},
    {"top", "Top", U::Fraction, 0.0, 0.0, 1.0},
    {"right", "Right", U::Fraction, 0.0, 0.0, 1.0},
    {"bottom", "Bottom", U::Fraction, 0.0, 0.0, 1.0},
    {"feather", "Edge Feather", U::Pixels, 0.0, 0.0, 1000.0},
}};
constexpr std::array<EffectParamInfo, 2> kBlurParams{{
    {"radius", "Blurriness", U::Pixels, 0.0, 0.0, 1000.0},
    {"repeatEdges", "Repeat Edge Pixels", U::Amount, 1.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 5> kBasicCorrectionParams{{
    {"exposure", "Exposure", U::Stops, 0.0, -5.0, 5.0},
    {"contrast", "Contrast", U::Amount, 0.0, -100.0, 100.0},
    {"saturation", "Saturation", U::Amount, 100.0, 0.0, 200.0},
    {"temperature", "Temperature", U::Amount, 0.0, -100.0, 100.0},
    {"tint", "Tint", U::Amount, 0.0, -100.0, 100.0},
}};
constexpr std::array<EffectParamInfo, 4> kVignetteParams{{
    {"amount", "Amount", U::Amount, 0.0, -5.0, 5.0},
    {"midpoint", "Midpoint", U::Amount, 50.0, 0.0, 100.0},
    {"roundness", "Roundness", U::Amount, 0.0, -100.0, 100.0},
    {"feather", "Feather", U::Amount, 50.0, 0.0, 100.0},
}};
constexpr std::array<EffectParamInfo, 2> kSharpenParams{{
    {"amount", "Amount", U::Amount, 0.0, 0.0, 500.0},
    {"radius", "Radius", U::Pixels, 1.0, 0.1, 10.0},
}};
constexpr std::array<EffectParamInfo, 7> kDropShadowParams{{
    {"opacity", "Opacity", U::Fraction, 0.5, 0.0, 1.0},
    {"angle", "Direction", U::Degrees, 135.0, -3600.0, 3600.0},
    {"distance", "Distance", U::Pixels, 10.0, 0.0, 4000.0},
    {"softness", "Softness", U::Pixels, 10.0, 0.0, 1000.0},
    {"red", "Color Red", U::Fraction, 0.0, 0.0, 1.0},
    {"green", "Color Green", U::Fraction, 0.0, 0.0, 1.0},
    {"blue", "Color Blue", U::Fraction, 0.0, 0.0, 1.0},
}};

// Title styling (TitleOps.h): the numeric part of a title, keyframable; text and font are on the title's
// generator media item. Colours are straight (not premultiplied) RGBA in the working space.
constexpr std::array<EffectParamInfo, 20> kTitleStyleParams{{
    {"size", "Font Size", U::Pixels, 100.0, 1.0, 2000.0},
    {"fill.red", "Fill Red", U::Fraction, 1.0, 0.0, 1.0},
    {"fill.green", "Fill Green", U::Fraction, 1.0, 0.0, 1.0},
    {"fill.blue", "Fill Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"fill.alpha", "Fill Opacity", U::Fraction, 1.0, 0.0, 1.0},
    {"stroke.width", "Stroke Width", U::Pixels, 0.0, 0.0, 500.0},
    {"stroke.red", "Stroke Red", U::Fraction, 0.0, 0.0, 1.0},
    {"stroke.green", "Stroke Green", U::Fraction, 0.0, 0.0, 1.0},
    {"stroke.blue", "Stroke Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"stroke.alpha", "Stroke Opacity", U::Fraction, 1.0, 0.0, 1.0},
    {"shadow.opacity", "Shadow Opacity", U::Fraction, 0.0, 0.0, 1.0},
    {"shadow.angle", "Shadow Angle", U::Degrees, 135.0, -3600.0, 3600.0},
    {"shadow.distance", "Shadow Distance", U::Pixels, 10.0, 0.0, 4000.0},
    {"shadow.softness", "Shadow Softness", U::Pixels, 10.0, 0.0, 1000.0},
    {"shadow.red", "Shadow Red", U::Fraction, 0.0, 0.0, 1.0}, // black by default, as before C.6
    {"shadow.green", "Shadow Green", U::Fraction, 0.0, 0.0, 1.0},
    {"shadow.blue", "Shadow Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"alignment", "Alignment", U::Amount, 1.0, 0.0, 2.0, false}, // 0 left, 1 centre, 2 right
    {"lineSpacing", "Line Spacing", U::Factor, 1.0, 0.1, 10.0},  // 1 = the font's line height
    {"tracking", "Tracking", U::Amount, 0.0, -1000.0, 10000.0},  // thousandths of an em, like Premiere
}};

// Colour grade (I.5, docs/color.md): one stack of correction stages. Stage switches (0/1, constants), then the scalar
// parameters of each stage in the order the panel shows them. Its curves and LUT references are EffectData, not
// parameters (not keyframable):
//   "basic.inputLut", "creative.look"   text: "asset:<id>" (a ProjectAsset of kind "lut") or "builtin:<name>"
//   "curves.master", "curves.red", "curves.green", "curves.blue"
//                                       points, x and y in [0, 1] (display-referred code values); none = identity
//   "curves.hueSat", "curves.hueHue", "curves.hueLuma"
//                                       points, x = hue in turns [0, 1) (periodic), y in [0, 1], 0.5 = no change
//   "curves.lumaSat", "curves.satSat"   points, x in [0, 1] (luma / saturation), y in [0, 1], 0.5 = no change
constexpr std::array<EffectParamInfo, 62> kGradeParams{{
    {"basic.enabled", "Basic Correction", U::Amount, 1.0, 0.0, 1.0, false},
    {"creative.enabled", "Creative", U::Amount, 1.0, 0.0, 1.0, false},
    {"curves.enabled", "Curves", U::Amount, 1.0, 0.0, 1.0, false},
    {"wheels.enabled", "Color Wheels", U::Amount, 1.0, 0.0, 1.0, false},
    {"hsl.enabled", "HSL Secondary", U::Amount, 1.0, 0.0, 1.0, false},
    {"vignette.enabled", "Vignette", U::Amount, 1.0, 0.0, 1.0, false},
    {"basic.temperature", "Temperature", U::Amount, 0.0, -100.0, 100.0},
    {"basic.tint", "Tint", U::Amount, 0.0, -100.0, 100.0},
    {"basic.exposure", "Exposure", U::Stops, 0.0, -5.0, 5.0},
    {"basic.contrast", "Contrast", U::Amount, 0.0, -100.0, 100.0},
    {"basic.highlights", "Highlights", U::Amount, 0.0, -100.0, 100.0},
    {"basic.shadows", "Shadows", U::Amount, 0.0, -100.0, 100.0},
    {"basic.whites", "Whites", U::Amount, 0.0, -100.0, 100.0},
    {"basic.blacks", "Blacks", U::Amount, 0.0, -100.0, 100.0},
    {"basic.saturation", "Saturation", U::Amount, 100.0, 0.0, 200.0},
    {"basic.vibrance", "Vibrance", U::Amount, 0.0, -100.0, 100.0},
    {"creative.lookIntensity", "Look Intensity", U::Amount, 100.0, 0.0, 200.0},
    {"creative.fadedFilm", "Faded Film", U::Amount, 0.0, 0.0, 100.0},
    {"creative.vibrance", "Creative Vibrance", U::Amount, 0.0, -100.0, 100.0},
    {"creative.saturation", "Creative Saturation", U::Amount, 100.0, 0.0, 200.0},
    {"creative.shadowTint.hue", "Shadow Tint Hue", U::Degrees, 0.0, 0.0, 360.0},
    {"creative.shadowTint.amount", "Shadow Tint", U::Amount, 0.0, 0.0, 100.0},
    {"creative.highlightTint.hue", "Highlight Tint Hue", U::Degrees, 0.0, 0.0, 360.0},
    {"creative.highlightTint.amount", "Highlight Tint", U::Amount, 0.0, 0.0, 100.0},
    {"creative.tintBalance", "Tint Balance", U::Amount, 0.0, -100.0, 100.0},
    // Wheels = ASC CDL: slope = gain - lift, offset = lift + offset, power = gamma.
    {"wheels.lift.red", "Lift Red", U::Amount, 0.0, -1.0, 1.0},
    {"wheels.lift.green", "Lift Green", U::Amount, 0.0, -1.0, 1.0},
    {"wheels.lift.blue", "Lift Blue", U::Amount, 0.0, -1.0, 1.0},
    {"wheels.gamma.red", "Gamma Red", U::Factor, 1.0, 0.1, 10.0},
    {"wheels.gamma.green", "Gamma Green", U::Factor, 1.0, 0.1, 10.0},
    {"wheels.gamma.blue", "Gamma Blue", U::Factor, 1.0, 0.1, 10.0},
    {"wheels.gain.red", "Gain Red", U::Factor, 1.0, 0.0, 8.0},
    {"wheels.gain.green", "Gain Green", U::Factor, 1.0, 0.0, 8.0},
    {"wheels.gain.blue", "Gain Blue", U::Factor, 1.0, 0.0, 8.0},
    {"wheels.offset.red", "Offset Red", U::Amount, 0.0, -2.0, 2.0},
    {"wheels.offset.green", "Offset Green", U::Amount, 0.0, -2.0, 2.0},
    {"wheels.offset.blue", "Offset Blue", U::Amount, 0.0, -2.0, 2.0},
    {"wheels.saturation", "CDL Saturation", U::Factor, 1.0, 0.0, 4.0},
    {"hsl.hue.center", "Hue Center", U::Degrees, 0.0, 0.0, 360.0},
    {"hsl.hue.width", "Hue Range", U::Degrees, 360.0, 0.0, 360.0},
    {"hsl.hue.softness", "Hue Softness", U::Degrees, 0.0, 0.0, 180.0},
    {"hsl.sat.low", "Saturation Low", U::Amount, 0.0, 0.0, 100.0},
    {"hsl.sat.high", "Saturation High", U::Amount, 100.0, 0.0, 100.0},
    {"hsl.sat.softness", "Saturation Softness", U::Amount, 0.0, 0.0, 100.0},
    {"hsl.luma.low", "Luma Low", U::Amount, 0.0, 0.0, 100.0},
    {"hsl.luma.high", "Luma High", U::Amount, 100.0, 0.0, 100.0},
    {"hsl.luma.softness", "Luma Softness", U::Amount, 0.0, 0.0, 100.0},
    {"hsl.invert", "Invert Mask", U::Amount, 0.0, 0.0, 1.0, false},
    {"hsl.denoise", "Denoise", U::Amount, 0.0, 0.0, 100.0},
    {"hsl.blur", "Blur", U::Pixels, 0.0, 0.0, 100.0},
    {"hsl.temperature", "Secondary Temperature", U::Amount, 0.0, -100.0, 100.0},
    {"hsl.tint", "Secondary Tint", U::Amount, 0.0, -100.0, 100.0},
    {"hsl.contrast", "Secondary Contrast", U::Amount, 0.0, -100.0, 100.0},
    {"hsl.sharpen", "Secondary Sharpen", U::Amount, 0.0, 0.0, 100.0},
    {"hsl.saturation", "Secondary Saturation", U::Amount, 100.0, 0.0, 200.0},
    {"hsl.offset.red", "Secondary Offset Red", U::Amount, 0.0, -1.0, 1.0},
    {"hsl.offset.green", "Secondary Offset Green", U::Amount, 0.0, -1.0, 1.0},
    {"hsl.offset.blue", "Secondary Offset Blue", U::Amount, 0.0, -1.0, 1.0},
    {"vignette.amount", "Vignette Amount", U::Amount, 0.0, -5.0, 5.0},
    {"vignette.midpoint", "Vignette Midpoint", U::Amount, 50.0, 0.0, 100.0},
    {"vignette.roundness", "Vignette Roundness", U::Amount, 0.0, -100.0, 100.0},
    {"vignette.feather", "Vignette Feather", U::Amount, 50.0, 0.0, 100.0},
}};

// --- Masks (I.6) ------------------------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 0> kMaskEffectParams{};
constexpr std::array<EffectParamInfo, 1> kMosaicParams{{{"cellSize", "Cell Size", U::Fraction, 0.02, 0.001, 0.5}}};

// --- Keying (C.15; the math in docs/internal/c15-parity-handoff.md) -------------------------------------

constexpr std::array<EffectParamInfo, 3> kTrackMatteParams{{
    {"matteTrack", "Matte (video track number; 0 = none)", U::Amount, 0.0, 0.0, 999.0, false},
    {"compositeUsing", "Composite Using (0 Matte Alpha, 1 Matte Luma)", U::Amount, 0.0, 0.0, 1.0, false},
    {"reverse", "Reverse", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 20> kUltraKeyParams{{
    {"output", "Output (0 Composite, 1 Alpha Channel, 2 Color Channel)", U::Amount, 0.0, 0.0, 2.0, false},
    {"keyColor.r", "Key Color Red", U::Fraction, 0.0, 0.0, 1.0},
    {"keyColor.g", "Key Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"keyColor.b", "Key Color Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"transparency", "Matte Generation: Transparency", U::Amount, 45.0, 0.0, 100.0},
    {"highlight", "Matte Generation: Highlight", U::Amount, 10.0, 0.0, 100.0},
    {"shadow", "Matte Generation: Shadow", U::Amount, 50.0, 0.0, 100.0},
    {"tolerance", "Matte Generation: Tolerance", U::Amount, 50.0, 0.0, 100.0},
    {"pedestal", "Matte Generation: Pedestal", U::Amount, 10.0, 0.0, 100.0},
    {"choke", "Matte Cleanup: Choke", U::Amount, 0.0, 0.0, 100.0},
    {"soften", "Matte Cleanup: Soften", U::Amount, 0.0, 0.0, 100.0},
    {"contrast", "Matte Cleanup: Contrast", U::Amount, 0.0, 0.0, 100.0},
    {"midPoint", "Matte Cleanup: Mid Point", U::Amount, 50.0, 0.0, 100.0},
    {"desaturate", "Spill Suppression: Desaturate", U::Amount, 25.0, 0.0, 100.0},
    {"range", "Spill Suppression: Range", U::Amount, 50.0, 0.0, 100.0},
    {"spill", "Spill Suppression: Spill", U::Amount, 50.0, 0.0, 100.0},
    {"luma", "Spill Suppression: Luma", U::Amount, 50.0, 0.0, 100.0},
    {"saturation", "Color Correction: Saturation", U::Amount, 100.0, 0.0, 200.0},
    {"hue", "Color Correction: Hue", U::Degrees, 0.0, -180.0, 180.0},
    {"luminance", "Color Correction: Luminance", U::Amount, 100.0, 0.0, 200.0},
}};

constexpr std::array<EffectParamInfo, 5> kBorderParams{{
    {"width", "Width", U::Pixels, 4.0, 0.0, 500.0},
    {"color.r", "Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"color.g", "Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"color.b", "Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"opacity", "Opacity", U::Fraction, 1.0, 0.0, 1.0},
}};

// --- Audio stack effects -----------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 1> kGainParams{{{"gain", "Gain", U::Decibels, 0.0, -96.0, 24.0}}};
constexpr std::array<EffectParamInfo, 7> kEqParams{{
    {"lowFrequency", "Low Frequency", U::Hertz, 100.0, 20.0, 1000.0},
    {"lowGain", "Low Gain", U::Decibels, 0.0, -24.0, 24.0},
    {"midFrequency", "Mid Frequency", U::Hertz, 1000.0, 100.0, 10000.0},
    {"midGain", "Mid Gain", U::Decibels, 0.0, -24.0, 24.0},
    {"midQ", "Mid Q", U::Ratio, 1.0, 0.1, 10.0},
    {"highFrequency", "High Frequency", U::Hertz, 8000.0, 1000.0, 20000.0},
    {"highGain", "High Gain", U::Decibels, 0.0, -24.0, 24.0},
}};
constexpr std::array<EffectParamInfo, 5> kCompressorParams{{
    {"threshold", "Threshold", U::Decibels, -20.0, -60.0, 0.0},
    {"ratio", "Ratio", U::Ratio, 4.0, 1.0, 20.0},
    {"attack", "Attack", U::Milliseconds, 10.0, 0.1, 200.0},
    {"release", "Release", U::Milliseconds, 100.0, 1.0, 2000.0},
    {"makeup", "Makeup Gain", U::Decibels, 0.0, 0.0, 24.0},
}};
constexpr std::array<EffectParamInfo, 1> kDenoiseParams{{{"amount", "Amount", U::Fraction, 1.0, 0.0, 1.0}}};
constexpr std::array<EffectParamInfo, 1> kEnhanceSpeechParams{{{"strength", "Strength", U::Fraction, 1.0, 0.0, 1.0}}};
constexpr std::array<EffectParamInfo, 3> kLimiterParams{{
    {"ceiling", "Ceiling", U::Decibels, -1.0, -24.0, 0.0},
    {"release", "Release", U::Milliseconds, 50.0, 1.0, 1000.0},
    {"lookahead", "Lookahead", U::Milliseconds, 5.0, 0.0, 20.0, false},
}};
// Essential Sound (A.7): repair, clarity and space effects the presets build stacks from.
constexpr std::array<EffectParamInfo, 3> kFilterParams{{
    {"lowCut", "Low Cut", U::Hertz, 20.0, 20.0, 2000.0},
    {"highCut", "High Cut", U::Hertz, 20000.0, 200.0, 20000.0},
    {"slope", "Slope (dB/octave)", U::Amount, 24.0, 12.0, 48.0, false},
}};
constexpr std::array<EffectParamInfo, 3> kDeHumParams{{
    {"frequency", "Frequency", U::Hertz, 50.0, 40.0, 70.0, false},
    {"harmonics", "Harmonics", U::Amount, 4.0, 1.0, 8.0, false},
    {"reduction", "Reduction", U::Decibels, 30.0, 0.0, 60.0},
}};
constexpr std::array<EffectParamInfo, 3> kDeEssParams{{
    {"threshold", "Threshold", U::Decibels, -30.0, -60.0, 0.0},
    {"reduction", "Max Reduction", U::Decibels, 10.0, 0.0, 24.0},
    {"frequency", "Frequency", U::Hertz, 5500.0, 2000.0, 12000.0},
}};
constexpr std::array<EffectParamInfo, 5> kExpanderParams{{
    {"threshold", "Threshold", U::Decibels, -45.0, -80.0, 0.0},
    {"ratio", "Ratio", U::Ratio, 2.0, 1.0, 10.0},
    {"range", "Range", U::Decibels, 12.0, 0.0, 60.0},
    {"attack", "Attack", U::Milliseconds, 2.0, 0.1, 100.0},
    {"release", "Release", U::Milliseconds, 150.0, 5.0, 2000.0},
}};
constexpr std::array<EffectParamInfo, 6> kReverbParams{{
    {"size", "Room Size", U::Fraction, 0.5, 0.0, 1.0, false},
    {"predelay", "Pre-delay", U::Milliseconds, 10.0, 0.0, 200.0, false},
    {"decay", "Decay (RT60)", U::Milliseconds, 1500.0, 100.0, 20000.0},
    {"damping", "Damping", U::Fraction, 0.5, 0.0, 1.0},
    {"mix", "Mix", U::Fraction, 0.25, 0.0, 1.0},
    {"width", "Width", U::Fraction, 1.0, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 2> kWidthParams{{
    {"width", "Width", U::Factor, 1.0, 0.0, 2.0},
    {"bassMono", "Mono Bass Below", U::Hertz, 0.0, 0.0, 500.0},
}};
constexpr std::array<EffectParamInfo, 0> kEssentialSoundParams{};

// --- Transitions ---------------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 2> kWipeParams{{
    {"angle", "Angle", U::Degrees, 0.0, 0.0, 360.0, false},
    {"feather", "Border Feather", U::Fraction, 0.0, 0.0, 1.0, false},
}};

std::vector<EffectDescriptor> builtinDescriptors() {
    const auto video = EffectKind::Video;
    const auto audio = EffectKind::Audio;
    return {
        // Intrinsic (every clip): parameters in ClipProperties.
        {"wabcut.motion", 1, "Transform", "Built-in", video, true, kMotionParams, C::PixelLocal | C::UsesAlpha,
         "Position, scale, rotation and anchor point of the clip (Premiere's Motion)."},
        {"wabcut.opacity", 1, "Opacity", "Built-in", video, true, kOpacityParams, C::PixelLocal | C::UsesAlpha,
         "Clip opacity. Fades are keyframes on it."},
        {"wabcut.volume", 1, "Volume", "Built-in", audio, true, kVolumeParams, C::None,
         "Clip level in dB. Audio fades are keyframes on it (or audio transitions)."},
        {"wabcut.panner", 1, "Panner", "Built-in", audio, true, kPannerParams, C::None, "Stereo balance."},
        // Video stack.
        {"wabcut.color.basic", 1, "Basic Correction", "Color", video, false, kBasicCorrectionParams,
         C::PixelLocal | C::LinearLight,
         "SDR correction: exposure (stops), contrast, saturation (100 = unchanged), temperature and tint."},
        {"wabcut.vignette", 1, "Vignette", "Color", video, false, kVignetteParams, C::PixelLocal,
         "Darkens (negative amount) or lightens the edges of the frame."},
        {"wabcut.color.grade", 1, "Color Grade", "Color", video, false, kGradeParams,
         C::Neighborhood | C::LinearLight,
         "A full grade in one effect, in a fixed order: Basic (input LUT, white balance, exposure, contrast, highlights, "
         "shadows, whites, blacks, saturation, vibrance), Creative (look LUT, faded film, tints, vibrance), Curves (RGB "
         "and hue curves), Color Wheels (lift, gamma, gain, offset as an ASC CDL), HSL Secondary (a qualifier with its "
         "own correction) and Vignette. Curves and LUTs are effect data (docs/color.md)."},
        {"wabcut.blur.gaussian", 1, "Gaussian Blur", "Blur & Sharpen", video, false, kBlurParams,
         C::Neighborhood | C::ExtendsBounds | C::UsesAlpha, "Gaussian blur of the given radius in pixels."},
        {"wabcut.sharpen", 1, "Sharpen", "Blur & Sharpen", video, false, kSharpenParams, C::Neighborhood,
         "Unsharp-mask sharpening."},
        {"wabcut.crop", 1, "Crop", "Transform", video, false, kCropParams, C::PixelLocal | C::UsesAlpha,
         "Crops each edge by a fraction of the frame, with an optional feathered edge."},
        {"wabcut.dropShadow", 1, "Drop Shadow", "Perspective", video, false, kDropShadowParams,
         C::Neighborhood | C::ExtendsBounds | C::UsesAlpha, "A soft shadow of the layer's alpha (titles, graphics)."},
        {"wabcut.title.style", 1, "Title Style", "Graphics", video, false, kTitleStyleParams,
         C::ExtendsBounds | C::UsesAlpha,
         "How a title clip draws its text: font size, fill and stroke colour, stroke width, shadow and its "
         "colour, alignment (0 left, 1 centre, 2 right), line spacing and tracking. Text and font are on the "
         "title's media item; position, scale and rotation are the clip's Transform. Only title clips use it."},
        {"wabcut.mask", 1, "Mask", "Masks", video, false, kMaskEffectParams, C::PixelLocal | C::UsesAlpha,
         "Opacity masks: the clip shows only where its masks cover it (cut-outs). The shapes are the instance's masks "
         "(docs/masks.md)."},
        {"wabcut.mosaic", 1, "Mosaic", "Stylize", video, false, kMosaicParams, C::Neighborhood,
         "Pixelates the picture into cells of the given size (a fraction of the frame height)."},
        // C.15: keying.
        {std::string_view(kTrackMatteEffect), 1, "Track Matte Key", "Keying", video, false, kTrackMatteParams,
         C::PixelLocal | C::UsesAlpha,
         "Shows the clip through a matte: the picture of another video track (its alpha or its luma), optionally "
         "reversed. The matte track's clips are read whatever the track's output switch (turn the track off to hide the "
         "matte itself). CompositePlan resolves the matte per frame."},
        {std::string_view(kBorderEffect), 1, "Border", "Stylize", video, false, kBorderParams,
         C::Neighborhood | C::ExtendsBounds | C::UsesAlpha,
         "A border around the layer's visible shape: a stroke of the given width in sequence pixels outside its alpha "
         "edge (after its crop and masks: rounded corners and circles get a rounded border). Facecam layouts use it."},
        {std::string_view(kUltraKeyEffect), 1, "Ultra Key", "Keying", video, false, kUltraKeyParams,
         C::PixelLocal | C::Neighborhood | C::UsesAlpha,
         "Chroma key (Premiere's Ultra Key): a matte from the distance to the key colour in YCbCr, with matte cleanup "
         "(choke, soften, contrast), spill suppression and colour correction of the foreground. Output the composite, "
         "the alpha channel or the colour channel."},
        // Audio stack.
        {"wabcut.audio.gain", 1, "Gain", "Amplitude", audio, false, kGainParams, C::None, "Gain in dB."},
        {"wabcut.audio.eq3", 1, "Parametric EQ (3-band)", "Filter and EQ", audio, false, kEqParams, C::AudioStateful,
         "Low shelf, peaking mid band and high shelf."},
        {"wabcut.audio.compressor", 1, "Compressor", "Dynamics", audio, false, kCompressorParams, C::AudioStateful,
         "Downward compressor with makeup gain."},
        {"wabcut.audio.limiter", 1, "Limiter", "Dynamics", audio, false, kLimiterParams,
         C::AudioStateful | C::AudioLookahead, "Brick-wall limiter with lookahead."},
        {"wabcut.audio.denoise", 1, "Reduce Noise", "Noise Reduction", audio, false, kDenoiseParams,
         C::AudioStateful | C::AudioLookahead,
         "Speech noise reduction (RNNoise). Amount blends the cleaned signal with the original."},
        {"wabcut.audio.enhanceSpeech", 1, "Enhance Speech", "Noise Reduction", audio, false, kEnhanceSpeechParams, C::None,
         "Studio-quality dialogue (DeepFilterNet3), rendered offline by audio.enhanceSpeech: the clip plays its media's "
         "enhanced take, blended with the original by Strength, before the clip's level and redactions; the original "
         "until the take is rendered."},
        // Essential Sound (A.7, docs/ai.md "Essential Sound").
        {"wabcut.audio.filter", 1, "Filter (Low/High Cut)", "Filter and EQ", audio, false, kFilterParams, C::AudioStateful,
         "Butterworth high-pass (Low Cut: rumble, wind, handling noise) and low-pass (High Cut) at 12 to 48 dB per octave. "
         "Off at the ends of their ranges (20 Hz, 20 kHz)."},
        {"wabcut.audio.dehum", 1, "DeHum", "Noise Reduction", audio, false, kDeHumParams, C::AudioStateful,
         "Removes mains hum: narrow cuts of Reduction at the hum frequency (50 Hz in most of the world, 60 Hz in the "
         "Americas) and its harmonics."},
        {"wabcut.audio.deess", 1, "DeEsser", "Dynamics", audio, false, kDeEssParams, C::AudioStateful,
         "Turns harsh \"s\" sounds down: the band above Frequency goes down by 3/4 of its level over Threshold, at most Max "
         "Reduction; below the threshold the sound is untouched."},
        {"wabcut.audio.expander", 1, "Expander", "Dynamics", audio, false, kExpanderParams, C::AudioStateful,
         "Downward expander: quiet passages below Threshold go further down (each dB below becomes Ratio dB, at most "
         "Range), which reduces a room's reverb tail and the noise between words."},
        {"wabcut.audio.reverb", 1, "Reverb", "Reverb", audio, false, kReverbParams, C::AudioStateful,
         "Algorithmic room reverb (a feedback delay network): Room Size and Pre-delay set the room, Decay its RT60, "
         "Damping how much faster the highs die away, Mix the wet share and Width the stereo spread of the tail."},
        {"wabcut.audio.width", 1, "Stereo Width", "Stereo Imagery", audio, false, kWidthParams, C::AudioStateful,
         "Mid/side width: 0 = mono, 1 = unchanged, 2 = twice as wide; the bass below Mono Bass Below stays centred. The "
         "mono fold-down is unchanged at any width."},
        {"wabcut.audio.essentialSound", 1, "Essential Sound", "Essential Sound", audio, false, kEssentialSoundParams,
         C::None,
         "The Essential Sound panel's record of the clip: its type (data \"type\": dialogue, music, sfx or ambience), the "
         "preset and options applied (\"preset\", \"options\"). It does nothing to the sound; the effects the preset "
         "built follow it in the stack, each marked with its panel section (data \"essentialSound\")."},
        // Transitions (constant parameters, Transition::params).
        {"crossDissolve", 1, "Cross Dissolve", "Dissolve", EffectKind::VideoTransition, false, {}, C::TwoInputs,
         "Blends the outgoing clip into the incoming one."},
        {"dipToBlack", 1, "Dip to Black", "Dissolve", EffectKind::VideoTransition, false, {}, C::TwoInputs,
         "Fades the outgoing clip to black, then the incoming clip up from black."},
        {"dipToWhite", 1, "Dip to White", "Dissolve", EffectKind::VideoTransition, false, {}, C::TwoInputs,
         "Fades through white."},
        {"wipe", 1, "Wipe", "Wipe", EffectKind::VideoTransition, false, kWipeParams, C::TwoInputs | C::UsesAlpha,
         "A straight wipe at an angle (0 = left to right), optionally feathered. Placeholder for the wipe family."},
        {"constantPower", 1, "Constant Power", "Crossfade", EffectKind::AudioTransition, false, {}, C::TwoInputs,
         "Equal-power crossfade (Premiere's default audio transition)."},
        {"constantGain", 1, "Constant Gain", "Crossfade", EffectKind::AudioTransition, false, {}, C::TwoInputs,
         "Linear-gain crossfade."},
    };
}

constexpr std::array<std::pair<EffectCapability, std::string_view>, 8> kCapabilityNames{{
    {C::PixelLocal, "pixelLocal"},
    {C::Neighborhood, "neighborhood"},
    {C::ExtendsBounds, "extendsBounds"},
    {C::UsesAlpha, "usesAlpha"},
    {C::LinearLight, "linearLight"},
    {C::TwoInputs, "twoInputs"},
    {C::AudioStateful, "audioStateful"},
    {C::AudioLookahead, "audioLookahead"},
}};

} // namespace

std::string_view toString(EffectKind kind) {
    switch (kind) {
    case EffectKind::Video:
        return "video";
    case EffectKind::Audio:
        return "audio";
    case EffectKind::VideoTransition:
        return "videoTransition";
    case EffectKind::AudioTransition:
        return "audioTransition";
    }
    return "video";
}

std::vector<std::string_view> capabilityNames(EffectCapabilities capabilities) {
    std::vector<std::string_view> out;
    for (const auto& [flag, name] : kCapabilityNames) {
        if (capabilities.has(flag)) {
            out.push_back(name);
        }
    }
    return out;
}

const ParamValue* EffectInstance::find(std::string_view key) const {
    const auto it = std::lower_bound(params.begin(), params.end(), key,
                                     [](const EffectParam& p, std::string_view k) { return p.key < k; });
    return it != params.end() && it->key == key ? &it->value : nullptr;
}

void EffectInstance::set(std::string key, ParamValue value) {
    const auto it = std::lower_bound(params.begin(), params.end(), key,
                                     [](const EffectParam& p, const std::string& k) { return p.key < k; });
    if (it != params.end() && it->key == key) {
        it->value = std::move(value);
    } else {
        params.insert(it, EffectParam{std::move(key), std::move(value)});
    }
}

std::optional<std::string> effectDataProblem(const EffectData& entry) {
    if (entry.key.empty()) {
        return "a data entry has no key";
    }
    if (entry.key.size() > kMaxEffectText) {
        return "a data key is longer than " + std::to_string(kMaxEffectText) + " bytes";
    }
    if (const std::vector<EffectPoint>* points = entry.points()) {
        if (points->size() > kMaxEffectPoints) {
            return "data " + entry.key + " has more than " + std::to_string(kMaxEffectPoints) + " points";
        }
        for (std::size_t i = 0; i < points->size(); ++i) {
            const EffectPoint& p = (*points)[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                return "data " + entry.key + " has a point that is not finite";
            }
            if (i > 0 && !((*points)[i - 1].x < p.x)) {
                return "data " + entry.key + " has points that are not strictly increasing in x";
            }
        }
    } else if (entry.text()->size() > kMaxEffectText) {
        return "data " + entry.key + " is longer than " + std::to_string(kMaxEffectText) + " bytes";
    }
    return std::nullopt;
}

const EffectData* EffectInstance::findData(std::string_view key) const {
    const auto it = std::lower_bound(data.begin(), data.end(), key,
                                     [](const EffectData& d, std::string_view k) { return d.key < k; });
    return it != data.end() && it->key == key ? &*it : nullptr;
}

void EffectInstance::setData(EffectData entry) {
    const auto it = std::lower_bound(data.begin(), data.end(), entry.key,
                                     [](const EffectData& d, const std::string& k) { return d.key < k; });
    const bool remove = entry.points() ? entry.points()->empty() : entry.text()->empty();
    if (it != data.end() && it->key == entry.key) {
        if (remove) {
            data.erase(it);
        } else {
            it->value = std::move(entry.value);
        }
    } else if (!remove) {
        data.insert(it, std::move(entry));
    }
}

const EffectParamInfo* EffectDescriptor::param(std::string_view key) const {
    for (const EffectParamInfo& p : params) {
        if (p.key == key) {
            return &p;
        }
    }
    return nullptr;
}

const EffectRegistry& EffectRegistry::builtin() {
    static const EffectRegistry registry(builtinDescriptors());
    return registry;
}

namespace {

struct ProviderSlot {
    std::mutex mutex; // installs only (rare); find() reads the atomic
    std::vector<std::shared_ptr<const EffectDescriptorProvider>> installed;
    std::atomic<const EffectDescriptorProvider*> current{nullptr};
};

ProviderSlot& providerSlot() {
    static ProviderSlot slot;
    return slot;
}

} // namespace

void setEffectDescriptorProvider(std::shared_ptr<const EffectDescriptorProvider> provider) {
    ProviderSlot& slot = providerSlot();
    std::lock_guard lock(slot.mutex);
    slot.current.store(provider.get(), std::memory_order_release);
    if (provider) {
        slot.installed.push_back(std::move(provider)); // kept: descriptors it returned stay valid
    }
}

const EffectDescriptor* EffectRegistry::find(std::string_view id) const {
    for (const EffectDescriptor& d : m_descriptors) {
        if (d.id == id) {
            return &d;
        }
    }
    if (id.starts_with(kPluginEffectPrefix) && this == &builtin()) {
        if (const EffectDescriptorProvider* provider = providerSlot().current.load(std::memory_order_acquire)) {
            return provider->find(id);
        }
    }
    return nullptr;
}

bool isEvaluable(const EffectInstance& instance, EffectKind kind, const EffectRegistry& registry) {
    const EffectDescriptor* d = registry.find(instance.effect);
    return d && !d->intrinsic && d->kind == kind && instance.version <= d->version;
}

double effectParameterAt(const EffectInstance& instance, std::string_view key, Ticks curveTime, double fallback,
                         const EffectRegistry& registry) {
    if (const ParamValue* v = instance.find(key)) {
        return v->valueAt(curveTime);
    }
    if (const EffectDescriptor* d = registry.find(instance.effect)) {
        if (const EffectParamInfo* p = d->param(key)) {
            return p->defaultValue;
        }
    }
    return fallback;
}

std::optional<std::string> effectStackProblem(const EffectStack& stack, EffectKind kind, std::uint64_t idLimit,
                                              const EffectRegistry& registry) {
    std::set<std::uint64_t> ids;
    for (const EffectInstance& e : stack) {
        const std::string what = "effect " + std::to_string(e.id.value) + " ('" + e.effect + "')";
        if (!e.id.isValid() || e.id.value >= idLimit) {
            return what + " has an id that is not below the id generator";
        }
        if (!ids.insert(e.id.value).second) {
            return what + " appears twice in one stack";
        }
        if (e.effect.empty()) {
            return what + " has no effect id";
        }
        for (std::size_t i = 0; i < e.params.size(); ++i) {
            if (i > 0 && !(e.params[i - 1].key < e.params[i].key)) {
                return what + " has unsorted or duplicate parameter keys";
            }
        }
        if (e.data.size() > kMaxEffectDataEntries) {
            return what + " has more than " + std::to_string(kMaxEffectDataEntries) + " data entries";
        }
        for (std::size_t i = 0; i < e.data.size(); ++i) {
            if (i > 0 && !(e.data[i - 1].key < e.data[i].key)) {
                return what + " has unsorted or duplicate data keys";
            }
            if (const auto problem = effectDataProblem(e.data[i])) {
                return what + ": " + *problem;
            }
        }
        if (const auto problem = masksProblem(e.masks)) {
            return what + ": " + *problem;
        }
        const EffectDescriptor* d = registry.find(e.effect);
        if (d && d->intrinsic) {
            return what + " is a built-in clip effect and cannot be in a stack";
        }
        const bool known = d && e.version <= d->version;
        if (known && d->kind != kind) {
            return what + " is a " + std::string(toString(d->kind)) + " effect in a " + std::string(toString(kind)) +
                   " stack";
        }
        for (const EffectParam& p : e.params) {
            const EffectParamInfo* info = known ? d->param(p.key) : nullptr;
            if (info) {
                if (!p.value.isValid(info->minValue, info->maxValue)) {
                    return what + " has parameter " + p.key + " outside its range or not finite";
                }
                if (!info->animatable && p.value.isCurve()) {
                    return what + " animates parameter " + p.key + ", which cannot be animated";
                }
            } else if (!p.value.isValid(-1e300, 1e300)) {
                return what + " has parameter " + p.key + " that is not finite";
            }
        }
    }
    return std::nullopt;
}

} // namespace wabcut
