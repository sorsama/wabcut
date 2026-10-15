#pragma once

// Wabcut effects in FCP7 XML (X.1). Three have FCP7 filters Premiere Pro and Resolve apply themselves: Gaussian
// Blur, Crop and Drop Shadow, written and read with FCP7's ids and units. Every other effect of a clip's stack is
// written as a filter named by its Wabcut id ("wabcut.audio.eq3") with its parameters by key: other editors skip it,
// Wabcut reads it back exactly.

#include <wabcut/model/Effects.h>

#include <span>
#include <string_view>

namespace wabcut::interchange::detail {

/// One parameter of a native filter: the Wabcut key, FCP7's parameterid, and FCP value = Wabcut value * scale.
struct NativeParam {
    std::string_view key;
    std::string_view fcpId;
    std::string_view fcpName;
    double scale = 1.0;
};

struct NativeEffect {
    std::string_view wabcut;
    std::string_view fcpId;
    std::string_view fcpName;
    std::string_view category;
    std::string_view type;
    std::span<const NativeParam> params;
    /// Drop Shadow: its colour is one FCP7 colour parameter ("color": alpha, red, green, blue 0..255) for the
    /// Wabcut keys red, green, blue.
    bool shadowColor = false;
};

inline constexpr NativeParam kBlurParams[] = {{"radius", "radius", "Radius", 1.0}};
inline constexpr NativeParam kCropParams[] = {{"left", "left", "Left", 100.0},
                                              {"right", "right", "Right", 100.0},
                                              {"top", "top", "Top", 100.0},
                                              {"bottom", "bottom", "Bottom", 100.0},
                                              {"feather", "edgefeather", "Edge Feather", 1.0}};
inline constexpr NativeParam kShadowParams[] = {{"distance", "offset", "Offset", 1.0},
                                                {"angle", "angle", "Angle", 1.0},
                                                {"softness", "softness", "Softness", 1.0},
                                                {"opacity", "opacity", "Opacity", 100.0}};

inline constexpr NativeEffect kNativeEffects[] = {
    {"wabcut.blur.gaussian", "GaussianBlur", "Gaussian Blur", "Blur", "filter", kBlurParams, false},
    {"wabcut.crop", "crop", "Crop", "motion", "motion", kCropParams, false},
    {"wabcut.dropShadow", "dropshadow", "Drop Shadow", "motion", "motion", kShadowParams, true},
};

/// The native filter of a Wabcut effect, or null.
inline const NativeEffect* nativeEffectFor(std::string_view wabcutId) {
    for (const NativeEffect& e : kNativeEffects) {
        if (e.wabcut == wabcutId) {
            return &e;
        }
    }
    return nullptr;
}

/// The Wabcut effect of an FCP7 filter (by effectid or name, case-insensitive), or null.
const NativeEffect* nativeEffectFromFcp(std::string_view effectId, std::string_view name);

} // namespace wabcut::interchange::detail
