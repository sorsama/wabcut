// Blend modes (C.15): names, Premiere's labels and menu order.

#include <wabcut/model/Blend.h>

#include <array>

namespace wabcut {

namespace {

struct ModeInfo {
    BlendMode mode;
    std::string_view name;
    std::string_view label;
    int group;
};

/// In Premiere's menu order.
constexpr std::array<ModeInfo, kBlendModeCount> kModes{{
    {BlendMode::Normal, "normal", "Normal", 0},
    {BlendMode::Dissolve, "dissolve", "Dissolve", 0},
    {BlendMode::Darken, "darken", "Darken", 1},
    {BlendMode::Multiply, "multiply", "Multiply", 1},
    {BlendMode::ColorBurn, "colorBurn", "Color Burn", 1},
    {BlendMode::LinearBurn, "linearBurn", "Linear Burn", 1},
    {BlendMode::DarkerColor, "darkerColor", "Darker Color", 1},
    {BlendMode::Lighten, "lighten", "Lighten", 2},
    {BlendMode::Screen, "screen", "Screen", 2},
    {BlendMode::ColorDodge, "colorDodge", "Color Dodge", 2},
    {BlendMode::Add, "add", "Linear Dodge (Add)", 2},
    {BlendMode::LighterColor, "lighterColor", "Lighter Color", 2},
    {BlendMode::Overlay, "overlay", "Overlay", 3},
    {BlendMode::SoftLight, "softLight", "Soft Light", 3},
    {BlendMode::HardLight, "hardLight", "Hard Light", 3},
    {BlendMode::VividLight, "vividLight", "Vivid Light", 3},
    {BlendMode::LinearLight, "linearLight", "Linear Light", 3},
    {BlendMode::PinLight, "pinLight", "Pin Light", 3},
    {BlendMode::HardMix, "hardMix", "Hard Mix", 3},
    {BlendMode::Difference, "difference", "Difference", 4},
    {BlendMode::Exclusion, "exclusion", "Exclusion", 4},
    {BlendMode::Subtract, "subtract", "Subtract", 4},
    {BlendMode::Divide, "divide", "Divide", 4},
    {BlendMode::Hue, "hue", "Hue", 5},
    {BlendMode::Saturation, "saturation", "Saturation", 5},
    {BlendMode::Color, "color", "Color", 5},
    {BlendMode::Luminosity, "luminosity", "Luminosity", 5},
}};

constexpr std::array<BlendMode, kBlendModeCount> menuOrder() {
    std::array<BlendMode, kBlendModeCount> out{};
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        out[i] = kModes[i].mode;
    }
    return out;
}

constexpr std::array<BlendMode, kBlendModeCount> kMenuOrder = menuOrder();

const ModeInfo& info(BlendMode mode) {
    for (const ModeInfo& m : kModes) {
        if (m.mode == mode) {
            return m;
        }
    }
    return kModes[0];
}

} // namespace

std::string_view toString(BlendMode mode) { return info(mode).name; }

std::optional<BlendMode> blendModeFromString(std::string_view text) {
    for (const ModeInfo& m : kModes) {
        if (m.name == text) {
            return m.mode;
        }
    }
    return std::nullopt;
}

std::string_view displayName(BlendMode mode) { return info(mode).label; }

std::span<const BlendMode> blendModesInMenuOrder() { return kMenuOrder; }

int blendModeGroup(BlendMode mode) { return info(mode).group; }

} // namespace wabcut
