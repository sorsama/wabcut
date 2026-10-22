#pragma once

// Premiere's video effect library on the GPU (0.2, docs/effects.md): which registry effects the compositor implements as
// EffectType::Library layer effects, the order of their parameters in LayerEffect::params, and the shader family each
// runs in. The evaluator (playback/Evaluate.cpp) fills LayerEffect::params from `keys`; the compositor (Compositor.cpp)
// passes them to the family shader (shaders/fx_color.frag, fx_distort.frag, fx_stylize.frag, fx_generate.frag) as
// p0..p7; render/reference/LibraryEffects.h is the CPU twin of every op, with the same values.
//
// Units are the registry's (EffectsLibrary.cpp): user colours are display-referred 0..1, lengths sequence pixels,
// angles degrees, points fractions of the layer's frame. Shaders convert (sRGB to linear, pixels to canvas texels by the
// preview divisor) as their CPU twins do.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace wabcut::render {

/// The family shader an op runs in (Compositor kFxFamilyShaders).
enum class FxFamily : std::uint8_t { Core = 0, Transition = 1, Color = 2, Distort = 3, Stylize = 4, Generate = 5 };

/// The library's effects (LayerEffect::op). Values are stable within a build only (the shaders' constants): the
/// registry id is the persistent name. Ranges: 1..39 the Color family, 40..69 Distort, 70..99 Stylize, 100.. Generate.
enum class LibraryOp : std::uint16_t {
    None = 0,
    // Color: pixel operations on the straight, BT.709-encoded colour (the working space's encoding), as Premiere's
    // 8-bit effects see it.
    BrightnessContrast = 1,
    Levels = 2,
    ProcAmp = 3,
    Extract = 4,
    Invert = 5,
    Tint = 6,
    ChangeColor = 7,
    ChangeToColor = 8,
    LeaveColor = 9,
    ColorBalance = 10,
    VideoLimiter = 11,
    BlackWhite = 12,
    ColorPass = 13,
    ColorReplace = 14,
    GammaCorrection = 15,
    Posterize = 16,
    Solarize = 17,
    Threshold = 18,
    StrobeLight = 19,
    AlphaAdjust = 20,
    LumaKey = 21,
    Noise = 22,
    // Distort: resampling of the layer (positions in its frame, through the inverse of its Motion).
    CornerPin = 40,
    LensDistortion = 41,
    Magnify = 42,
    Mirror = 43,
    Offset = 44,
    Spherize = 45,
    Twirl = 46,
    WaveWarp = 47,
    TurbulentDisplace = 48,
    Transform = 49,
    HorizontalFlip = 50,
    VerticalFlip = 51,
    EdgeFeather = 52,
    Replicate = 53,
    Basic3D = 54,
    BevelEdges = 55,
    DirectionalBlur = 56,
    CameraBlur = 57,
    // Stylize: neighbourhood operations; some take a blurred copy (Compositor) as their second input.
    UnsharpMask = 70,
    Emboss = 71,
    ColorEmboss = 72,
    FindEdges = 73,
    Median = 74,
    DustScratches = 75,
    AlphaGlow = 76,
    BevelAlpha = 77,
    RadialShadow = 78,
    RoughenEdges = 79,
    ColorKey = 80,
    ConvolutionKernel = 81,
    // Generate: pictures drawn over or instead of the layer, the transition effects, and second pictures.
    FourColorGradient = 100,
    Ramp = 101,
    Circle = 102,
    Grid = 103,
    Checkerboard = 104,
    LensFlare = 105,
    BlockDissolve = 106,
    GradientWipe = 107,
    LinearWipe = 108,
    RadialWipe = 109,
    VenetianBlinds = 110,
    Echo = 111,
    TextOverlay = 112, ///< Timecode, Clip Name, Simple Text: LayerEffect::matte is the text drawn on the canvas
    DifferenceMatte = 113,
    ImageMatteKey = 114,
};

constexpr FxFamily familyOf(LibraryOp op) {
    const auto v = static_cast<std::uint16_t>(op);
    return v < 40 ? FxFamily::Color : v < 70 ? FxFamily::Distort : v < 100 ? FxFamily::Stylize : FxFamily::Generate;
}

/// The evaluator's extras after the descriptor's parameters (LayerEffect::params indices).
inline constexpr std::size_t kLibraryTimeParam = 31; ///< the curve time in seconds (EffectLibraryEntry::needsTime)

/// One registry effect the compositor implements.
struct EffectLibraryEntry {
    std::string_view id;
    LibraryOp op = LibraryOp::None;
    /// The descriptor's parameter keys in LayerEffect::params order (empty = unused).
    std::array<std::string_view, 31> keys{};
    /// The effect animates by itself (Noise grain, Wave Warp's speed): params[kLibraryTimeParam] = curve time (s).
    bool needsTime = false;
};

/// Every library effect, by registry id.
std::span<const EffectLibraryEntry> effectLibrary();
/// The entry of a registry id, or null.
const EffectLibraryEntry* findLibraryEffect(std::string_view id);

} // namespace wabcut::render
