#pragma once

// Blend modes (C.15): how a layer combines with what is below it. One list for clips (the Opacity section's Blend Mode,
// ClipContent::blendMode), graphic layers (Graphics.h) and adjustment layers. The reference math for the render owner
// is in docs/internal/c15-parity-handoff.md ("Blend modes"): premultiplied linear-light RGBA, the formula per mode, then the
// result mixed over the backdrop by the layer's alpha x opacity.
//
// Values are stable (saved by name, never by number); new modes are appended. The first eight are the C.13 graphic
// modes; `Add` is Premiere's Linear Dodge (Add).

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace wabcut {

enum class BlendMode : std::uint8_t {
    Normal,
    Multiply,
    Screen,
    Overlay,
    Add, ///< Linear Dodge (Add)
    Darken,
    Lighten,
    Difference,
    // C.15: the rest of Premiere's list.
    Dissolve,
    ColorBurn,
    LinearBurn,
    DarkerColor,
    ColorDodge,
    LighterColor,
    SoftLight,
    HardLight,
    VividLight,
    LinearLight,
    PinLight,
    HardMix,
    Exclusion,
    Subtract,
    Divide,
    Hue,
    Saturation,
    Color,
    Luminosity,
};

inline constexpr std::size_t kBlendModeCount = 27;

/// Stable name ("normal", "colorDodge", ...), as saved in projects and taken by the commands.
std::string_view toString(BlendMode mode);
std::optional<BlendMode> blendModeFromString(std::string_view text);
/// Premiere's menu label ("Linear Dodge (Add)").
std::string_view displayName(BlendMode mode);
/// Every mode in Premiere's menu order and groups: Normal, Dissolve | Darken, Multiply, Color Burn, Linear Burn, Darker
/// Color | Lighten, Screen, Color Dodge, Linear Dodge (Add), Lighter Color | Overlay, Soft Light, Hard Light, Vivid
/// Light, Linear Light, Pin Light, Hard Mix | Difference, Exclusion, Subtract, Divide | Hue, Saturation, Color,
/// Luminosity.
std::span<const BlendMode> blendModesInMenuOrder();
/// The menu group (0 normal, 1 darken, 2 lighten, 3 contrast, 4 inversion, 5 component) a mode belongs to.
int blendModeGroup(BlendMode mode);

} // namespace wabcut
