// 0.2: Premiere's video effect library on the GPU (the family shaders, render/EffectLibrary.h) against its CPU twins
// (render/reference/LibraryEffects.h), data-driven: each case is an effect id with the registry's defaults and a few
// settings changed (so the evaluator's parameter order, EffectLibrary.cpp, is checked against the descriptors too).
// One test case per family keeps the suite fast; the effects that read a second picture (Echo, the text effects,
// Difference Matte, Image Matte Key, Gradient Wipe's layer) get it as the evaluator gives it, a composite.
// Rendered pairs land in test-output/render/library/.

#include "EffectsReference.h"
#include "RenderTestSupport.h"

#include <wabcut/model/Effects.h>
#include <wabcut/render/EffectLibrary.h>
#include <wabcut/render/reference/LibraryEffects.h>

#include <QDir>
#include <QElapsedTimer>
#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;
namespace ref = wabcut::render::reference;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

std::uint8_t byte(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

/// A busy, colourful opaque picture: hue sweep across, lightness down, checks and lines.
std::array<std::uint8_t, 4> colorful(int x, int y) {
    const double h = x / double(kW);
    const double l = 0.15 + 0.75 * y / double(kH);
    const double r = std::clamp(std::abs(h * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    const double g = std::clamp(2.0 - std::abs(h * 6.0 - 2.0), 0.0, 1.0);
    const double b = std::clamp(2.0 - std::abs(h * 6.0 - 4.0), 0.0, 1.0);
    const double s = ((x / 24) + (y / 24)) % 2 == 0 ? 0.85 : 0.45;
    const auto ch = [&](double c) { return byte(l + (c - 0.5) * s * (1.0 - std::abs(2.0 * l - 1.0))); };
    if (x % 53 == 5 || y % 41 == 7) {
        return {byte(0.95), byte(0.95), byte(0.92), 255};
    }
    return {ch(r), ch(g), ch(b), 255};
}

/// A card with soft transparent edges, for the alpha effects.
std::array<std::uint8_t, 4> card(int x, int y) {
    const double dx = std::max(0.0, std::abs(x + 0.5 - kW / 2.0) - 90.0);
    const double dy = std::max(0.0, std::abs(y + 0.5 - kH / 2.0) - 45.0);
    const double a = std::clamp(1.0 - std::hypot(dx, dy) / 18.0, 0.0, 1.0);
    const auto c = colorful(x, y);
    return {c[0], c[1], c[2], byte(a)};
}

using Picture = std::array<std::uint8_t, 4> (*)(int, int);

/// The effect `id` with the registry's defaults and `settings`, in EffectLibrary.h's parameter order.
LayerEffect libraryEffectOf(std::string_view id, const std::vector<std::pair<std::string_view, double>>& settings) {
    const EffectLibraryEntry* entry = findLibraryEffect(id);
    const EffectDescriptor* d = EffectRegistry::builtin().find(id);
    REQUIRE(entry);
    REQUIRE(d);
    LayerEffect e;
    e.type = EffectType::Library;
    e.op = static_cast<std::uint16_t>(entry->op);
    for (std::size_t k = 0; k < entry->keys.size() && !entry->keys[k].empty(); ++k) {
        const EffectParamInfo* info = d->param(entry->keys[k]);
        REQUIRE(info);
        e.params[k] = info->defaultValue;
    }
    for (const auto& [key, value] : settings) {
        if (key == "time") {
            e.params[kLibraryTimeParam] = value;
            continue;
        }
        if (key == "#10") { // the evaluator's extra (Strobe Light: on)
            e.params[10] = value;
            continue;
        }
        const auto it = std::find(entry->keys.begin(), entry->keys.end(), key);
        REQUIRE(it != entry->keys.end());
        e.params[static_cast<std::size_t>(it - entry->keys.begin())] = value;
    }
    return e;
}

/// The CPU twin of a stack of effects (library effects and the I.2 ones EffectsReference implements). `second`: the
/// library effects' second picture (LayerEffect::matte on the GPU), null when they have none.
LinearImage referenceStack(LinearImage image, const std::vector<LayerEffect>& effects, const LinearImage* second = nullptr) {
    for (const LayerEffect& e : effects) {
        if (e.type == EffectType::Library) {
            ref::LibraryParams params{};
            std::copy(e.params.begin(), e.params.end(), params.begin());
            const ref::Image other = second ? toReference(*second) : ref::Image{};
            image = fromReference(ref::libraryEffect(toReference(image), static_cast<ref::LibraryEffect>(e.op), params,
                                                     second ? &other : nullptr));
        } else {
            image = referenceEffect(image, e);
        }
    }
    return image;
}

struct Case {
    std::string name;
    Picture picture;
    std::vector<LayerEffect> effects;
    /// The second picture of the library effects (echoes, text, a difference or gradient layer, a matte image).
    Picture second = nullptr;
};

Case one(std::string name, Picture picture, std::string_view id,
         std::vector<std::pair<std::string_view, double>> settings = {}) {
    return {std::move(name), picture, {libraryEffectOf(id, settings)}};
}

Case withSecond(std::string name, Picture picture, Picture second, std::string_view id,
                std::vector<std::pair<std::string_view, double>> settings = {}) {
    return {std::move(name), picture, {libraryEffectOf(id, settings)}, second};
}

/// Second pictures: a soft "echo" of the colourful picture shifted right, a text-like card (white bars on a
/// translucent box), and a slightly changed copy (a difference layer: equal on the left half).
std::array<std::uint8_t, 4> shifted(int x, int y) {
    auto c = colorful(std::max(0, x - 23), y);
    return {static_cast<std::uint8_t>(c[0] * 3 / 4), static_cast<std::uint8_t>(c[1] * 3 / 4),
            static_cast<std::uint8_t>(c[2] * 3 / 4), 191};
}

std::array<std::uint8_t, 4> textCard(int x, int y) {
    const bool box = x >= 60 && x < 260 && y >= 120 && y < 160;
    if (!box) {
        return {0, 0, 0, 0};
    }
    const bool glyph = (x / 6) % 3 != 0 && y >= 128 && y < 152;
    return glyph ? std::array<std::uint8_t, 4>{255, 255, 255, 255} : std::array<std::uint8_t, 4>{0, 0, 0, 128};
}

std::array<std::uint8_t, 4> nearlySame(int x, int y) {
    if (x < kW / 2) {
        return colorful(x, y);
    }
    return colorful(kW - 1 - x, y); // mirrored on the right: differs there
}

LayerEffect exposure(double stops) {
    LayerEffect e;
    e.type = EffectType::BasicCorrection;
    e.params[0] = stops;
    e.params[2] = 100.0; // saturation unchanged
    return e;
}

std::vector<Case> colorCases() {
    std::vector<Case> cases = {
        one("brightness_contrast", colorful, "wabcut.color.brightnessContrast", {{"brightness", 30.0}, {"contrast", 40.0}}),
        one("brightness_contrast_flat", colorful, "wabcut.color.brightnessContrast", {{"brightness", -50.0}, {"contrast", -60.0}}),
        one("levels", colorful, "wabcut.adjust.levels",
            {{"rgb.blackInput", 20.0}, {"rgb.whiteInput", 230.0}, {"rgb.gamma", 150.0}, {"red.blackOutput", 30.0},
             {"blue.whiteOutput", 200.0}, {"green.gamma", 70.0}}),
        one("procamp", colorful, "wabcut.adjust.procAmp",
            {{"brightness", 10.0}, {"contrast", 120.0}, {"hue", 40.0}, {"saturation", 150.0}}),
        one("procamp_split", colorful, "wabcut.adjust.procAmp",
            {{"hue", -70.0}, {"saturation", 40.0}, {"splitScreen", 1.0}, {"splitPercent", 0.6}}),
        one("extract_soft", colorful, "wabcut.adjust.extract", {{"blackInput", 60.0}, {"whiteInput", 200.0}, {"softness", 30.0}}),
        one("extract_invert", colorful, "wabcut.adjust.extract", {{"blackInput", 80.0}, {"whiteInput", 180.0}, {"invert", 1.0}}),
        one("invert_rgb", colorful, "wabcut.channel.invert"),
        one("invert_green", colorful, "wabcut.channel.invert", {{"channel", 2.0}}),
        one("invert_hls_blend", colorful, "wabcut.channel.invert", {{"channel", 4.0}, {"blend", 0.3}}),
        one("invert_saturation", colorful, "wabcut.channel.invert", {{"channel", 7.0}}),
        one("invert_yiq", colorful, "wabcut.channel.invert", {{"channel", 8.0}}),
        one("invert_quadrature", colorful, "wabcut.channel.invert", {{"channel", 11.0}}),
        one("invert_alpha", card, "wabcut.channel.invert", {{"channel", 12.0}}),
        one("tint", colorful, "wabcut.color.tint",
            {{"mapBlack.r", 0.1}, {"mapBlack.b", 0.3}, {"mapWhite.g", 0.9}, {"mapWhite.b", 0.6}, {"amount", 0.8}}),
        one("change_color_hue", colorful, "wabcut.color.changeColor",
            {{"hueTransform", 90.0}, {"lightnessTransform", 10.0}, {"saturationTransform", 20.0}, {"color.r", 0.9},
             {"color.g", 0.2}, {"color.b", 0.7}, {"tolerance", 0.3}, {"softness", 0.1}}),
        one("change_color_rgb_mask", colorful, "wabcut.color.changeColor",
            {{"view", 1.0}, {"match", 0.0}, {"color.r", 0.5}, {"color.g", 0.5}, {"color.b", 0.8}, {"tolerance", 0.25},
             {"softness", 0.2}}),
        one("change_color_chroma", colorful, "wabcut.color.changeColor",
            {{"hueTransform", -45.0}, {"match", 2.0}, {"color.r", 0.2}, {"color.g", 0.8}, {"color.b", 0.3}, {"tolerance", 0.2},
             {"softness", 0.15}, {"invert", 1.0}}),
        one("change_to_color_set", colorful, "wabcut.color.changeToColor",
            {{"from.r", 0.85}, {"from.g", 0.3}, {"from.b", 0.5}, {"to.r", 0.2}, {"to.g", 0.8}, {"to.b", 0.3}, {"change", 3.0},
             {"tolerance.hue", 0.2}, {"tolerance.lightness", 0.6}, {"tolerance.saturation", 0.6}, {"softness", 0.4}}),
        one("change_to_color_transform", colorful, "wabcut.color.changeToColor",
            {{"from.r", 0.3}, {"from.g", 0.4}, {"from.b", 0.9}, {"to.r", 0.9}, {"to.g", 0.6}, {"to.b", 0.1}, {"change", 1.0},
             {"changeBy", 1.0}, {"tolerance.hue", 0.15}, {"softness", 0.3}}),
        one("change_to_color_matte", colorful, "wabcut.color.changeToColor", {{"tolerance.hue", 0.12}, {"viewMatte", 1.0}}),
        one("leave_color_rgb", colorful, "wabcut.color.leaveColor",
            {{"amount", 1.0}, {"color.r", 0.8}, {"color.g", 0.3}, {"color.b", 0.75}, {"tolerance", 0.35}, {"softness", 0.1}}),
        one("leave_color_hue", colorful, "wabcut.color.leaveColor",
            {{"amount", 0.8}, {"color.r", 0.1}, {"color.g", 0.6}, {"color.b", 0.9}, {"tolerance", 0.2}, {"softness", 0.15},
             {"match", 1.0}}),
        one("color_balance", colorful, "wabcut.color.balance",
            {{"shadow.red", 40.0}, {"midtone.green", -30.0}, {"highlight.blue", 50.0}, {"highlight.red", -20.0}}),
        one("color_balance_preserve", colorful, "wabcut.color.balance",
            {{"shadow.blue", 60.0}, {"midtone.red", 45.0}, {"preserveLuminosity", 1.0}}),
        {"video_limiter", colorful,
         {exposure(1.0), libraryEffectOf("wabcut.color.videoLimiter", {{"clipLevel", 2.0}, {"compression", 3.0}})}},
        {"video_limiter_warning", colorful,
         {exposure(0.8), libraryEffectOf("wabcut.color.videoLimiter", {{"clipLevel", 0.0}, {"gamutWarning", 1.0}})}},
        one("black_white", colorful, "wabcut.image.blackWhite"),
        one("color_pass", colorful, "wabcut.image.colorPass", {{"color.r", 0.9}, {"color.g", 0.35}, {"color.b", 0.8}, {"similarity", 30.0}}),
        one("color_pass_reverse", colorful, "wabcut.image.colorPass",
            {{"color.r", 0.3}, {"color.g", 0.8}, {"color.b", 0.4}, {"similarity", 25.0}, {"reverse", 1.0}}),
        one("color_replace", colorful, "wabcut.image.colorReplace",
            {{"target.r", 0.9}, {"target.g", 0.35}, {"target.b", 0.8}, {"replace.r", 0.1}, {"replace.g", 0.6}, {"replace.b", 0.9},
             {"similarity", 35.0}}),
        one("color_replace_solid", colorful, "wabcut.image.colorReplace",
            {{"target.r", 0.3}, {"target.g", 0.8}, {"target.b", 0.4}, {"similarity", 30.0}, {"solid", 1.0}}),
        one("gamma_dark", colorful, "wabcut.image.gammaCorrection", {{"gamma", 20.0}}),
        one("gamma_light", colorful, "wabcut.image.gammaCorrection", {{"gamma", 5.0}}),
        one("posterize", colorful, "wabcut.stylize.posterize", {{"level", 4.0}}),
        one("solarize", colorful, "wabcut.stylize.solarize", {{"threshold", 0.6}}),
        one("threshold", colorful, "wabcut.stylize.threshold", {{"level", 100.0}}),
        one("strobe_copy_blend", colorful, "wabcut.stylize.strobeLight", {{"#10", 1.0}, {"blend", 0.5}, {"color.g", 0.4}}),
        one("strobe_add", colorful, "wabcut.stylize.strobeLight", {{"#10", 1.0}, {"operator", 1.0}, {"color.r", 0.3}}),
        one("strobe_xor", colorful, "wabcut.stylize.strobeLight", {{"#10", 1.0}, {"operator", 7.0}, {"color.b", 0.2}}),
        one("strobe_darken", colorful, "wabcut.stylize.strobeLight",
            {{"#10", 1.0}, {"operator", 9.0}, {"color.r", 0.5}, {"color.g", 0.5}, {"color.b", 0.5}}),
        one("strobe_screen", colorful, "wabcut.stylize.strobeLight", {{"#10", 1.0}, {"operator", 12.0}, {"color.r", 0.7}}),
        one("strobe_transparent", card, "wabcut.stylize.strobeLight", {{"#10", 1.0}, {"strobe", 1.0}, {"blend", 0.25}}),
        one("strobe_off", colorful, "wabcut.stylize.strobeLight", {{"#10", 0.0}}),
        one("alpha_adjust_invert", card, "wabcut.key.alphaAdjust", {{"opacity", 0.6}, {"invertAlpha", 1.0}}),
        one("alpha_adjust_mask", card, "wabcut.key.alphaAdjust", {{"maskOnly", 1.0}}),
        one("alpha_adjust_ignore", card, "wabcut.key.alphaAdjust", {{"ignoreAlpha", 1.0}, {"opacity", 0.8}}),
        one("luma_key", colorful, "wabcut.key.luma", {{"threshold", 0.6}, {"cutoff", 0.2}}),
        one("luma_key_lights", colorful, "wabcut.key.luma", {{"threshold", 0.3}, {"cutoff", 0.75}}),
        one("noise_color", colorful, "wabcut.noise.noise", {{"amount", 0.3}, {"time", 1.234}}),
        one("noise_mono_wrap", colorful, "wabcut.noise.noise", {{"amount", 0.5}, {"colorNoise", 0.0}, {"clip", 0.0}, {"time", 7.5}}),
    };
    return cases;
}

std::vector<Case> distortCases() {
    return {
        one("corner_pin", colorful, "wabcut.distort.cornerPin",
            {{"upperLeft.x", 0.1}, {"upperLeft.y", 0.05}, {"upperRight.x", 0.85}, {"upperRight.y", 0.15},
             {"lowerLeft.x", 0.05}, {"lowerLeft.y", 0.9}, {"lowerRight.x", 0.95}, {"lowerRight.y", 0.8}}),
        one("corner_pin_affine", colorful, "wabcut.distort.cornerPin",
            {{"upperLeft.x", 0.2}, {"upperRight.x", 1.2}, {"lowerLeft.x", 0.0}, {"lowerRight.x", 1.0}}),
        one("lens_distortion_convex", colorful, "wabcut.distort.lensDistortion", {{"curvature", 60.0}}),
        one("lens_distortion_concave_fill", colorful, "wabcut.distort.lensDistortion",
            {{"curvature", -70.0}, {"horizontalDecentering", 20.0}, {"verticalPrism", 30.0}, {"fillAlpha", 0.0},
             {"fillColor.g", 0.2}}),
        one("magnify_circle", colorful, "wabcut.distort.magnify",
            {{"center.x", 0.4}, {"center.y", 0.55}, {"magnification", 250.0}, {"size", 50.0}, {"feather", 10.0}}),
        one("magnify_square_screen", colorful, "wabcut.distort.magnify",
            {{"shape", 1.0}, {"magnification", 180.0}, {"size", 40.0}, {"blendingMode", 4.0}, {"opacity", 0.8},
             {"link", 2.0}, {"feather", 4.0}}),
        one("mirror", colorful, "wabcut.distort.mirror", {{"center.x", 0.45}, {"angle", 20.0}}),
        one("offset", colorful, "wabcut.distort.offset", {{"center.x", 0.7}, {"center.y", 0.35}, {"blend", 0.2}}),
        one("spherize", colorful, "wabcut.distort.spherize", {{"radius", 70.0}, {"center.x", 0.55}}),
        one("twirl", colorful, "wabcut.distort.twirl", {{"angle", 160.0}, {"radius", 35.0}}),
        one("wave_warp_sine", colorful, "wabcut.distort.waveWarp", {{"height", 8.0}, {"width", 50.0}, {"time", 0.4}}),
        one("wave_warp_triangle_pinned", colorful, "wabcut.distort.waveWarp",
            {{"waveType", 2.0}, {"height", 10.0}, {"direction", 30.0}, {"pinning", 1.0}, {"phase", 45.0}, {"time", 1.2}}),
        one("turbulent_displace", colorful, "wabcut.distort.turbulentDisplace",
            {{"amount", 20.0}, {"size", 40.0}, {"complexity", 2.5}, {"evolution", 90.0}}),
        one("turbulent_twist_unpinned", colorful, "wabcut.distort.turbulentDisplace",
            {{"displacement", 2.0}, {"amount", 60.0}, {"size", 60.0}, {"pinning", 0.0}, {"seed", 7.0}}),
        one("transform", colorful, "wabcut.distort.transform",
            {{"position.x", 0.55}, {"position.y", 0.45}, {"scaleHeight", 80.0}, {"skew", 15.0}, {"skewAxis", 30.0},
             {"rotation", 12.0}, {"opacity", 0.9}}),
        one("transform_nonuniform", colorful, "wabcut.distort.transform",
            {{"uniformScale", 0.0}, {"scaleHeight", 70.0}, {"scaleWidth", 120.0}, {"anchor.x", 0.3}}),
        one("horizontal_flip", colorful, "wabcut.transform.horizontalFlip"),
        one("vertical_flip", card, "wabcut.transform.verticalFlip"),
        one("edge_feather", colorful, "wabcut.transform.edgeFeather", {{"amount", 30.0}}),
        one("replicate", colorful, "wabcut.stylize.replicate", {{"count", 3.0}}),
        one("basic_3d", colorful, "wabcut.perspective.basic3d", {{"swivel", 30.0}, {"tilt", -20.0}, {"distance", 20.0}}),
        one("basic_3d_back_specular", colorful, "wabcut.perspective.basic3d", {{"swivel", 140.0}, {"specular", 1.0}}),
        // 0.2 Animations' Fold: Basic 3D, then a Transform moving the hinge edge back (playback transitionAnimationAt).
        Case{"fold_chain", colorful,
             {libraryEffectOf("wabcut.perspective.basic3d", {{"tilt", 40.0}}),
              libraryEffectOf("wabcut.distort.transform", {{"position.y", 0.4}})}},
        // Thickness 0.16: the band's edge (14.4 px) off the pixel centres, where float and double would round apart.
        one("bevel_edges", colorful, "wabcut.perspective.bevelEdges", {{"thickness", 0.16}, {"intensity", 0.5}}),
        one("directional_blur", colorful, "wabcut.blur.directional", {{"direction", 60.0}, {"length", 18.0}}),
        one("camera_blur", colorful, "wabcut.blur.camera", {{"percent", 15.0}}),
    };
}

std::vector<Case> stylizeCases() {
    return {
        one("unsharp_mask", colorful, "wabcut.blur.unsharpMask", {{"amount", 120.0}, {"radius", 2.0}}),
        one("unsharp_mask_threshold", colorful, "wabcut.blur.unsharpMask", {{"amount", 200.0}, {"radius", 3.0}, {"threshold", 8.0}}),
        one("emboss", colorful, "wabcut.stylize.emboss", {{"relief", 2.0}, {"contrast", 200.0}}),
        one("color_emboss", colorful, "wabcut.stylize.colorEmboss", {{"direction", 120.0}, {"relief", 3.0}, {"blend", 0.2}}),
        one("find_edges", colorful, "wabcut.stylize.findEdges"),
        one("find_edges_invert", colorful, "wabcut.stylize.findEdges", {{"invert", 1.0}, {"blend", 0.3}}),
        one("convolution_kernel_sharpen", colorful, "wabcut.adjust.convolutionKernel",
            {{"m12", -1.0}, {"m21", -1.0}, {"m22", 5.0}, {"m23", -1.0}, {"m32", -1.0}}),
        one("convolution_kernel_emboss", colorful, "wabcut.adjust.convolutionKernel",
            {{"m11", -1.0}, {"m12", -1.0}, {"m21", -1.0}, {"m22", 0.0}, {"m23", 1.0}, {"m32", 1.0}, {"m33", 1.0},
             {"offset", 128.0}}),
        one("convolution_kernel_blur_alpha", card, "wabcut.adjust.convolutionKernel",
            {{"m11", 1.0}, {"m12", 2.0}, {"m13", 1.0}, {"m21", 2.0}, {"m22", 4.0}, {"m23", 2.0}, {"m31", 1.0}, {"m32", 2.0},
             {"m33", 1.0}, {"scale", 16.0}, {"processAlpha", 1.0}}),
        one("median", colorful, "wabcut.noise.median", {{"radius", 2.0}}),
        one("dust_scratches", colorful, "wabcut.noise.dustScratches", {{"radius", 2.0}, {"threshold", 20.0}}),
        one("alpha_glow", card, "wabcut.stylize.alphaGlow", {{"glow", 12.0}, {"startColor.b", 0.2}}),
        one("alpha_glow_end_color", card, "wabcut.stylize.alphaGlow",
            {{"glow", 8.0}, {"useEndColor", 1.0}, {"fadeOut", 0.0}, {"brightness", 200.0}}),
        one("bevel_alpha", card, "wabcut.perspective.bevelAlpha", {{"thickness", 4.0}, {"intensity", 0.7}}),
        one("radial_shadow", card, "wabcut.perspective.radialShadow", {{"distance", 25.0}, {"opacity", 0.7}, {"softness", 6.0}}),
        one("radial_shadow_glass_only", card, "wabcut.perspective.radialShadow",
            {{"light.x", 0.2}, {"light.y", 0.3}, {"render", 1.0}, {"shadowOnly", 1.0}}),
        one("roughen_edges", card, "wabcut.stylize.roughenEdges", {{"border", 10.0}, {"scale", 30.0}}),
        one("roughen_edges_color", card, "wabcut.stylize.roughenEdges", {{"edgeType", 1.0}, {"border", 12.0}, {"scale", 25.0}}),
        one("color_key", colorful, "wabcut.key.colorKey",
            {{"keyColor.r", 0.3}, {"keyColor.g", 0.6}, {"keyColor.b", 0.8}, {"tolerance", 60.0}}),
        one("color_key_thin_feather", colorful, "wabcut.key.colorKey",
            {{"keyColor.r", 0.3}, {"keyColor.g", 0.6}, {"keyColor.b", 0.8}, {"tolerance", 60.0}, {"edgeThin", 2.0},
             {"edgeFeather", 2.0}}),
        one("color_key_grow", colorful, "wabcut.key.colorKey",
            {{"keyColor.r", 0.8}, {"keyColor.g", 0.5}, {"keyColor.b", 0.2}, {"tolerance", 70.0}, {"edgeThin", -2.0}}),
    };
}

std::vector<Case> generateCases() {
    return {
        one("four_color_gradient", colorful, "wabcut.generate.fourColorGradient"),
        one("four_color_gradient_multiply", colorful, "wabcut.generate.fourColorGradient",
            {{"blend", 50.0}, {"opacity", 0.7}, {"blendingMode", 3.0}}),
        one("ramp_linear", colorful, "wabcut.generate.ramp", {{"blend", 0.25}}),
        one("ramp_radial", colorful, "wabcut.generate.ramp",
            {{"shape", 1.0}, {"start.x", 0.4}, {"start.y", 0.5}, {"end.x", 0.9}, {"end.y", 0.9}, {"startColor.r", 1.0}}),
        one("circle", colorful, "wabcut.generate.circle", {{"radius", 50.0}, {"blendingMode", 1.0}}),
        one("circle_ring_feather", colorful, "wabcut.generate.circle",
            {{"radius", 60.0}, {"edge", 2.0}, {"thickness", 15.0}, {"featherOuter", 6.0}, {"featherInner", 3.0},
             {"blendingMode", 4.0}, {"color.b", 0.2}}),
        one("circle_stencil", colorful, "wabcut.generate.circle", {{"radius", 70.0}, {"blendingMode", 9.0}}),
        one("grid", colorful, "wabcut.generate.grid", {{"blendingMode", 1.0}, {"border", 4.0}}),
        one("grid_sliders_feather", colorful, "wabcut.generate.grid",
            {{"sizeFrom", 2.0}, {"width", 40.0}, {"height", 25.0}, {"border", 6.0}, {"featherWidth", 3.0}, {"invert", 1.0}}),
        one("checkerboard", colorful, "wabcut.generate.checkerboard", {{"width", 30.0}, {"blendingMode", 6.0}}),
        one("lens_flare", colorful, "wabcut.generate.lensFlare", {{"center.x", 0.3}, {"center.y", 0.3}}),
        one("lens_flare_105", colorful, "wabcut.generate.lensFlare", {{"lensType", 2.0}, {"brightness", 150.0}, {"blend", 0.3}}),
        one("block_dissolve", colorful, "wabcut.transition.blockDissolve", {{"completion", 0.4}, {"blockWidth", 8.0}, {"blockHeight", 6.0}}),
        one("gradient_wipe_self", colorful, "wabcut.transition.gradientWipe", {{"completion", 0.45}, {"softness", 0.1}}),
        one("gradient_wipe_inverted", colorful, "wabcut.transition.gradientWipe", {{"completion", 0.3}, {"softness", 0.2}, {"invert", 1.0}}),
        one("linear_wipe", colorful, "wabcut.transition.linearWipe", {{"completion", 0.4}, {"feather", 20.0}}),
        one("linear_wipe_angle", colorful, "wabcut.transition.linearWipe", {{"completion", 0.6}, {"angle", 210.0}}),
        one("radial_wipe_effect", colorful, "wabcut.transition.radialWipe", {{"completion", 0.35}, {"feather", 6.0}}),
        one("radial_wipe_both", colorful, "wabcut.transition.radialWipe", {{"completion", 0.6}, {"wipe", 2.0}, {"startAngle", 45.0}}),
        one("venetian_blinds_effect", colorful, "wabcut.transition.venetianBlinds", {{"completion", 0.4}, {"width", 20.0}}),
        one("venetian_blinds_effect_angle", colorful, "wabcut.transition.venetianBlinds",
            {{"completion", 0.55}, {"direction", 90.0}, {"width", 24.0}, {"feather", 4.0}}),
    };
}

/// The effects that read a second picture (the evaluator prepares it: Evaluate.cpp).
std::vector<Case> secondPictureCases() {
    return {
        withSecond("echo_add", colorful, shifted, "wabcut.time.echo", {{"startingIntensity", 0.7}}),
        withSecond("echo_maximum", colorful, shifted, "wabcut.time.echo", {{"operator", 1.0}}),
        withSecond("echo_minimum", colorful, shifted, "wabcut.time.echo", {{"operator", 2.0}}),
        withSecond("echo_screen", colorful, shifted, "wabcut.time.echo", {{"operator", 3.0}, {"startingIntensity", 0.8}}),
        withSecond("echo_in_back", card, shifted, "wabcut.time.echo", {{"operator", 4.0}}),
        withSecond("echo_in_front", card, shifted, "wabcut.time.echo", {{"operator", 5.0}}),
        withSecond("simple_text", colorful, textCard, "wabcut.video.simpleText"),
        withSecond("timecode_over_card", card, textCard, "wabcut.video.timecode"),
        withSecond("difference_matte", colorful, nearlySame, "wabcut.key.differenceMatte",
                   {{"differenceLayer", 1.0}, {"tolerance", 0.05}, {"softness", 0.1}}),
        withSecond("difference_matte_view", colorful, nearlySame, "wabcut.key.differenceMatte",
                   {{"differenceLayer", 1.0}, {"view", 2.0}, {"tolerance", 0.1}}),
        one("difference_matte_no_layer", colorful, "wabcut.key.differenceMatte"),
        withSecond("image_matte_alpha", colorful, card, "wabcut.key.imageMatte", {{"compositeUsing", 0.0}}),
        withSecond("image_matte_luma_reverse", colorful, shifted, "wabcut.key.imageMatte", {{"reverse", 1.0}}),
        one("image_matte_none", colorful, "wabcut.key.imageMatte"),
        withSecond("gradient_wipe_layer", colorful, nearlySame, "wabcut.transition.gradientWipe",
                   {{"completion", 0.5}, {"softness", 0.15}, {"gradientLayer", 1.0}}),
    };
}

void runCases(const std::vector<Case>& cases) {
    auto renderer = makeRenderer(OutputRole::Export);
    const QString dir = outputDir() + QStringLiteral("/library");
    QDir().mkpath(dir);
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, c.picture));
            layer.effects = c.effects;
            std::optional<LinearImage> second;
            if (c.second) {
                // The evaluator's second picture: a composite on a canvas like the layer's (Evaluate.cpp).
                const std::shared_ptr<const PreparedComposite> picture =
                    compositeOf({kW, kH}, {layerOf(makeRgbaFrame(kW, kH, c.second))});
                for (LayerEffect& e : layer.effects) {
                    if (e.type == EffectType::Library) {
                        e.matte = picture;
                    }
                }
                second = linearFromRgba8(kW, kH, c.second);
            }
            auto image = renderer->render(compositeOf({kW, kH}, {layer}));
            REQUIRE(image);
            const LinearImage expected =
                referenceStack(linearFromRgba8(kW, kH, c.picture), c.effects, second ? &*second : nullptr);
            const std::vector<std::uint8_t> rgb = encodeOverBlack(expected, outputTransformFor(ColorPipeline{}, OutputRole::Export));
            savePng(*image, QStringLiteral("library/%1.png").arg(QString::fromStdString(c.name)));
            QImage(rgb.data(), kW, kH, kW * 3, QImage::Format_RGB888)
                .save(dir + QStringLiteral("/%1.reference.png").arg(QString::fromStdString(c.name)));
            const RenderMatch m = compareToReference(*image, rgb);
            WARN(c.name << ": PSNR " << m.psnr << " dB, " << m.mismatched * 100.0 << " % of pixels off");
            // A black-and-white result (Extract, Threshold) flips a few pixels sitting on its threshold between the
            // GPU's floats and the reference's doubles: those are judged by how few pixels differ.
            const bool binary = c.name.starts_with("extract") || c.name.starts_with("threshold");
            CHECK(m.psnr >= (binary ? 30.0 : 40.0));
            CHECK(m.mismatched <= (binary ? 0.001 : 0.003));
        }
    }
}

} // namespace

