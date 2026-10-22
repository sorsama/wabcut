#include "EffectsLibrary.h"

#include <algorithm>
#include <array>

namespace wabcut::detail {

namespace {

using C = EffectCapability;
using U = ParamUnit;

/// Concatenates parameter lists at compile time (shared groups such as a transition's border options).
template <std::size_t... N>
constexpr auto join(const std::array<EffectParamInfo, N>&... parts) {
    std::array<EffectParamInfo, (N + ...)> out{};
    std::size_t at = 0;
    ((std::copy(parts.begin(), parts.end(), out.begin() + static_cast<std::ptrdiff_t>(at)), at += N), ...);
    return out;
}

// ---- Transition options shared by Premiere's classic transitions (Effect Controls) -----------------------------------
//
// Transition parameters are constants (Transition::params). Start and End are where the transition begins and ends
// (0..100 %: the evaluator maps progress p to start + (end - start) p); Reverse plays the pattern backwards (B shows
// where the pattern at 1 - p does not); Border Width (sequence px) and Border Color draw a band on the A side of the
// pattern's edge; Anti-aliasing Quality softens the edge (Off: one pixel of coverage, never aliased; Low 2 px, Medium
// 3 px, High 5 px). Center is a point in the frame (0..1 of its width and height).

constexpr std::array<EffectParamInfo, 2> kStartEnd{{
    {"start", "Start", U::Fraction, 0.0, 0.0, 1.0, false},
    {"end", "End", U::Fraction, 1.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 6> kEdge{{
    {"borderWidth", "Border Width", U::Pixels, 0.0, 0.0, 200.0, false},
    {"borderColor.r", "Border Color Red", U::Fraction, 0.0, 0.0, 1.0, false},
    {"borderColor.g", "Border Color Green", U::Fraction, 0.0, 0.0, 1.0, false},
    {"borderColor.b", "Border Color Blue", U::Fraction, 0.0, 0.0, 1.0, false},
    {"reverse", "Reverse", U::Amount, 0.0, 0.0, 1.0, false},
    {"antiAliasing", "Anti-aliasing Quality", U::Amount, 0.0, 0.0, 3.0, false, "Off|Low|Medium|High"},
}};
constexpr std::array<EffectParamInfo, 1> kReverseOnly{{
    {"reverse", "Reverse", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 2> kCenter{{
    {"center.x", "Center X", U::Fraction, 0.5, 0.0, 1.0, false},
    {"center.y", "Center Y", U::Fraction, 0.5, 0.0, 1.0, false},
}};

// Edge selectors (Premiere's arrows around the transition's thumbnail).
constexpr std::string_view kFourWays = "West to East|North to South|East to West|South to North";
constexpr std::string_view kEightWays = "West to East|North to South|East to West|South to North|"
                                        "Northwest to Southeast|Northeast to Southwest|Southeast to Northwest|"
                                        "Southwest to Northeast";
constexpr std::string_view kCorners = "From Northwest|From Northeast|From Southeast|From Southwest";
constexpr std::string_view kOrientations = "Horizontal|Vertical";

constexpr std::array<EffectParamInfo, 1> kDirection4{{{"direction", "Direction", U::Amount, 0.0, 0.0, 3.0, false, kFourWays}}};
constexpr std::array<EffectParamInfo, 1> kDirection8{{{"direction", "Direction", U::Amount, 0.0, 0.0, 7.0, false, kEightWays}}};
constexpr std::array<EffectParamInfo, 1> kCorner{{{"direction", "Direction", U::Amount, 0.0, 0.0, 3.0, false, kCorners}}};
constexpr std::array<EffectParamInfo, 1> kFrom4{{
    {"direction", "Direction", U::Amount, 0.0, 0.0, 3.0, false, "From North|From East|From South|From West"},
}};
constexpr std::array<EffectParamInfo, 1> kOrientation{{
    {"direction", "Direction", U::Amount, 0.0, 0.0, 1.0, false, kOrientations},
}};

// ---- Per transition ---------------------------------------------------------------------------------------------------

constexpr auto kDissolveParams = kStartEnd;
constexpr std::array<EffectParamInfo, 3> kDipColor{{
    {"color.r", "Color Red", U::Fraction, 0.0, 0.0, 1.0, false},
    {"color.g", "Color Green", U::Fraction, 0.0, 0.0, 1.0, false},
    {"color.b", "Color Blue", U::Fraction, 0.0, 0.0, 1.0, false},
}};
constexpr auto kDipToColorParams = join(kStartEnd, kDipColor);
constexpr std::array<EffectParamInfo, 2> kWipeOwn{{
    {"angle", "Angle", U::Degrees, 0.0, 0.0, 360.0, false},
    {"feather", "Border Feather", U::Fraction, 0.0, 0.0, 1.0, false},
}};
constexpr auto kWipeParams = join(kWipeOwn, kStartEnd, kEdge);
constexpr auto kIrisParams = join(kStartEnd, kEdge, kCenter);
constexpr auto kEdgeParams = join(kStartEnd, kEdge);
constexpr auto kCornerParams = join(kStartEnd, kCorner, kEdge);
constexpr auto kOrientationParams = join(kStartEnd, kOrientation, kEdge);
constexpr auto kPushParams = join(kStartEnd, kDirection4, kReverseOnly);
constexpr auto kSlideParams = join(kStartEnd, kDirection8, kEdge);
constexpr auto kPageParams = join(kStartEnd, kCorner, kReverseOnly);
constexpr auto kDirectionEdgeParams = join(kStartEnd, kDirection4, kEdge);
constexpr auto kFromEdgeParams = join(kStartEnd, kFrom4, kEdge);
/// Random Wipe sweeps from top to bottom by default.
constexpr std::array<EffectParamInfo, 1> kDirection4Down{{{"direction", "Direction", U::Amount, 1.0, 0.0, 3.0, false, kFourWays}}};
constexpr auto kRandomWipeParams = join(kStartEnd, kDirection4Down, kEdge);

constexpr std::array<EffectParamInfo, 1> kBands{{{"bands", "Number of Bands", U::Amount, 7.0, 1.0, 64.0, false}}};
constexpr std::array<EffectParamInfo, 1> kBlinds{{{"bands", "Number of Bands", U::Amount, 6.0, 1.0, 64.0, false}}};
constexpr auto kBandSlideParams = join(kStartEnd, kOrientation, kBands, kEdge);
constexpr auto kBandWipeParams = join(kStartEnd, kOrientation, kBands, kEdge);
constexpr auto kBlindsParams = join(kStartEnd, kDirection4, kBlinds, kEdge);
constexpr std::array<EffectParamInfo, 2> kSlices8x8{{
    {"cells.x", "Horizontal Slices", U::Amount, 8.0, 1.0, 64.0, false},
    {"cells.y", "Vertical Slices", U::Amount, 8.0, 1.0, 64.0, false},
}};
constexpr auto kCheckerParams = join(kStartEnd, kDirection4, kSlices8x8, kEdge);
constexpr std::array<EffectParamInfo, 2> kBlocks{{
    {"cells.x", "Wide", U::Amount, 20.0, 1.0, 128.0, false},
    {"cells.y", "High", U::Amount, 20.0, 1.0, 128.0, false},
}};
constexpr auto kRandomBlocksParams = join(kStartEnd, kBlocks, kEdge);
constexpr std::array<EffectParamInfo, 2> kSpiralCells{{
    {"cells.x", "Horizontal", U::Amount, 6.0, 1.0, 64.0, false},
    {"cells.y", "Vertical", U::Amount, 4.0, 1.0, 64.0, false},
}};
constexpr std::array<EffectParamInfo, 2> kZigZagCells{{
    {"cells.x", "Horizontal", U::Amount, 8.0, 1.0, 64.0, false},
    {"cells.y", "Vertical", U::Amount, 6.0, 1.0, 64.0, false},
}};
constexpr auto kSpiralParams = join(kStartEnd, kSpiralCells, kEdge);
constexpr auto kZigZagParams = join(kStartEnd, kDirection4, kZigZagCells, kEdge);
constexpr std::array<EffectParamInfo, 1> kWedges{{{"wedges", "Number of Wedges", U::Amount, 8.0, 1.0, 32.0, false}}};
constexpr auto kPinwheelParams = join(kStartEnd, kWedges, kEdge, kCenter);
constexpr std::array<EffectParamInfo, 2> kGradientOwn{{
    {"gradient", "Gradient", U::Amount, 0.0, 0.0, 3.0, false,
     "Linear (Left to Right)|Linear (Top to Bottom)|Radial (Center Out)|Diagonal"},
    {"softness", "Softness", U::Amount, 10.0, 0.0, 100.0, false},
}};
constexpr auto kGradientWipeParams = join(kStartEnd, kGradientOwn, kReverseOnly);
constexpr auto kCrossZoomParams = join(kStartEnd, kCenter);

// Creator transitions (Premiere 26's Film Impact library has transitions of these names: Whip, Zoom Blur, Glitch,
// Light Leak, Flash).
constexpr std::array<EffectParamInfo, 1> kBlurAmount{{{"amount", "Blur", U::Amount, 50.0, 0.0, 100.0, false}}};
constexpr auto kWhipParams = join(kStartEnd, kDirection4, kBlurAmount);
constexpr std::array<EffectParamInfo, 1> kZoomDirection{{{"direction", "Direction", U::Amount, 0.0, 0.0, 1.0, false, "In|Out"}}};
constexpr auto kZoomBlurParams = join(kStartEnd, kZoomDirection, kBlurAmount, kCenter);
constexpr std::array<EffectParamInfo, 2> kGlitchOwn{{
    {"amount", "Intensity", U::Amount, 50.0, 0.0, 100.0, false},
    {"colorSplit", "Color Split", U::Pixels, 12.0, 0.0, 100.0, false},
}};
constexpr auto kGlitchParams = join(kStartEnd, kGlitchOwn);
constexpr std::array<EffectParamInfo, 4> kLightLeakOwn{{
    {"color.r", "Color Red", U::Fraction, 1.0, 0.0, 1.0, false},
    {"color.g", "Color Green", U::Fraction, 0.55, 0.0, 1.0, false},
    {"color.b", "Color Blue", U::Fraction, 0.2, 0.0, 1.0, false},
    {"intensity", "Intensity", U::Fraction, 1.0, 0.0, 2.0, false},
}};
constexpr auto kLightLeakParams = join(kStartEnd, kLightLeakOwn);
constexpr std::array<EffectParamInfo, 4> kFlashOwn{{
    {"color.r", "Color Red", U::Fraction, 1.0, 0.0, 1.0, false},
    {"color.g", "Color Green", U::Fraction, 1.0, 0.0, 1.0, false},
    {"color.b", "Color Blue", U::Fraction, 1.0, 0.0, 1.0, false},
    {"intensity", "Intensity", U::Fraction, 1.0, 0.0, 2.0, false},
}};
constexpr auto kFlashParams = join(kStartEnd, kFlashOwn);

// Animations (Premiere 25.5's, model/Animations.h): the evaluator moves the animated clip itself
// (playback::transitionAnimationAt). Options: Travel's side, Flip's axis, Fold's hinge edge, Spin's turn, Block's size.
constexpr std::array<EffectParamInfo, 1> kTravelSide{{{"side", "Side", U::Amount, 0.0, 0.0, 3.0, false, "Left|Right|Top|Bottom"}}};
constexpr std::array<EffectParamInfo, 1> kFlipAxis{{{"axis", "Axis", U::Amount, 0.0, 0.0, 1.0, false, "Horizontal|Vertical"}}};
constexpr std::array<EffectParamInfo, 1> kFoldHinge{{{"hinge", "Hinge", U::Amount, 0.0, 0.0, 3.0, false, "Top|Bottom|Left|Right"}}};
constexpr std::array<EffectParamInfo, 1> kSpinTurn{{{"turn", "Turn", U::Amount, 0.0, 0.0, 1.0, false, "Clockwise|Counterclockwise"}}};
constexpr std::array<EffectParamInfo, 1> kBlockSize{{{"blockSize", "Block Size", U::Pixels, 64.0, 2.0, 1000.0, false}}};
constexpr auto kTravelParams = join(kStartEnd, kTravelSide);
constexpr auto kFlipParams = join(kStartEnd, kFlipAxis);
constexpr auto kFoldParams = join(kStartEnd, kFoldHinge);
constexpr auto kSpinParams = join(kStartEnd, kSpinTurn);
constexpr auto kBlockParams = join(kStartEnd, kBlockSize);

constexpr auto kT = EffectKind::VideoTransition;
const EffectCapabilities kMix = C::TwoInputs;
const EffectCapabilities kPattern = C::TwoInputs | C::UsesAlpha;

// ---- Video effects (Premiere's bins; parameters as Premiere's Effect Controls lists them) ----------------------------
//
// Colours are display-referred 0..1 (r, g, b keys), "points" are fractions of the layer's frame (x, y keys), lengths
// sequence pixels, Premiere's 0..255 levels stay 0..255. The maths: render/reference/LibraryEffects.h.

// Adjust.
constexpr std::array<EffectParamInfo, 20> kLevelsParams{{
    {"rgb.blackInput", "(RGB) Black Input Level", U::Amount, 0.0, 0.0, 255.0},
    {"rgb.whiteInput", "(RGB) White Input Level", U::Amount, 255.0, 0.0, 255.0},
    {"rgb.blackOutput", "(RGB) Black Output Level", U::Amount, 0.0, 0.0, 255.0},
    {"rgb.whiteOutput", "(RGB) White Output Level", U::Amount, 255.0, 0.0, 255.0},
    {"rgb.gamma", "(RGB) Gamma", U::Amount, 100.0, 10.0, 1000.0},
    {"red.blackInput", "(R) Black Input Level", U::Amount, 0.0, 0.0, 255.0},
    {"red.whiteInput", "(R) White Input Level", U::Amount, 255.0, 0.0, 255.0},
    {"red.blackOutput", "(R) Black Output Level", U::Amount, 0.0, 0.0, 255.0},
    {"red.whiteOutput", "(R) White Output Level", U::Amount, 255.0, 0.0, 255.0},
    {"red.gamma", "(R) Gamma", U::Amount, 100.0, 10.0, 1000.0},
    {"green.blackInput", "(G) Black Input Level", U::Amount, 0.0, 0.0, 255.0},
    {"green.whiteInput", "(G) White Input Level", U::Amount, 255.0, 0.0, 255.0},
    {"green.blackOutput", "(G) Black Output Level", U::Amount, 0.0, 0.0, 255.0},
    {"green.whiteOutput", "(G) White Output Level", U::Amount, 255.0, 0.0, 255.0},
    {"green.gamma", "(G) Gamma", U::Amount, 100.0, 10.0, 1000.0},
    {"blue.blackInput", "(B) Black Input Level", U::Amount, 0.0, 0.0, 255.0},
    {"blue.whiteInput", "(B) White Input Level", U::Amount, 255.0, 0.0, 255.0},
    {"blue.blackOutput", "(B) Black Output Level", U::Amount, 0.0, 0.0, 255.0},
    {"blue.whiteOutput", "(B) White Output Level", U::Amount, 255.0, 0.0, 255.0},
    {"blue.gamma", "(B) Gamma", U::Amount, 100.0, 10.0, 1000.0},
}};
constexpr std::array<EffectParamInfo, 6> kProcAmpParams{{
    {"brightness", "Brightness", U::Amount, 0.0, -100.0, 100.0},
    {"contrast", "Contrast", U::Amount, 100.0, 0.0, 200.0},
    {"hue", "Hue", U::Degrees, 0.0, -180.0, 180.0},
    {"saturation", "Saturation", U::Amount, 100.0, 0.0, 200.0},
    {"splitScreen", "Split Screen", U::Amount, 0.0, 0.0, 1.0, false},
    {"splitPercent", "Split Percent", U::Fraction, 0.5, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 4> kExtractParams{{
    {"blackInput", "Black Input Level", U::Amount, 0.0, 0.0, 255.0},
    {"whiteInput", "White Input Level", U::Amount, 255.0, 0.0, 255.0},
    {"softness", "Softness", U::Amount, 0.0, 0.0, 100.0},
    {"invert", "Invert", U::Amount, 0.0, 0.0, 1.0, false},
}};
// Channel.
constexpr std::array<EffectParamInfo, 2> kInvertParams{{
    {"channel", "Channel", U::Amount, 0.0, 0.0, 12.0, false,
     "RGB|Red|Green|Blue|HLS|Hue|Lightness|Saturation|YIQ|Luminance|In Phase Chrominance|Quadrature Chrominance|Alpha"},
    {"blend", "Blend With Original", U::Fraction, 0.0, 0.0, 1.0},
}};
// Color Correction.
constexpr std::array<EffectParamInfo, 2> kBrightnessContrastParams{{
    {"brightness", "Brightness", U::Amount, 0.0, -100.0, 100.0},
    {"contrast", "Contrast", U::Amount, 0.0, -100.0, 100.0},
}};
constexpr std::array<EffectParamInfo, 7> kTintParams{{
    {"mapBlack.r", "Map Black To Red", U::Fraction, 0.0, 0.0, 1.0},
    {"mapBlack.g", "Map Black To Green", U::Fraction, 0.0, 0.0, 1.0},
    {"mapBlack.b", "Map Black To Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"mapWhite.r", "Map White To Red", U::Fraction, 1.0, 0.0, 1.0},
    {"mapWhite.g", "Map White To Green", U::Fraction, 1.0, 0.0, 1.0},
    {"mapWhite.b", "Map White To Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"amount", "Amount to Tint", U::Fraction, 1.0, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 11> kChangeColorParams{{
    {"view", "View", U::Amount, 0.0, 0.0, 1.0, false, "Corrected Layer|Color Correction Mask"},
    {"hueTransform", "Hue Transform", U::Degrees, 0.0, -180.0, 180.0},
    {"lightnessTransform", "Lightness Transform", U::Amount, 0.0, -100.0, 100.0},
    {"saturationTransform", "Saturation Transform", U::Amount, 0.0, -100.0, 100.0},
    {"color.r", "Color To Change Red", U::Fraction, 1.0, 0.0, 1.0},
    {"color.g", "Color To Change Green", U::Fraction, 0.0, 0.0, 1.0},
    {"color.b", "Color To Change Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"tolerance", "Matching Tolerance", U::Fraction, 0.15, 0.0, 1.0},
    {"softness", "Matching Softness", U::Fraction, 0.0, 0.0, 1.0},
    {"match", "Match Colors", U::Amount, 1.0, 0.0, 2.0, false, "Using RGB|Using Hue|Using Chroma"},
    {"invert", "Invert Color Correction Mask", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 13> kChangeToColorParams{{
    {"from.r", "From Red", U::Fraction, 1.0, 0.0, 1.0},
    {"from.g", "From Green", U::Fraction, 0.0, 0.0, 1.0},
    {"from.b", "From Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"to.r", "To Red", U::Fraction, 0.0, 0.0, 1.0},
    {"to.g", "To Green", U::Fraction, 0.0, 0.0, 1.0},
    {"to.b", "To Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"change", "Change", U::Amount, 0.0, 0.0, 3.0, false,
     "Hue|Hue & Lightness|Hue & Saturation|Hue, Lightness & Saturation"},
    {"changeBy", "Change By", U::Amount, 0.0, 0.0, 1.0, false, "Setting To Color|Transforming To Color"},
    {"tolerance.hue", "Tolerance: Hue", U::Fraction, 0.05, 0.0, 1.0},
    {"tolerance.lightness", "Tolerance: Lightness", U::Fraction, 0.5, 0.0, 1.0},
    {"tolerance.saturation", "Tolerance: Saturation", U::Fraction, 0.5, 0.0, 1.0},
    {"softness", "Softness", U::Fraction, 0.5, 0.0, 1.0},
    {"viewMatte", "View Correction Matte", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 7> kLeaveColorParams{{
    {"amount", "Amount to Decolor", U::Fraction, 0.0, 0.0, 1.0},
    {"color.r", "Color To Leave Red", U::Fraction, 1.0, 0.0, 1.0},
    {"color.g", "Color To Leave Green", U::Fraction, 0.0, 0.0, 1.0},
    {"color.b", "Color To Leave Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"tolerance", "Tolerance", U::Fraction, 0.3, 0.0, 1.0},
    {"softness", "Edge Softness", U::Fraction, 0.0, 0.0, 1.0},
    {"match", "Match colors", U::Amount, 0.0, 0.0, 1.0, false, "Using RGB|Using Hue"},
}};
constexpr std::array<EffectParamInfo, 10> kColorBalanceParams{{
    {"shadow.red", "Shadow Red Balance", U::Amount, 0.0, -100.0, 100.0},
    {"shadow.green", "Shadow Green Balance", U::Amount, 0.0, -100.0, 100.0},
    {"shadow.blue", "Shadow Blue Balance", U::Amount, 0.0, -100.0, 100.0},
    {"midtone.red", "Midtone Red Balance", U::Amount, 0.0, -100.0, 100.0},
    {"midtone.green", "Midtone Green Balance", U::Amount, 0.0, -100.0, 100.0},
    {"midtone.blue", "Midtone Blue Balance", U::Amount, 0.0, -100.0, 100.0},
    {"highlight.red", "Highlight Red Balance", U::Amount, 0.0, -100.0, 100.0},
    {"highlight.green", "Highlight Green Balance", U::Amount, 0.0, -100.0, 100.0},
    {"highlight.blue", "Highlight Blue Balance", U::Amount, 0.0, -100.0, 100.0},
    {"preserveLuminosity", "Preserve Luminosity", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 6> kVideoLimiterParams{{
    {"clipLevel", "Clip Level", U::Amount, 3.0, 0.0, 9.0, false,
     "100 IRE|101 IRE|102 IRE|103 IRE|104 IRE|105 IRE|106 IRE|107 IRE|108 IRE|109 IRE"},
    {"compression", "Compression Before Clipping", U::Amount, 0.0, 0.0, 4.0, false, "None|3%|5%|10%|20%"},
    {"gamutWarning", "Gamut Warning", U::Amount, 0.0, 0.0, 1.0, false},
    {"warningColor.r", "Gamut Warning Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"warningColor.g", "Gamut Warning Color Green", U::Fraction, 0.4, 0.0, 1.0},
    {"warningColor.b", "Gamut Warning Color Blue", U::Fraction, 0.7, 0.0, 1.0},
}};
// Image Control.
constexpr std::array<EffectParamInfo, 0> kNoParams{};
constexpr std::array<EffectParamInfo, 5> kColorPassParams{{
    {"color.r", "Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"color.g", "Color Green", U::Fraction, 0.0, 0.0, 1.0},
    {"color.b", "Color Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"similarity", "Similarity", U::Amount, 20.0, 0.0, 100.0},
    {"reverse", "Reverse", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 8> kColorReplaceParams{{
    {"target.r", "Target Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"target.g", "Target Color Green", U::Fraction, 0.0, 0.0, 1.0},
    {"target.b", "Target Color Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"replace.r", "Replace Color Red", U::Fraction, 0.0, 0.0, 1.0},
    {"replace.g", "Replace Color Green", U::Fraction, 0.0, 0.0, 1.0},
    {"replace.b", "Replace Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"similarity", "Similarity", U::Amount, 20.0, 0.0, 100.0},
    {"solid", "Solid Colors", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 1> kGammaParams{{{"gamma", "Gamma", U::Amount, 10.0, 1.0, 28.0}}};
// Stylize (pixel operations).
constexpr std::array<EffectParamInfo, 1> kPosterizeParams{{{"level", "Level", U::Amount, 6.0, 2.0, 255.0}}};
constexpr std::array<EffectParamInfo, 1> kSolarizeParams{{{"threshold", "Threshold", U::Fraction, 0.5, 0.0, 1.0}}};
constexpr std::array<EffectParamInfo, 1> kThresholdParams{{{"level", "Level", U::Amount, 128.0, 0.0, 255.0}}};
constexpr std::array<EffectParamInfo, 10> kStrobeParams{{
    {"color.r", "Strobe Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"color.g", "Strobe Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"color.b", "Strobe Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"blend", "Blend With Original", U::Fraction, 0.0, 0.0, 1.0},
    {"duration", "Strobe Duration (secs)", U::Amount, 0.05, 0.0, 100.0},
    {"period", "Strobe Period (secs)", U::Amount, 1.0, 0.001, 100.0},
    {"probability", "Random Strobe Probability", U::Fraction, 0.0, 0.0, 1.0},
    {"strobe", "Strobe", U::Amount, 0.0, 0.0, 1.0, false, "Operates On Color Only|Makes Layer Transparent"},
    {"operator", "Strobe Operator", U::Amount, 0.0, 0.0, 12.0, false,
     "Copy|Add|Subtract|Multiply|Difference|And|Or|Xor|Lighten|Darken|Minimum|Maximum|Screen"},
    {"seed", "Random Seed", U::Amount, 0.0, 0.0, 10000.0, false},
}};
// Keying (pixel operations).
constexpr std::array<EffectParamInfo, 4> kAlphaAdjustParams{{
    {"opacity", "Opacity", U::Fraction, 1.0, 0.0, 1.0},
    {"ignoreAlpha", "Ignore Alpha", U::Amount, 0.0, 0.0, 1.0, false},
    {"invertAlpha", "Invert Alpha", U::Amount, 0.0, 0.0, 1.0, false},
    {"maskOnly", "Mask Only", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 2> kLumaKeyParams{{
    {"threshold", "Threshold", U::Fraction, 0.0, 0.0, 1.0},
    {"cutoff", "Cutoff", U::Fraction, 0.0, 0.0, 1.0},
}};
// Noise & Grain.
constexpr std::array<EffectParamInfo, 3> kNoiseParams{{
    {"amount", "Amount of Noise", U::Fraction, 0.0, 0.0, 1.0},
    {"colorNoise", "Use Color Noise", U::Amount, 1.0, 0.0, 1.0, false},
    {"clip", "Clip Result Values", U::Amount, 1.0, 0.0, 1.0, false},
}};

// Distort, Transform, Perspective, Blur (resampling).
constexpr std::array<EffectParamInfo, 8> kCornerPinParams{{
    {"upperLeft.x", "Upper Left X", U::Fraction, 0.0, -10.0, 10.0},
    {"upperLeft.y", "Upper Left Y", U::Fraction, 0.0, -10.0, 10.0},
    {"upperRight.x", "Upper Right X", U::Fraction, 1.0, -10.0, 10.0},
    {"upperRight.y", "Upper Right Y", U::Fraction, 0.0, -10.0, 10.0},
    {"lowerLeft.x", "Lower Left X", U::Fraction, 0.0, -10.0, 10.0},
    {"lowerLeft.y", "Lower Left Y", U::Fraction, 1.0, -10.0, 10.0},
    {"lowerRight.x", "Lower Right X", U::Fraction, 1.0, -10.0, 10.0},
    {"lowerRight.y", "Lower Right Y", U::Fraction, 1.0, -10.0, 10.0},
}};
constexpr std::array<EffectParamInfo, 9> kLensDistortionParams{{
    {"curvature", "Curvature", U::Amount, 0.0, -100.0, 100.0},
    {"verticalDecentering", "Vertical Decentering", U::Amount, 0.0, -100.0, 100.0},
    {"horizontalDecentering", "Horizontal Decentering", U::Amount, 0.0, -100.0, 100.0},
    {"verticalPrism", "Vertical Prism FX", U::Amount, 0.0, -100.0, 100.0},
    {"horizontalPrism", "Horizontal Prism FX", U::Amount, 0.0, -100.0, 100.0},
    {"fillAlpha", "Fill Alpha", U::Amount, 1.0, 0.0, 1.0, false},
    {"fillColor.r", "Fill Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"fillColor.g", "Fill Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"fillColor.b", "Fill Color Blue", U::Fraction, 1.0, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 11> kMagnifyParams{{
    {"shape", "Shape", U::Amount, 0.0, 0.0, 1.0, false, "Circle|Square"},
    {"center.x", "Center X", U::Fraction, 0.5, -10.0, 10.0},
    {"center.y", "Center Y", U::Fraction, 0.5, -10.0, 10.0},
    {"magnification", "Magnification", U::Amount, 200.0, 100.0, 2000.0},
    {"link", "Link", U::Amount, 0.0, 0.0, 2.0, false, "None|Size To Magnification|Size & Feather To Magnification"},
    {"size", "Size", U::Pixels, 100.0, 0.0, 4000.0},
    {"feather", "Feather", U::Pixels, 0.0, 0.0, 1000.0},
    {"opacity", "Opacity", U::Fraction, 1.0, 0.0, 1.0},
    {"scaling", "Scaling", U::Amount, 0.0, 0.0, 2.0, false, "Standard|Soft|Scatter"},
    {"blendingMode", "Blending Mode", U::Amount, 1.0, 0.0, 8.0, false,
     "None|Normal|Add|Multiply|Screen|Overlay|Darken|Lighten|Difference"},
    {"resizeLayer", "Resize Layer", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 3> kMirrorParams{{
    {"center.x", "Reflection Center X", U::Fraction, 0.5, -10.0, 10.0},
    {"center.y", "Reflection Center Y", U::Fraction, 0.5, -10.0, 10.0},
    {"angle", "Reflection Angle", U::Degrees, 0.0, -3600.0, 3600.0},
}};
constexpr std::array<EffectParamInfo, 3> kOffsetParams{{
    {"center.x", "Shift Center To X", U::Fraction, 0.5, -10.0, 10.0},
    {"center.y", "Shift Center To Y", U::Fraction, 0.5, -10.0, 10.0},
    {"blend", "Blend With Original", U::Fraction, 0.0, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 3> kSpherizeParams{{
    {"radius", "Radius", U::Pixels, 0.0, 0.0, 2500.0},
    {"center.x", "Center of Sphere X", U::Fraction, 0.5, -10.0, 10.0},
    {"center.y", "Center of Sphere Y", U::Fraction, 0.5, -10.0, 10.0},
}};
constexpr std::array<EffectParamInfo, 4> kTwirlParams{{
    {"angle", "Angle", U::Degrees, 0.0, -36000.0, 36000.0},
    {"radius", "Twirl Radius", U::Amount, 30.0, 0.0, 100.0},
    {"center.x", "Twirl Center X", U::Fraction, 0.5, -10.0, 10.0},
    {"center.y", "Twirl Center Y", U::Fraction, 0.5, -10.0, 10.0},
}};
constexpr std::array<EffectParamInfo, 8> kWaveWarpParams{{
    {"waveType", "Wave Type", U::Amount, 0.0, 0.0, 8.0, false,
     "Sine|Square|Triangle|Sawtooth|Circle|Semicircle|Uncircle|Noise|Smooth Noise"},
    {"height", "Wave Height", U::Pixels, 10.0, -1000.0, 1000.0},
    {"width", "Wave Width", U::Pixels, 40.0, 1.0, 2000.0},
    {"direction", "Direction", U::Degrees, 90.0, -3600.0, 3600.0},
    {"speed", "Wave Speed", U::Amount, 1.0, -100.0, 100.0},
    {"pinning", "Pinning", U::Amount, 0.0, 0.0, 8.0, false,
     "None|All Edges|Center|Left Edge|Top Edge|Right Edge|Bottom Edge|Horizontal Edges|Vertical Edges"},
    {"phase", "Phase", U::Degrees, 0.0, -36000.0, 36000.0},
    {"antialiasing", "Antialiasing (Best Quality)", U::Amount, 1.0, 0.0, 2.0, false, "Low|Medium|High"},
}};
constexpr std::array<EffectParamInfo, 12> kTurbulentParams{{
    {"displacement", "Displacement", U::Amount, 0.0, 0.0, 8.0, false,
     "Turbulent|Bulge|Twist|Turbulent Smoother|Bulge Smoother|Twist Smoother|Vertical Displacement|"
     "Horizontal Displacement|Cross Displacement"},
    {"amount", "Amount", U::Amount, 50.0, -1000.0, 1000.0},
    {"size", "Size", U::Amount, 100.0, 2.0, 1000.0},
    {"offset.x", "Offset (Turbulence) X", U::Fraction, 0.5, -100.0, 100.0},
    {"offset.y", "Offset (Turbulence) Y", U::Fraction, 0.5, -100.0, 100.0},
    {"complexity", "Complexity", U::Amount, 1.0, 1.0, 10.0},
    {"evolution", "Evolution", U::Degrees, 0.0, -360000.0, 360000.0},
    {"cycleEvolution", "Cycle Evolution", U::Amount, 0.0, 0.0, 1.0, false},
    {"cycle", "Cycle (in Revolutions)", U::Amount, 1.0, 1.0, 1000.0, false},
    {"seed", "Random Seed", U::Amount, 0.0, 0.0, 10000.0, false},
    {"pinning", "Pinning", U::Amount, 1.0, 0.0, 7.0, false,
     "None|Pin All|Pin Horizontal|Pin Vertical|Pin Left|Pin Top|Pin Right|Pin Bottom"},
    {"antialiasing", "Antialiasing for Best Quality", U::Amount, 0.0, 0.0, 1.0, false, "Low|High"},
}};
constexpr std::array<EffectParamInfo, 14> kTransformParams{{
    {"anchor.x", "Anchor Point X", U::Fraction, 0.5, -100.0, 100.0},
    {"anchor.y", "Anchor Point Y", U::Fraction, 0.5, -100.0, 100.0},
    {"position.x", "Position X", U::Fraction, 0.5, -100.0, 100.0},
    {"position.y", "Position Y", U::Fraction, 0.5, -100.0, 100.0},
    {"uniformScale", "Uniform Scale", U::Amount, 1.0, 0.0, 1.0, false},
    {"scaleHeight", "Scale Height", U::Amount, 100.0, -30000.0, 30000.0},
    {"scaleWidth", "Scale Width", U::Amount, 100.0, -30000.0, 30000.0},
    {"skew", "Skew", U::Amount, 0.0, -70.0, 70.0},
    {"skewAxis", "Skew Axis", U::Degrees, 0.0, -36000.0, 36000.0},
    {"rotation", "Rotation", U::Degrees, 0.0, -36000.0, 36000.0},
    {"opacity", "Opacity", U::Fraction, 1.0, 0.0, 1.0},
    {"useCompositionShutter", "Use Composition's Shutter Angle", U::Amount, 1.0, 0.0, 1.0, false},
    {"shutterAngle", "Shutter Angle", U::Degrees, 0.0, 0.0, 360.0},
    {"sampling", "Sampling", U::Amount, 0.0, 0.0, 1.0, false, "Bilinear|Bicubic"},
}};
constexpr std::array<EffectParamInfo, 1> kEdgeFeatherParams{{{"amount", "Amount", U::Amount, 0.0, 0.0, 100.0}}};
constexpr std::array<EffectParamInfo, 1> kReplicateParams{{{"count", "Count", U::Amount, 2.0, 2.0, 16.0}}};
constexpr std::array<EffectParamInfo, 5> kBasic3DParams{{
    {"swivel", "Swivel", U::Degrees, 0.0, -36000.0, 36000.0},
    {"tilt", "Tilt", U::Degrees, 0.0, -36000.0, 36000.0},
    {"distance", "Distance to Image", U::Amount, 0.0, -99.0, 1000.0},
    {"specular", "Show Specular Highlight", U::Amount, 0.0, 0.0, 1.0, false},
    {"wireframe", "Draw Preview Wireframe", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 6> kBevelEdgesParams{{
    {"thickness", "Edge Thickness", U::Fraction, 0.1, 0.0, 0.5},
    {"lightAngle", "Light Angle", U::Degrees, -60.0, -36000.0, 36000.0},
    {"lightColor.r", "Light Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"lightColor.g", "Light Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"lightColor.b", "Light Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"intensity", "Light Intensity", U::Fraction, 0.2, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 2> kDirectionalBlurParams{{
    {"direction", "Direction", U::Degrees, 0.0, -36000.0, 36000.0},
    {"length", "Blur Length", U::Pixels, 0.0, 0.0, 1000.0},
}};
constexpr std::array<EffectParamInfo, 1> kCameraBlurParams{{{"percent", "Percent Blur", U::Amount, 0.0, 0.0, 100.0}}};

// Stylize, Blur & Sharpen, Keying, Perspective (neighbourhood; several passes).
constexpr std::array<EffectParamInfo, 3> kUnsharpMaskParams{{
    {"amount", "Amount", U::Amount, 50.0, 0.0, 500.0},
    {"radius", "Radius", U::Pixels, 1.0, 0.1, 250.0},
    {"threshold", "Threshold", U::Amount, 0.0, 0.0, 255.0},
}};
constexpr std::array<EffectParamInfo, 4> kEmbossParams{{
    {"direction", "Direction", U::Degrees, 45.0, -36000.0, 36000.0},
    {"relief", "Relief", U::Pixels, 1.0, 0.0, 10.0},
    {"contrast", "Contrast", U::Amount, 100.0, 0.0, 500.0},
    {"blend", "Blend With Original", U::Fraction, 0.0, 0.0, 1.0},
}};
/// Convolution Kernel (Premiere's Adjust bin): the 3 x 3 weights (M11 top left), Offset (added, 0..255 levels), Scale
/// (the sum's divisor) and Process Alpha.
constexpr std::array<EffectParamInfo, 12> kConvolutionKernelParams{{
    {"m11", "M11", U::Amount, 0.0, -999.0, 999.0},
    {"m12", "M12", U::Amount, 0.0, -999.0, 999.0},
    {"m13", "M13", U::Amount, 0.0, -999.0, 999.0},
    {"m21", "M21", U::Amount, 0.0, -999.0, 999.0},
    {"m22", "M22", U::Amount, 1.0, -999.0, 999.0},
    {"m23", "M23", U::Amount, 0.0, -999.0, 999.0},
    {"m31", "M31", U::Amount, 0.0, -999.0, 999.0},
    {"m32", "M32", U::Amount, 0.0, -999.0, 999.0},
    {"m33", "M33", U::Amount, 0.0, -999.0, 999.0},
    {"offset", "Offset", U::Amount, 0.0, -999.0, 999.0},
    {"scale", "Scale", U::Amount, 1.0, 1.0, 999.0},
    {"processAlpha", "Process Alpha", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 2> kFindEdgesParams{{
    {"invert", "Invert", U::Amount, 0.0, 0.0, 1.0, false},
    {"blend", "Blend With Original", U::Fraction, 0.0, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 2> kMedianParams{{
    {"radius", "Radius", U::Pixels, 1.0, 0.0, 100.0},
    {"operateOnAlpha", "Operate On Alpha Channel", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 3> kDustScratchesParams{{
    {"radius", "Radius", U::Pixels, 1.0, 0.0, 100.0},
    {"threshold", "Threshold", U::Amount, 0.0, 0.0, 255.0},
    {"operateOnAlpha", "Operate On Alpha Channel", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 10> kAlphaGlowParams{{
    {"glow", "Glow", U::Pixels, 15.0, 0.0, 100.0},
    {"brightness", "Brightness", U::Amount, 255.0, 0.0, 255.0},
    {"startColor.r", "Start Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"startColor.g", "Start Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"startColor.b", "Start Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"endColor.r", "End Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"endColor.g", "End Color Green", U::Fraction, 0.5, 0.0, 1.0},
    {"endColor.b", "End Color Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"useEndColor", "Use End Color", U::Amount, 0.0, 0.0, 1.0, false},
    {"fadeOut", "Fade Out", U::Amount, 1.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 6> kBevelAlphaParams{{
    {"thickness", "Edge Thickness", U::Pixels, 2.0, 0.0, 200.0},
    {"lightAngle", "Light Angle", U::Degrees, -60.0, -36000.0, 36000.0},
    {"lightColor.r", "Light Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"lightColor.g", "Light Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"lightColor.b", "Light Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"intensity", "Light Intensity", U::Fraction, 0.4, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 12> kRadialShadowParams{{
    {"color.r", "Shadow Color Red", U::Fraction, 0.0, 0.0, 1.0},
    {"color.g", "Shadow Color Green", U::Fraction, 0.0, 0.0, 1.0},
    {"color.b", "Shadow Color Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"opacity", "Opacity", U::Fraction, 0.5, 0.0, 1.0},
    {"light.x", "Light Source X", U::Fraction, 0.5, -10.0, 10.0},
    {"light.y", "Light Source Y", U::Fraction, 0.0, -10.0, 10.0},
    {"distance", "Projection Distance", U::Amount, 10.0, 0.0, 1000.0},
    {"softness", "Softness", U::Pixels, 0.0, 0.0, 1000.0},
    {"render", "Render", U::Amount, 0.0, 0.0, 1.0, false, "Regular|Glass Edge"},
    {"colorInfluence", "Color Influence", U::Fraction, 1.0, 0.0, 1.0},
    {"shadowOnly", "Shadow Only", U::Amount, 0.0, 0.0, 1.0, false},
    {"resizeLayer", "Resize Layer", U::Amount, 1.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 16> kRoughenEdgesParams{{
    {"edgeType", "Edge Type", U::Amount, 0.0, 0.0, 7.0, false,
     "Roughen|Roughen Color|Cut|Spiky|Rusty|Rusty Color|Photocopy|Photocopy Color"},
    {"edgeColor.r", "Edge Color Red", U::Fraction, 0.6, 0.0, 1.0},
    {"edgeColor.g", "Edge Color Green", U::Fraction, 0.3, 0.0, 1.0},
    {"edgeColor.b", "Edge Color Blue", U::Fraction, 0.1, 0.0, 1.0},
    {"border", "Border", U::Pixels, 8.0, 0.0, 500.0},
    {"sharpness", "Edge Sharpness", U::Amount, 1.0, 0.0, 10.0},
    {"fractalInfluence", "Fractal Influence", U::Fraction, 1.0, 0.0, 1.0},
    {"scale", "Scale", U::Amount, 100.0, 10.0, 1000.0},
    {"stretch", "Stretch Width or Height", U::Amount, 0.0, -10.0, 10.0},
    {"offset.x", "Offset (Turbulence) X", U::Fraction, 0.5, -100.0, 100.0},
    {"offset.y", "Offset (Turbulence) Y", U::Fraction, 0.5, -100.0, 100.0},
    {"complexity", "Complexity", U::Amount, 2.0, 1.0, 10.0},
    {"evolution", "Evolution", U::Degrees, 0.0, -360000.0, 360000.0},
    {"cycleEvolution", "Cycle Evolution", U::Amount, 0.0, 0.0, 1.0, false},
    {"cycle", "Cycle (in Revolutions)", U::Amount, 1.0, 1.0, 1000.0, false},
    {"seed", "Random Seed", U::Amount, 0.0, 0.0, 10000.0, false},
}};
constexpr std::array<EffectParamInfo, 6> kColorKeyParams{{
    {"keyColor.r", "Key Color Red", U::Fraction, 0.0, 0.0, 1.0},
    {"keyColor.g", "Key Color Green", U::Fraction, 0.0, 0.0, 1.0},
    {"keyColor.b", "Key Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"tolerance", "Color Tolerance", U::Amount, 0.0, 0.0, 255.0},
    {"edgeThin", "Edge Thin", U::Pixels, 0.0, -5.0, 5.0},
    {"edgeFeather", "Edge Feather", U::Pixels, 0.0, 0.0, 10.0},
}};
// Generate and the Transition bin's effects.
constexpr std::string_view kShapeModes = "None|Normal|Add|Multiply|Screen|Overlay|Darken|Lighten|Difference|Stencil Alpha|"
                                         "Silhouette Alpha";
constexpr std::array<EffectParamInfo, 24> kFourColorParams{{
    {"point1.x", "Point 1 X", U::Fraction, 0.1, -10.0, 10.0},
    {"point1.y", "Point 1 Y", U::Fraction, 0.1, -10.0, 10.0},
    {"color1.r", "Color 1 Red", U::Fraction, 1.0, 0.0, 1.0},
    {"color1.g", "Color 1 Green", U::Fraction, 1.0, 0.0, 1.0},
    {"color1.b", "Color 1 Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"point2.x", "Point 2 X", U::Fraction, 0.9, -10.0, 10.0},
    {"point2.y", "Point 2 Y", U::Fraction, 0.1, -10.0, 10.0},
    {"color2.r", "Color 2 Red", U::Fraction, 0.0, 0.0, 1.0},
    {"color2.g", "Color 2 Green", U::Fraction, 1.0, 0.0, 1.0},
    {"color2.b", "Color 2 Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"point3.x", "Point 3 X", U::Fraction, 0.1, -10.0, 10.0},
    {"point3.y", "Point 3 Y", U::Fraction, 0.9, -10.0, 10.0},
    {"color3.r", "Color 3 Red", U::Fraction, 1.0, 0.0, 1.0},
    {"color3.g", "Color 3 Green", U::Fraction, 0.0, 0.0, 1.0},
    {"color3.b", "Color 3 Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"point4.x", "Point 4 X", U::Fraction, 0.9, -10.0, 10.0},
    {"point4.y", "Point 4 Y", U::Fraction, 0.9, -10.0, 10.0},
    {"color4.r", "Color 4 Red", U::Fraction, 0.0, 0.0, 1.0},
    {"color4.g", "Color 4 Green", U::Fraction, 0.0, 0.0, 1.0},
    {"color4.b", "Color 4 Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"blend", "Blend", U::Amount, 100.0, 1.0, 10000.0},
    {"jitter", "Jitter", U::Fraction, 0.0, 0.0, 1.0},
    {"opacity", "Opacity", U::Fraction, 1.0, 0.0, 1.0},
    {"blendingMode", "Blending Mode", U::Amount, 0.0, 0.0, 8.0, false,
     "None|Normal|Add|Multiply|Screen|Overlay|Darken|Lighten|Difference"},
}};
constexpr std::array<EffectParamInfo, 13> kRampParams{{
    {"start.x", "Start of Ramp X", U::Fraction, 0.5, -10.0, 10.0},
    {"start.y", "Start of Ramp Y", U::Fraction, 0.0, -10.0, 10.0},
    {"startColor.r", "Start Color Red", U::Fraction, 0.0, 0.0, 1.0},
    {"startColor.g", "Start Color Green", U::Fraction, 0.0, 0.0, 1.0},
    {"startColor.b", "Start Color Blue", U::Fraction, 0.0, 0.0, 1.0},
    {"end.x", "End of Ramp X", U::Fraction, 0.5, -10.0, 10.0},
    {"end.y", "End of Ramp Y", U::Fraction, 1.0, -10.0, 10.0},
    {"endColor.r", "End Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"endColor.g", "End Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"endColor.b", "End Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"shape", "Ramp Shape", U::Amount, 0.0, 0.0, 1.0, false, "Linear Ramp|Radial Ramp"},
    {"scatter", "Ramp Scatter", U::Amount, 0.0, 0.0, 512.0},
    {"blend", "Blend With Original", U::Fraction, 0.0, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 13> kCircleParams{{
    {"center.x", "Center X", U::Fraction, 0.5, -10.0, 10.0},
    {"center.y", "Center Y", U::Fraction, 0.5, -10.0, 10.0},
    {"radius", "Radius", U::Pixels, 75.0, 0.0, 4000.0},
    {"edge", "Edge", U::Amount, 0.0, 0.0, 4.0, false,
     "None|Edge Radius|Thickness|Thickness * Radius|Thickness & Feather * Radius"},
    {"thickness", "Edge Radius / Thickness", U::Pixels, 10.0, 0.0, 4000.0},
    {"featherOuter", "Feather Outer Edge", U::Pixels, 0.0, 0.0, 1000.0},
    {"featherInner", "Feather Inner Edge", U::Pixels, 0.0, 0.0, 1000.0},
    {"invert", "Invert Circle", U::Amount, 0.0, 0.0, 1.0, false},
    {"color.r", "Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"color.g", "Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"color.b", "Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"opacity", "Opacity", U::Fraction, 1.0, 0.0, 1.0},
    {"blendingMode", "Blending Mode", U::Amount, 0.0, 0.0, 10.0, false, kShapeModes},
}};
constexpr std::array<EffectParamInfo, 16> kGridParams{{
    {"anchor.x", "Anchor X", U::Fraction, 0.5, -10.0, 10.0},
    {"anchor.y", "Anchor Y", U::Fraction, 0.5, -10.0, 10.0},
    {"sizeFrom", "Size From", U::Amount, 0.0, 0.0, 2.0, false, "Corner Point|Width Slider|Width & Height Sliders"},
    {"corner.x", "Corner X", U::Fraction, 0.55, -10.0, 10.0},
    {"corner.y", "Corner Y", U::Fraction, 0.6, -10.0, 10.0},
    {"width", "Width", U::Pixels, 64.0, 1.0, 4000.0},
    {"height", "Height", U::Pixels, 64.0, 1.0, 4000.0},
    {"border", "Border", U::Pixels, 3.0, 0.0, 1000.0},
    {"featherWidth", "Feather Width", U::Pixels, 0.0, 0.0, 1000.0},
    {"featherHeight", "Feather Height", U::Pixels, 0.0, 0.0, 1000.0},
    {"invert", "Invert Grid", U::Amount, 0.0, 0.0, 1.0, false},
    {"color.r", "Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"color.g", "Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"color.b", "Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"opacity", "Opacity", U::Fraction, 1.0, 0.0, 1.0},
    {"blendingMode", "Blending Mode", U::Amount, 0.0, 0.0, 10.0, false, kShapeModes},
}};
constexpr std::array<EffectParamInfo, 14> kCheckerboardParams{{
    {"anchor.x", "Anchor X", U::Fraction, 0.5, -10.0, 10.0},
    {"anchor.y", "Anchor Y", U::Fraction, 0.5, -10.0, 10.0},
    {"sizeFrom", "Size From", U::Amount, 1.0, 0.0, 2.0, false, "Corner Point|Width Slider|Width & Height Sliders"},
    {"corner.x", "Corner X", U::Fraction, 0.55, -10.0, 10.0},
    {"corner.y", "Corner Y", U::Fraction, 0.6, -10.0, 10.0},
    {"width", "Width", U::Pixels, 64.0, 1.0, 4000.0},
    {"height", "Height", U::Pixels, 64.0, 1.0, 4000.0},
    {"featherWidth", "Feather Width", U::Pixels, 0.0, 0.0, 1000.0},
    {"featherHeight", "Feather Height", U::Pixels, 0.0, 0.0, 1000.0},
    {"color.r", "Color Red", U::Fraction, 1.0, 0.0, 1.0},
    {"color.g", "Color Green", U::Fraction, 1.0, 0.0, 1.0},
    {"color.b", "Color Blue", U::Fraction, 1.0, 0.0, 1.0},
    {"opacity", "Opacity", U::Fraction, 1.0, 0.0, 1.0},
    {"blendingMode", "Blending Mode", U::Amount, 0.0, 0.0, 10.0, false, kShapeModes},
}};
constexpr std::array<EffectParamInfo, 5> kLensFlareParams{{
    {"center.x", "Flare Center X", U::Fraction, 0.25, -10.0, 10.0},
    {"center.y", "Flare Center Y", U::Fraction, 0.25, -10.0, 10.0},
    {"brightness", "Flare Brightness", U::Amount, 100.0, 10.0, 300.0},
    {"lensType", "Lens Type", U::Amount, 0.0, 0.0, 2.0, false, "50-300mm Zoom|35mm Prime|105mm Prime"},
    {"blend", "Blend With Original", U::Fraction, 0.0, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 5> kBlockDissolveParams{{
    {"completion", "Transition Completion", U::Fraction, 0.0, 0.0, 1.0},
    {"blockWidth", "Block Width", U::Pixels, 1.0, 1.0, 4000.0},
    {"blockHeight", "Block Height", U::Pixels, 1.0, 1.0, 4000.0},
    {"feather", "Feather", U::Pixels, 0.0, 0.0, 1000.0},
    {"softEdges", "Soft Edges (Best Quality)", U::Amount, 1.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 5> kGradientWipeEffectParams{{
    {"completion", "Transition Completion", U::Fraction, 0.0, 0.0, 1.0},
    {"softness", "Transition Softness", U::Fraction, 0.0, 0.0, 1.0},
    {"gradientLayer", "Gradient Layer (video track number; 0 = this layer)", U::Amount, 0.0, 0.0, 999.0, false},
    {"placement", "Gradient Placement", U::Amount, 2.0, 0.0, 2.0, false, "Tile Gradient|Center Gradient|Stretch Gradient to Fit"},
    {"invert", "Invert Gradient", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 3> kLinearWipeParams{{
    {"completion", "Transition Completion", U::Fraction, 0.0, 0.0, 1.0},
    {"angle", "Wipe Angle", U::Degrees, 90.0, -36000.0, 36000.0},
    {"feather", "Feather", U::Pixels, 0.0, 0.0, 4000.0},
}};
constexpr std::array<EffectParamInfo, 6> kRadialWipeEffectParams{{
    {"completion", "Transition Completion", U::Fraction, 0.0, 0.0, 1.0},
    {"startAngle", "Start Angle", U::Degrees, 0.0, -36000.0, 36000.0},
    {"center.x", "Wipe Center X", U::Fraction, 0.5, -10.0, 10.0},
    {"center.y", "Wipe Center Y", U::Fraction, 0.5, -10.0, 10.0},
    {"wipe", "Wipe", U::Amount, 0.0, 0.0, 2.0, false, "Clockwise|Counterclockwise|Both"},
    {"feather", "Feather", U::Pixels, 0.0, 0.0, 4000.0},
}};
constexpr std::array<EffectParamInfo, 4> kVenetianEffectParams{{
    {"completion", "Transition Completion", U::Fraction, 0.0, 0.0, 1.0},
    {"direction", "Direction", U::Degrees, 0.0, -36000.0, 36000.0},
    {"width", "Width", U::Pixels, 10.0, 1.0, 4000.0},
    {"feather", "Feather", U::Pixels, 0.0, 0.0, 4000.0},
}};
// Time, Video and the effects that read a second picture (the evaluator prepares it).
constexpr std::array<EffectParamInfo, 1> kPosterizeTimeParams{{{"frameRate", "Frame Rate", U::Amount, 12.0, 0.01, 99.0}}};
constexpr std::array<EffectParamInfo, 5> kEchoParams{{
    {"echoTime", "Echo Time (seconds)", U::Amount, -0.033, -10.0, 10.0},
    {"count", "Number of Echoes", U::Amount, 1.0, 0.0, 8.0},
    {"startingIntensity", "Starting Intensity", U::Fraction, 1.0, 0.0, 1.0},
    {"decay", "Decay", U::Fraction, 1.0, 0.0, 1.0},
    {"operator", "Echo Operator", U::Amount, 0.0, 0.0, 6.0, false,
     "Add|Maximum|Minimum|Screen|Composite In Back|Composite In Front|Blend"},
}};
constexpr std::array<EffectParamInfo, 11> kTimecodeParams{{
    {"position.x", "Position X", U::Fraction, 0.5, -10.0, 10.0},
    {"position.y", "Position Y", U::Fraction, 0.9, -10.0, 10.0},
    {"size", "Size", U::Amount, 15.0, 1.0, 100.0},
    {"opacity", "Opacity", U::Fraction, 0.5, 0.0, 1.0},
    {"fieldSymbol", "Field Symbol", U::Amount, 1.0, 0.0, 1.0, false},
    {"format", "Format", U::Amount, 0.0, 0.0, 3.0, false, "SMPTE|Frames|Feet + Frames 16mm|Feet + Frames 35mm"},
    {"source", "Timecode Source", U::Amount, 1.0, 0.0, 2.0, false, "Clip|Media|Generate"},
    {"timeDisplay", "Time Display", U::Amount, 0.0, 0.0, 5.0, false, "Sequence Rate|24|25|30|50|60"},
    {"offset", "Offset", U::Amount, 0.0, -50.0, 50.0},
    {"startingTimecode", "Starting Timecode (frames)", U::Amount, 0.0, 0.0, 1e8, false},
    {"labelText", "Label Text", U::Amount, 0.0, 0.0, 10.0, false,
     "None|Automatic|Camera 1|Camera 2|Camera 3|Camera 4|Camera 5|Camera 6|Camera 7|Camera 8|Camera 9"},
}};
constexpr std::array<EffectParamInfo, 6> kClipNameParams{{
    {"position.x", "Position X", U::Fraction, 0.5, -10.0, 10.0},
    {"position.y", "Position Y", U::Fraction, 0.9, -10.0, 10.0},
    {"alignment", "Alignment", U::Amount, 1.0, 0.0, 2.0, false, "Left|Center|Right"},
    {"size", "Size", U::Amount, 15.0, 1.0, 100.0},
    {"opacity", "Opacity", U::Fraction, 0.5, 0.0, 1.0},
    {"display", "Display", U::Amount, 0.0, 0.0, 2.0, false, "Sequence Clip Name|Project Clip Name|File Name"},
}};
constexpr std::array<EffectParamInfo, 6> kSimpleTextParams{{
    {"position.x", "Position X", U::Fraction, 0.5, -10.0, 10.0},
    {"position.y", "Position Y", U::Fraction, 0.5, -10.0, 10.0},
    {"alignment", "Alignment", U::Amount, 1.0, 0.0, 2.0, false, "Left|Center|Right"},
    {"size", "Size", U::Amount, 10.0, 1.0, 100.0},
    {"opacity", "Opacity", U::Fraction, 1.0, 0.0, 1.0},
    {"background", "Background Opacity", U::Fraction, 0.0, 0.0, 1.0},
}};
constexpr std::array<EffectParamInfo, 2> kImageMatteParams{{
    {"compositeUsing", "Composite Using", U::Amount, 1.0, 0.0, 1.0, false, "Matte Alpha|Matte Luma"},
    {"reverse", "Reverse", U::Amount, 0.0, 0.0, 1.0, false},
}};
constexpr std::array<EffectParamInfo, 6> kDifferenceMatteParams{{
    {"view", "View", U::Amount, 0.0, 0.0, 2.0, false, "Final Output|Source Only|Matte Only"},
    {"differenceLayer", "Difference Layer (video track number; 0 = none)", U::Amount, 0.0, 0.0, 999.0, false},
    {"ifSizesDiffer", "If Layer Sizes Differ", U::Amount, 1.0, 0.0, 1.0, false, "Center|Stretch to Fit"},
    {"tolerance", "Matching Tolerance", U::Fraction, 0.15, 0.0, 1.0},
    {"softness", "Matching Softness", U::Fraction, 0.0, 0.0, 1.0},
    {"blur", "Blur Before Difference", U::Pixels, 0.0, 0.0, 100.0},
}};

constexpr auto kV = EffectKind::Video;
const EffectCapabilities kPixel = C::PixelLocal;
const EffectCapabilities kPixelAlpha = C::PixelLocal | C::UsesAlpha;
const EffectCapabilities kWarp = C::Neighborhood | C::UsesAlpha;
const EffectCapabilities kWarpOut = C::Neighborhood | C::ExtendsBounds | C::UsesAlpha;

} // namespace

std::vector<EffectDescriptor> libraryTransitionDescriptors() {
    return {
        // Dissolve.
        {"crossDissolve", 1, "Cross Dissolve", "Dissolve", kT, false, kDissolveParams, kMix,
         "Blends the outgoing clip into the incoming one (in linear light, as the compositor blends)."},
        {"additiveDissolve", 1, "Additive Dissolve", "Dissolve", kT, false, kDissolveParams, kMix,
         "Adds the incoming clip to the outgoing one: A fades over the second half, B rises over the first, so the "
         "middle is brighter (an additive mix)."},
        {"dipToBlack", 1, "Dip to Black", "Dissolve", kT, false, kDissolveParams, kMix,
         "Fades the outgoing clip to black, then the incoming clip up from black."},
        {"dipToColor", 1, "Dip to Color", "Dissolve", kT, false, kDipToColorParams, kMix,
         "Fades the outgoing clip to a colour, then the incoming clip up from it."},
        {"dipToWhite", 1, "Dip to White", "Dissolve", kT, false, kDissolveParams, kMix, "Fades through white."},
        {"filmDissolve", 1, "Film Dissolve", "Dissolve", kT, false, kDissolveParams, kMix,
         "A dissolve in linear light (gamma 1.0), as film dissolves look."},
        {"nonAdditiveDissolve", 1, "Non-Additive Dissolve", "Dissolve", kT, false, kDissolveParams, kMix,
         "A non-additive mix: each channel shows the brighter of the two clips at their levels, so B's highlights "
         "appear first and A's shadows go last."},
        // Iris.
        {"irisBox", 1, "Iris Box", "Iris", kT, false, kIrisParams, kPattern,
         "The incoming clip opens in a rectangle (the frame's shape) from the center."},
        {"irisCross", 1, "Iris Cross", "Iris", kT, false, kIrisParams, kPattern,
         "The incoming clip opens in a cross from the center."},
        {"irisDiamond", 1, "Iris Diamond", "Iris", kT, false, kIrisParams, kPattern,
         "The incoming clip opens in a diamond from the center."},
        {"irisRound", 1, "Iris Round", "Iris", kT, false, kIrisParams, kPattern,
         "The incoming clip opens in a circle from the center."},
        // Page Peel.
        {"pagePeel", 1, "Page Peel", "Page Peel", kT, false, kPageParams, kPattern,
         "The outgoing clip peels away from a corner, its shaded back curling over, revealing the incoming clip."},
        {"pageTurn", 1, "Page Turn", "Page Peel", kT, false, kPageParams, kPattern,
         "The outgoing clip turns over like a page from a corner; its back shows the picture mirrored."},
        // Slide.
        {"bandSlide", 1, "Band Slide", "Slide", kT, false, kBandSlideParams, kPattern,
         "Bands of the incoming clip slide in from alternate sides."},
        {"centerSplit", 1, "Center Split", "Slide", kT, false, kEdgeParams, kPattern,
         "The outgoing clip splits into four quarters that move to the corners, revealing the incoming clip."},
        {"push", 1, "Push", "Slide", kT, false, kPushParams, kMix,
         "The incoming clip pushes the outgoing clip out of the frame."},
        {"slide", 1, "Slide", "Slide", kT, false, kSlideParams, kPattern,
         "The incoming clip slides in over the outgoing clip."},
        {"split", 1, "Split", "Slide", kT, false, kOrientationParams, kPattern,
         "The outgoing clip splits in two halves that slide apart, revealing the incoming clip."},
        {"whip", 1, "Whip", "Slide", kT, false, kWhipParams, kMix,
         "A fast push with directional motion blur (a whip pan): eases out of A and into B."},
        // Wipe.
        {"bandWipe", 1, "Band Wipe", "Wipe", kT, false, kBandWipeParams, kPattern,
         "Bands of the incoming clip wipe in from alternate sides."},
        {"barnDoors", 1, "Barn Doors", "Wipe", kT, false, kOrientationParams, kPattern,
         "The incoming clip opens from the center like a pair of doors."},
        {"checkerWipe", 1, "Checker Wipe", "Wipe", kT, false, kCheckerParams, kPattern,
         "A checkerboard of wipes: alternate squares wipe in half a square apart."},
        {"checkerBoard", 1, "CheckerBoard", "Wipe", kT, false, kCheckerParams, kPattern,
         "Two checkerboards of squares wipe in, one after the other."},
        {"clockWipe", 1, "Clock Wipe", "Wipe", kT, false, kFromEdgeParams, kPattern,
         "The incoming clip is wiped in by a clock hand sweeping around the center."},
        {"gradientWipe", 1, "Gradient Wipe", "Wipe", kT, false, kGradientWipeParams, kPattern,
         "The incoming clip appears where a gradient is darker than the transition's progress, softened by Softness."},
        {"inset", 1, "Inset", "Wipe", kT, false, kCornerParams, kPattern,
         "The incoming clip grows in a rectangle from a corner."},
        {"paintSplatter", 1, "Paint Splatter", "Wipe", kT, false, kEdgeParams, kPattern,
         "The incoming clip appears in growing splatters of paint."},
        {"pinwheel", 1, "Pinwheel", "Wipe", kT, false, kPinwheelParams, kPattern,
         "The incoming clip appears in rotating wedges around the center."},
        {"radialWipe", 1, "Radial Wipe", "Wipe", kT, false, kCornerParams, kPattern,
         "The incoming clip is wiped in by a line sweeping around a corner."},
        {"randomBlocks", 1, "Random Blocks", "Wipe", kT, false, kRandomBlocksParams, kPattern,
         "The incoming clip appears in blocks at random."},
        {"randomWipe", 1, "Random Wipe", "Wipe", kT, false, kRandomWipeParams, kPattern,
         "Small blocks of the incoming clip appear at random as a wipe moves across."},
        {"spiralBoxes", 1, "Spiral Boxes", "Wipe", kT, false, kSpiralParams, kPattern,
         "Boxes of the incoming clip spiral in from the edges to the center."},
        {"venetianBlinds", 1, "Venetian Blinds", "Wipe", kT, false, kBlindsParams, kPattern,
         "The incoming clip appears in bands, like opening blinds."},
        {"wedgeWipe", 1, "Wedge Wipe", "Wipe", kT, false, kFromEdgeParams, kPattern,
         "The incoming clip opens in a wedge, like a fan, from the center."},
        {"wipe", 1, "Wipe", "Wipe", kT, false, kWipeParams, kPattern,
         "A straight wipe at an angle (0 = left to right), optionally feathered."},
        {"zigZagBlocks", 1, "Zig-Zag Blocks", "Wipe", kT, false, kZigZagParams, kPattern,
         "Blocks of the incoming clip appear row by row, back and forth."},
        // Zoom.
        {"crossZoom", 1, "Cross Zoom", "Zoom", kT, false, kCrossZoomParams, kMix,
         "Zooms into the outgoing clip with a radial blur, then out of the incoming clip."},
        // Creator transitions (Premiere 26's Lights & Blurs and Grunge & Distort bins).
        {"zoomBlur", 1, "Zoom Blur", "Lights & Blurs", kT, false, kZoomBlurParams, kMix,
         "A punchy zoom with radial blur, in or out, cutting from A to B at its peak."},
        {"lightLeak", 1, "Light Leak", "Lights & Blurs", kT, false, kLightLeakParams, kMix,
         "Warm light leaks bloom across the frame while the clips dissolve under them."},
        {"flash", 1, "Flash", "Lights & Blurs", kT, false, kFlashParams, kMix,
         "A bright flash: the picture blows out to the colour and cuts to the incoming clip at its peak."},
        {"glitch", 1, "Glitch", "Grunge & Distort", kT, false, kGlitchParams, kMix,
         "Digital glitch: slices jump sideways, colours split and blocks flicker between the clips."},
        // Animations (Premiere 25.5): on a clip's In point the clip animates in, on its Out point out (the In animation
        // reversed); between two clips the incoming one animates in over the outgoing one.
        {"animationBlock", 1, "Block", "Animations", kT, false, kBlockParams, kMix,
         "The clip appears (disappears) in random blocks of Block Size pixels (Block Dissolve)."},
        {"animationFade", 1, "Fade", "Animations", kT, false, kStartEnd, kMix,
         "The clip fades in from transparent (out to transparent)."},
        {"animationFlip", 1, "Flip", "Animations", kT, false, kFlipParams, kMix,
         "The clip flips in from edge-on, turning about its vertical (Horizontal) or horizontal (Vertical) axis in 3D, "
         "and flips on out (Basic 3D)."},
        {"animationFold", 1, "Fold", "Animations", kT, false, kFoldParams, kMix,
         "The clip unfolds from its hinge edge, swinging into place in 3D, and folds away out (Basic 3D)."},
        {"animationPop", 1, "Pop", "Animations", kT, false, kStartEnd, kMix,
         "The clip pops up from nothing, overshoots a little and settles (out: swells, then pops down to nothing)."},
        {"animationPull", 1, "Pull", "Animations", kT, false, kStartEnd, kMix,
         "The clip pulls back from a 160 % close-up to full size while fading in (out: pushes in and fades)."},
        {"animationSpin", 1, "Spin", "Animations", kT, false, kSpinParams, kMix,
         "The clip spins in half a turn while growing to full size (out: spins on while shrinking to nothing)."},
        {"animationSpring", 1, "Spring", "Animations", kT, false, kStartEnd, kMix,
         "The clip springs up from below the frame, bounces and settles (out: hops, then drops out below the frame)."},
        {"animationTravel", 1, "Travel", "Animations", kT, false, kTravelParams, kMix,
         "The clip travels in from off the frame on one side (out: off the frame to that side)."},
        {"animationSurprise", 1, "Surprise Me", "Animations", kT, false, kStartEnd, kMix,
         "A random animation: becomes one of the Animations (with a random side, axis or hinge) when it is applied."},
    };
}

std::vector<EffectDescriptor> libraryVideoEffectDescriptors() {
    return {
        // Adjust.
        {"wabcut.adjust.levels", 1, "Levels", "Adjust", kV, false, kLevelsParams, kPixel,
         "Input and output black and white levels and gamma, for the RGB channels together and each channel: out = "
         "black out + (white out - black out) ((in - black in) / (white in - black in))^(100 / gamma), levels 0..255."},
        {"wabcut.adjust.procAmp", 1, "ProcAmp", "Adjust", kV, false, kProcAmpParams, kPixel,
         "A processing amplifier: brightness and contrast of the luma, hue and saturation of the chroma; Split Screen "
         "leaves the right part of the frame (past Split Percent) unprocessed for comparison."},
        {"wabcut.adjust.extract", 1, "Extract", "Adjust", kV, false, kExtractParams, kPixel,
         "Black and white: pixels whose luma is between the black and white input levels become white, the rest black "
         "(Softness ramps the edges with grey; Invert swaps them)."},
        {"wabcut.adjust.convolutionKernel", 1, "Convolution Kernel", "Adjust", kV, false, kConvolutionKernelParams, kWarp,
         "Each pixel becomes a weighted sum of itself and its eight neighbours: out = (M11 x top left + ... + M33 x "
         "bottom right) / Scale + Offset, on 0..255 levels of each channel (Process Alpha: the alpha too). Custom "
         "blurs, sharpens, embosses and edges; the Convolution Kernel presets have the classic kernels."},
        // Channel.
        {"wabcut.channel.invert", 1, "Invert", "Channel", kV, false, kInvertParams, kPixelAlpha,
         "Inverts the picture's channels: RGB or one of them, HLS or one of hue, lightness and saturation, YIQ or one of "
         "its components, or alpha; blended with the original."},
        // Color Correction.
        {"wabcut.color.brightnessContrast", 1, "Brightness & Contrast", "Color Correction", kV, false,
         kBrightnessContrastParams, kPixel, "Brightens or darkens the whole picture and raises or lowers its contrast."},
        {"wabcut.color.tint", 1, "Tint", "Color Correction", kV, false, kTintParams, kPixel,
         "Maps the picture's luminance between two colours (Map Black To, Map White To), by Amount to Tint."},
        {"wabcut.color.changeColor", 1, "Change Color", "Color Correction", kV, false, kChangeColorParams, kPixel,
         "Shifts the hue, lightness and saturation of a colour range (matched in RGB, by hue or by chroma, with "
         "tolerance and softness); View shows the correction mask."},
        {"wabcut.color.changeToColor", 1, "Change to Color", "Color Correction", kV, false, kChangeToColorParams,
         kPixel,
         "Changes a colour (From, within the hue, lightness and saturation tolerances) to another (To), setting or "
         "transforming the chosen components."},
        {"wabcut.color.leaveColor", 1, "Leave Color", "Color Correction", kV, false, kLeaveColorParams, kPixel,
         "Removes the colour from everything but one colour range (Tolerance, Edge Softness), by Amount to Decolor."},
        {"wabcut.color.balance", 1, "Color Balance", "Color Correction", kV, false, kColorBalanceParams, kPixel,
         "Red, green and blue balance of the shadows, midtones and highlights, optionally keeping the luminosity."},
        {"wabcut.color.videoLimiter", 1, "Video Limiter", "Color Correction", kV, false, kVideoLimiterParams, kPixel,
         "Keeps the signal within a broadcast level: values above Clip Level (100 IRE = video white) are compressed "
         "and clipped; Gamut Warning marks the limited pixels."},
        // Image Control.
        {"wabcut.image.blackWhite", 1, "Black & White", "Image Control", kV, false, kNoParams, kPixel,
         "Greyscale: the Rec. 709 luma of the picture."},
        {"wabcut.image.colorPass", 1, "Color Pass", "Image Control", kV, false, kColorPassParams, kPixel,
         "Keeps one colour (within Similarity) and turns everything else grey; Reverse keeps everything else."},
        {"wabcut.image.colorReplace", 1, "Color Replace", "Image Control", kV, false, kColorReplaceParams, kPixel,
         "Replaces a colour (within Similarity) with another, keeping its grey levels (or solid with Solid Colors)."},
        {"wabcut.image.gammaCorrection", 1, "Gamma Correction", "Image Control", kV, false, kGammaParams, kPixel,
         "Lightens or darkens the midtones: out = in^(Gamma / 10) (10 = unchanged)."},
        // Keying.
        {"wabcut.key.alphaAdjust", 1, "Alpha Adjust", "Keying", kV, false, kAlphaAdjustParams, kPixelAlpha,
         "The layer's alpha channel: its opacity, ignored or inverted, or shown alone (Mask Only)."},
        {"wabcut.key.luma", 1, "Luma Key", "Keying", kV, false, kLumaKeyParams, kPixelAlpha,
         "Keys out by luminance: below Cutoff transparent, above Threshold opaque, a ramp between (Cutoff above "
         "Threshold keys out the light areas instead)."},
        // Noise & Grain.
        {"wabcut.noise.noise", 1, "Noise", "Noise & Grain", kV, false, kNoiseParams, kPixel,
         "Random grain (new every frame), in colour or the same in each channel, clipped or wrapped."},
        // Stylize.
        {"wabcut.stylize.posterize", 1, "Posterize", "Stylize", kV, false, kPosterizeParams, kPixel,
         "Reduces each channel to Level tones."},
        {"wabcut.stylize.solarize", 1, "Solarize", "Stylize", kV, false, kSolarizeParams, kPixel,
         "Inverts the tones above Threshold: a blend of a negative and a positive picture."},
        {"wabcut.stylize.strobeLight", 1, "Strobe Light", "Stylize", kV, false, kStrobeParams, kPixelAlpha,
         "Flashes the strobe colour (or makes the layer transparent) for Strobe Duration every Strobe Period, plus at "
         "random by Random Strobe Probability, with an operator and a blend with the original."},
        {"wabcut.stylize.threshold", 1, "Threshold", "Stylize", kV, false, kThresholdParams, kPixel,
         "Black and white: luminance at or above Level becomes white, the rest black."},
        {"wabcut.stylize.replicate", 1, "Replicate", "Stylize", kV, false, kReplicateParams, kWarp,
         "Tiles the picture Count x Count times in the frame."},
        // Blur & Sharpen.
        {"wabcut.blur.directional", 1, "Directional Blur", "Blur & Sharpen", kV, false, kDirectionalBlurParams, kWarpOut,
         "Motion blur along Direction (0 degrees = vertical), Blur Length pixels long, equally on both sides."},
        {"wabcut.blur.camera", 1, "Camera Blur", "Blur & Sharpen", kV, false, kCameraBlurParams, kWarpOut,
         "Out-of-focus blur: an even disc of up to 40 pixels' radius by Percent Blur."},
        // Distort.
        {"wabcut.distort.cornerPin", 1, "Corner Pin", "Distort", kV, false, kCornerPinParams, kWarpOut,
         "Pins the picture's four corners anywhere (a perspective warp); the points are fractions of the frame."},
        {"wabcut.distort.lensDistortion", 1, "Lens Distortion", "Distort", kV, false, kLensDistortionParams, kWarp,
         "Lens curvature (convex or concave), decentring and prism effects; the uncovered frame is transparent or "
         "the fill colour."},
        {"wabcut.distort.magnify", 1, "Magnify", "Distort", kV, false, kMagnifyParams, kWarp,
         "A magnifying glass: a circle or square of Size, Magnification, Feather and Opacity, blended over the picture."},
        {"wabcut.distort.mirror", 1, "Mirror", "Distort", kV, false, kMirrorParams, kWarp,
         "Reflects the picture across a line through the Reflection Center at Reflection Angle (0 = the left side onto "
         "the right)."},
        {"wabcut.distort.offset", 1, "Offset", "Distort", kV, false, kOffsetParams, kWarp,
         "Pans the picture so its centre lands on Shift Center To, wrapping around the edges."},
        {"wabcut.distort.spherize", 1, "Spherize", "Distort", kV, false, kSpherizeParams, kWarp,
         "Wraps a circular area (Radius, Center of Sphere) around a sphere."},
        {"wabcut.distort.twirl", 1, "Twirl", "Distort", kV, false, kTwirlParams, kWarp,
         "Twists the picture around its centre, most at the centre (Angle) and nothing at Twirl Radius (a percentage of "
         "the larger side; 50 reaches the edges)."},
        {"wabcut.distort.waveWarp", 1, "Wave Warp", "Distort", kV, false, kWaveWarpParams, kWarpOut,
         "Waves travelling across the picture (type, height, width, direction, speed in cycles per second, phase), "
         "pinned at the chosen edges; it animates by itself."},
        {"wabcut.distort.turbulentDisplace", 1, "Turbulent Displace", "Distort", kV, false, kTurbulentParams, kWarpOut,
         "Fractal-noise displacement (turbulent, bulge, twist, vertical, horizontal, cross), animated with Evolution."},
        {"wabcut.distort.transform", 1, "Transform", "Distort", kV, false, kTransformParams, kWarpOut,
         "Anchor point, position, scale (uniform or by height and width), skew, rotation and opacity, applied at its "
         "place in the stack (unlike Motion, after the effects above it)."},
        // Transform.
        {"wabcut.transform.horizontalFlip", 1, "Horizontal Flip", "Transform", kV, false, kNoParams, kWarp,
         "Flips the picture left to right."},
        {"wabcut.transform.verticalFlip", 1, "Vertical Flip", "Transform", kV, false, kNoParams, kWarp,
         "Flips the picture upside down."},
        {"wabcut.transform.edgeFeather", 1, "Edge Feather", "Transform", kV, false, kEdgeFeatherParams, kPixelAlpha,
         "Feathers the edges of the frame to transparent; Amount sets the width."},
        // Perspective.
        {"wabcut.perspective.basic3d", 1, "Basic 3D", "Perspective", kV, false, kBasic3DParams, kWarpOut,
         "Turns the picture in 3D (Swivel around the vertical axis, Tilt around the horizontal one) and moves it away "
         "(Distance to Image), with an optional specular highlight; the back shows the picture mirrored."},
        {"wabcut.perspective.bevelEdges", 1, "Bevel Edges", "Perspective", kV, false, kBevelEdgesParams, kPixel,
         "Lit, bevelled frame edges: Edge Thickness, Light Angle, Light Color and Intensity."},
        {"wabcut.perspective.bevelAlpha", 1, "Bevel Alpha", "Perspective", kV, false, kBevelAlphaParams, kWarp,
         "Lights and shades the edges of the layer's alpha as a bevel of Edge Thickness pixels."},
        {"wabcut.perspective.radialShadow", 1, "Radial Shadow", "Perspective", kV, false, kRadialShadowParams, kWarpOut,
         "A shadow cast from a point light (Light Source) at Projection Distance, softened, Regular or Glass Edge (the "
         "layer's colour shows through), under the layer or alone."},
        // Blur & Sharpen.
        {"wabcut.blur.unsharpMask", 1, "Unsharp Mask", "Blur & Sharpen", kV, false, kUnsharpMaskParams, kWarp,
         "Sharpens by the difference from a blurred copy (Radius), by Amount, where it passes Threshold (0..255)."},
        // Stylize (neighbourhood).
        {"wabcut.stylize.alphaGlow", 1, "Alpha Glow", "Stylize", kV, false, kAlphaGlowParams, kWarpOut,
         "A glow around the layer's alpha edges: Glow (its reach), Brightness, a start colour fading to the end "
         "colour, or solid without Fade Out."},
        {"wabcut.stylize.colorEmboss", 1, "Color Emboss", "Stylize", kV, false, kEmbossParams, kWarp,
         "Embosses the picture's edges in Direction, keeping its colours."},
        {"wabcut.stylize.emboss", 1, "Emboss", "Stylize", kV, false, kEmbossParams, kWarp,
         "Embosses the picture's edges in Direction on grey."},
        {"wabcut.stylize.findEdges", 1, "Find Edges", "Stylize", kV, false, kFindEdgesParams, kWarp,
         "Draws the picture's edges: dark lines on white (Invert: bright lines on black), blended with the original."},
        {"wabcut.stylize.roughenEdges", 1, "Roughen Edges", "Stylize", kV, false, kRoughenEdgesParams, kWarp,
         "Roughens the layer's alpha edges with fractal noise (Border, Edge Sharpness, Fractal Influence, Scale, "
         "Complexity, Evolution); the Color types tint the edge."},
        // Noise & Grain.
        {"wabcut.noise.dustScratches", 1, "Dust & Scratches", "Noise & Grain", kV, false, kDustScratchesParams, kWarp,
         "Replaces pixels that differ from their neighbourhood's median (Radius) by more than Threshold."},
        {"wabcut.noise.median", 1, "Median", "Noise & Grain", kV, false, kMedianParams, kWarp,
         "Each pixel becomes the median of its neighbourhood (Radius): removes noise, paints."},
        // Keying.
        {"wabcut.key.colorKey", 1, "Color Key", "Keying", kV, false, kColorKeyParams, kWarp,
         "Keys out the colours within Color Tolerance of Key Color, then thins (or grows) and feathers the matte's edge."},
        // Generate.
        {"wabcut.generate.fourColorGradient", 1, "4-Color Gradient", "Generate", kV, false, kFourColorParams, kPixel,
         "A gradient of four colours at four points (Blend, Jitter), at Opacity with a Blending Mode."},
        {"wabcut.generate.checkerboard", 1, "Checkerboard", "Generate", kV, false, kCheckerboardParams, kPixel,
         "A checkerboard of the colour and transparent squares, sized from the anchor and a corner or by width and "
         "height."},
        {"wabcut.generate.circle", 1, "Circle", "Generate", kV, false, kCircleParams, kPixel,
         "A circle or ring (Edge), feathered, in a colour, at Opacity with a Blending Mode (Stencil and Silhouette Alpha "
         "cut the layer)."},
        {"wabcut.generate.grid", 1, "Grid", "Generate", kV, false, kGridParams, kPixel,
         "A grid of lines (Border wide, feathered), sized from the anchor and a corner or by width and height."},
        {"wabcut.generate.lensFlare", 1, "Lens Flare", "Generate", kV, false, kLensFlareParams, kPixel,
         "A lens flare (glow, streak and ghosts along the line through the centre) at Flare Center, by lens type."},
        {"wabcut.generate.ramp", 1, "Ramp", "Generate", kV, false, kRampParams, kPixel,
         "A linear or radial gradient between two colours at two points, with scatter, blended with the original."},
        // Transition (effects animated by Transition Completion).
        {"wabcut.transition.blockDissolve", 1, "Block Dissolve", "Transition", kV, false, kBlockDissolveParams, kPixelAlpha,
         "Blocks of the layer disappear at random as Transition Completion rises."},
        {"wabcut.transition.gradientWipe", 1, "Gradient Wipe", "Transition", kV, false, kGradientWipeEffectParams,
         kPixelAlpha,
         "The layer disappears where a gradient (another track's picture, or the layer's own luminance) is darker than "
         "Transition Completion, softened by Transition Softness."},
        {"wabcut.transition.linearWipe", 1, "Linear Wipe", "Transition", kV, false, kLinearWipeParams, kPixelAlpha,
         "Wipes the layer away along Wipe Angle (90 = left to right) by Transition Completion, feathered."},
        {"wabcut.transition.radialWipe", 1, "Radial Wipe", "Transition", kV, false, kRadialWipeEffectParams, kPixelAlpha,
         "Wipes the layer away around a centre from Start Angle (clockwise, counter-clockwise or both)."},
        {"wabcut.transition.venetianBlinds", 1, "Venetian Blinds", "Transition", kV, false, kVenetianEffectParams,
         kPixelAlpha, "Wipes the layer away in strips of Width along Direction."},
        // Keying with a second picture.
        {"wabcut.key.differenceMatte", 1, "Difference Matte", "Keying", kV, false, kDifferenceMatteParams, kPixelAlpha,
         "Keys out what matches another track's picture (Difference Layer: a clean plate), within Matching Tolerance and "
         "Softness; View shows the source or the matte."},
        {"wabcut.key.imageMatte", 1, "Image Matte Key", "Keying", kV, false, kImageMatteParams, kPixelAlpha,
         "Shows the clip through a still image's alpha or luma (effect data \"image\": \"media:<id>\"), optionally "
         "reversed; the image is stretched to the frame."},
        // Time (the evaluator applies them).
        {"wabcut.time.echo", 1, "Echo", "Time", kV, false, kEchoParams, C::UsesAlpha,
         "Combines the clip with its own frames Echo Time apart (up to 8 echoes), each at Starting Intensity x "
         "Decay^n, by the operator. Like Premiere's, the echoes are the clip's frames without the effects above it."},
        {"wabcut.time.posterizeTime", 1, "Posterize Time", "Time", kV, false, kPosterizeTimeParams, C::None,
         "Plays the clip at Frame Rate: each picture, and every effect, holds for 1 / Frame Rate seconds of the clip."},
        // Video (text drawn over the clip).
        {"wabcut.video.clipName", 1, "Clip Name", "Video", kV, false, kClipNameParams, kPixelAlpha,
         "Draws the clip's name (in the sequence or the project) or its file's on a box."},
        {"wabcut.video.simpleText", 1, "Simple Text", "Video", kV, false, kSimpleTextParams, kPixelAlpha,
         "Draws a line of text (effect data \"text\") over the clip, optionally on a box."},
        {"wabcut.video.timecode", 1, "Timecode", "Video", kV, false, kTimecodeParams, kPixelAlpha,
         "Burns in the timecode (of the clip, its media or generated) in SMPTE, frames or feet + frames, on a box."},
    };
}

} // namespace wabcut::detail