TEST_CASE("Effect library: the Color family matches its CPU reference (0.2)", "[render][gpu][library]") {
    runCases(colorCases());
}

TEST_CASE("Effect library: the Distort family matches its CPU reference (0.2)", "[render][gpu][library]") {
    runCases(distortCases());
}

TEST_CASE("Effect library: the Stylize family matches its CPU reference (0.2)", "[render][gpu][library]") {
    runCases(stylizeCases());
}

TEST_CASE("Effect library: the Generate family matches its CPU reference (0.2)", "[render][gpu][library]") {
    runCases(generateCases());
}

TEST_CASE("Effect library: echoes text mattes and other layers as second pictures match the reference (0.2)",
          "[render][gpu][library]") {
    runCases(secondPictureCases());
}

TEST_CASE("Effect library: 1080p cost of a few heavy effects (0.2)", "[render][gpu][library][perf]") {
    // Each effect alone on a 1080p layer: render + readback, best of a few, reported (not a gate: the readback and the
    // machine dominate). The multi-pass ones (Unsharp Mask, Alpha Glow, Color Key, Radial Shadow) are among them.
    auto renderer = makeRenderer(OutputRole::Export);
    const auto big = [](int x, int y) { return colorful(x * kW / 1920, y * kH / 1080); };
    const std::vector<std::pair<std::string, LayerEffect>> effects = {
        {"turbulent_displace", libraryEffectOf("wabcut.distort.turbulentDisplace", {{"amount", 40.0}})},
        {"camera_blur", libraryEffectOf("wabcut.blur.camera", {{"percent", 30.0}})},
        {"directional_blur", libraryEffectOf("wabcut.blur.directional", {{"length", 40.0}})},
        {"unsharp_mask", libraryEffectOf("wabcut.blur.unsharpMask", {{"radius", 8.0}})},
        {"median", libraryEffectOf("wabcut.noise.median", {{"radius", 3.0}})},
        {"color_key", libraryEffectOf("wabcut.key.colorKey", {{"tolerance", 50.0}, {"edgeFeather", 4.0}})},
        {"lens_flare", libraryEffectOf("wabcut.generate.lensFlare", {})},
    };
    for (const auto& [name, effect] : effects) {
        CompositeLayer layer = layerOf(makeRgbaFrame(1920, 1080, big));
        layer.effects = {effect};
        qint64 best = std::numeric_limits<qint64>::max();
        for (int i = 0; i < 4; ++i) {
            QElapsedTimer timer;
            timer.start();
            REQUIRE(renderer->render(compositeOf({1920, 1080}, {layer})));
            best = std::min(best, timer.nsecsElapsed());
        }
        WARN(name << " at 1080p: " << best / 1.0e6 << " ms per frame incl. readback");
        CHECK(best < 1'000'000'000); // a second would be a hang, not a cost
    }
}

TEST_CASE("Effect library: every library effect has a descriptor with its keys (0.2)", "[render][library]") {
    for (const EffectLibraryEntry& entry : effectLibrary()) {
        INFO(entry.id);
        const EffectDescriptor* d = EffectRegistry::builtin().find(entry.id);
        REQUIRE(d);
        CHECK(d->kind == EffectKind::Video);
        std::size_t keys = 0;
        for (const std::string_view key : entry.keys) {
            if (key.empty()) {
                break;
            }
            CHECK(d->param(key));
            ++keys;
        }
        CHECK(keys == d->params.size()); // every parameter reaches the compositor
        CHECK(familyOf(entry.op) != FxFamily::Core);
    }
}
