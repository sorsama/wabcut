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
#include <wabcut/render/FaceMap.h>
#include <wabcut/render/Mask.h>
#include <wabcut/render/reference/LibraryEffects.h>
#include <wabcut/render/reference/LibraryGeometry.h>

#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
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
        if (key.size() > 1 && key[0] == '#') { // a parameter the evaluator fills (Strobe Light's #10: on; AI Eye Contact's baked eyes, #6 and #8..)
            e.params[static_cast<std::size_t>(std::stoul(std::string(key.substr(1))))] = value;
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

/// A depth map for AI Refocus: premultiplied black whose alpha is the distance (0 = nearest .. 1 = farthest) - far on the left
/// growing nearer to the right, with a near disc in the middle.
std::array<std::uint8_t, 4> depthCard(int x, int y) {
    const double ramp = 1.0 - (x + 0.5) / kW * 0.8;
    const double d = std::hypot(x + 0.5 - kW * 0.5, y + 0.5 - kH * 0.5);
    const double distance = d < 40.0 ? 0.1 : ramp;
    return {0, 0, 0, byte(distance)};
}

/// 0.7 (Skin Smoothing, AI Face Retouch): a face in a bluish room - an ellipse of shaded skin with pore texture, two spots, a hair band, eyes
/// (whites and an iris), a mouth (yellowish teeth over a dark tongue) - and the face map that goes with it (R skin, G mouth, B eyes).
bool inFaceEllipse(int x, int y) {
    const double ex = (x + 0.5 - 160.0) / 90.0;
    const double ey = (y + 0.5 - 90.0) / 70.0;
    return ex * ex + ey * ey < 1.0;
}
bool inEyes(int x, int y) { return y >= 66 && y < 76 && ((x >= 112 && x < 146) || (x >= 174 && x < 208)); }
bool inMouth(int x, int y) { return x >= 134 && x < 186 && y >= 116 && y < 132; }

std::array<std::uint8_t, 4> skinPortrait(int x, int y) {
    const auto hash = [](int a, int b) {
        std::uint32_t h = static_cast<std::uint32_t>(a) * 73856093u ^ static_cast<std::uint32_t>(b) * 19349663u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return static_cast<double>(h & 0xFFFFu) / 65535.0 - 0.5;
    };
    if (!inFaceEllipse(x, y)) {
        const double t = y / double(kH);
        return {byte(0.22 + 0.1 * t), byte(0.34 + 0.08 * t), byte(0.62 - 0.1 * t), 255};
    }
    if (y < 28) {
        return {byte(0.18), byte(0.12), byte(0.09), 255}; // hair
    }
    if (inEyes(x, y)) {
        const int cx = x < 160 ? 129 : 191;
        return std::hypot(x + 0.5 - cx, y + 0.5 - 71.0) < 4.0 ? std::array<std::uint8_t, 4>{byte(0.25), byte(0.15), byte(0.10), 255}
                                                                : std::array<std::uint8_t, 4>{byte(0.88), byte(0.85), byte(0.82), 255};
    }
    if (inMouth(x, y)) {
        return y < 124 ? std::array<std::uint8_t, 4>{byte(0.86), byte(0.79), byte(0.58), 255}
                       : std::array<std::uint8_t, 4>{byte(0.50), byte(0.15), byte(0.15), 255};
    }
    if ((x >= 118 && x < 121 && y >= 99 && y < 102) || (x >= 198 && x < 201 && y >= 94 && y < 97)) {
        return {byte(0.76), byte(0.45), byte(0.40), 255}; // spots
    }
    const double shade = 1.0 - 0.12 * ((y + 0.5 - 90.0) / 70.0);
    const double pore = 0.03 * hash(x, y);
    return {byte(0.82 * shade + pore), byte(0.64 * shade + pore), byte(0.54 * shade + pore), 255};
}

std::array<std::uint8_t, 4> faceMapCard(int x, int y) {
    const bool eyes = inFaceEllipse(x, y) && inEyes(x, y);
    const bool mouth = inFaceEllipse(x, y) && inMouth(x, y);
    const bool skin = inFaceEllipse(x, y) && y >= 28 && !eyes && !mouth;
    return {static_cast<std::uint8_t>(skin ? 255 : 0), static_cast<std::uint8_t>(mouth ? 255 : 0), static_cast<std::uint8_t>(eyes ? 255 : 0), 255};
}

/// 0.7 AI Eye Contact: an eye drawn analytically (a lens-shaped opening with shaded sclera, an iris with a pupil and a catch-light, skin round it),
/// level and rolled by 8 degrees. The effect's parameters for it are the baked ones (model/EyeContact.h): the eye's frame, its iris and a shift.
constexpr double kEyeCx = 160.0;
constexpr double kEyeCy = 90.0;
constexpr double kEyeHalf = 60.0;
constexpr double kEyeUp = 22.0;
constexpr double kEyeLo = 17.0;
constexpr double kEyeRadius = 14.0;
constexpr double kEyeIrisX = 3.0;
constexpr double kEyeIrisY = 8.0;

std::array<std::uint8_t, 4> eyePixel(int x, int y, double rollDegrees) {
    const double t = rollDegrees * 3.14159265358979 / 180.0;
    const double dx = x + 0.5 - kEyeCx;
    const double dy = y + 0.5 - kEyeCy;
    const double lx = dx * std::cos(t) + dy * std::sin(t);
    const double ly = -dx * std::sin(t) + dy * std::cos(t);
    const double u = lx / kEyeHalf;
    const double k = 1.0 - u * u;
    const double grain = 0.012 * (static_cast<double>((static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u) >> 7 & 0xFF) / 255.0 - 0.5);
    if (!(std::abs(u) < 1.0 && ly > -kEyeUp * k && ly < kEyeLo * k)) {
        const double shade = 1.0 - 0.1 * (y / double(kH));
        return {byte(0.80 * shade + grain), byte(0.62 * shade + grain), byte(0.52 * shade + grain), 255}; // skin
    }
    const double dIris = std::hypot(lx - kEyeIrisX, ly - kEyeIrisY);
    if (std::hypot(lx - kEyeIrisX + 4.0, ly - kEyeIrisY + 4.0) < 1.8) {
        return {byte(0.98), byte(0.98), byte(0.98), 255}; // the catch-light
    }
    if (dIris < 5.0) {
        return {byte(0.04), byte(0.03), byte(0.03), 255}; // the pupil
    }
    if (dIris < kEyeRadius) {
        const double ring = 0.85 + 0.15 * std::sin(dIris * 1.7);
        return {byte(0.30 * ring), byte(0.18 * ring), byte(0.10 * ring), 255};
    }
    const double shade = 0.78 + 0.22 * (ly + kEyeUp * k) / ((kEyeUp + kEyeLo) * k); // darker under the upper lid
    return {byte(0.93 * shade + grain), byte(0.91 * shade + grain), byte(0.89 * shade + grain), 255};
}
std::array<std::uint8_t, 4> eyeCard(int x, int y) { return eyePixel(x, y, 0.0); }
std::array<std::uint8_t, 4> eyeCardRolled(int x, int y) { return eyePixel(x, y, 8.0); }

/// The effect's settings for one baked eye: the shift in the eye's frame (pixels), its roll (degrees), the Show menu.
std::vector<std::pair<std::string_view, double>> eyeSettings(double shiftX, double shiftY, double rollDegrees = 0.0, double view = 0.0, int eyes = 1) {
    static constexpr std::array<std::string_view, 12> kKeys{"#6", "#8", "#9", "#10", "#11", "#12", "#13", "#14", "#15", "#16", "#17", "#18"};
    return {{"view", view},
            {kKeys[0], static_cast<double>(eyes)},
            {kKeys[1], kEyeCx / kW},
            {kKeys[2], kEyeCy / kH},
            {kKeys[3], rollDegrees * 3.14159265358979 / 180.0},
            {kKeys[4], kEyeHalf / kW},
            {kKeys[5], kEyeUp / kW},
            {kKeys[6], kEyeLo / kW},
            {kKeys[7], kEyeIrisX / kW},
            {kKeys[8], kEyeIrisY / kW},
            {kKeys[9], kEyeRadius / kW},
            {kKeys[10], shiftX / kW},
            {kKeys[11], shiftY / kW}};
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
        // 0.7 AI Refocus (Depth Blur): the depth map is the second picture; without one the picture is left as it is.
        withSecond("depth_blur_near_focus", colorful, depthCard, "wabcut.ai.refocus", {{"focus", 15.0}, {"range", 10.0}, {"amount", 60.0}}),
        withSecond("depth_blur_far_focus", colorful, depthCard, "wabcut.ai.refocus", {{"focus", 85.0}, {"range", 20.0}, {"amount", 100.0}}),
        withSecond("depth_blur_background_only", colorful, depthCard, "wabcut.ai.refocus",
                   {{"focus", 10.0}, {"amount", 80.0}, {"foreground", 0.0}}),
        withSecond("depth_blur_view_depth", colorful, depthCard, "wabcut.ai.refocus", {{"view", 1.0}}),
        withSecond("depth_blur_view_blur", colorful, depthCard, "wabcut.ai.refocus", {{"focus", 30.0}, {"view", 2.0}}),
        withSecond("depth_blur_no_blur", colorful, depthCard, "wabcut.ai.refocus", {{"amount", 0.0}}),
        one("depth_blur_no_depth", colorful, "wabcut.ai.refocus"),
        // 0.7 Skin Smoothing (card 100) and AI Face Retouch (card 132): the face map is the second picture; without one the picture is left as it is.
        one("skin_smoothing_default", skinPortrait, "wabcut.retouch.skinSmoothing"),
        one("skin_smoothing_full", skinPortrait, "wabcut.retouch.skinSmoothing", {{"amount", 100.0}, {"detail", 0.0}, {"size", 5.0}}),
        one("skin_smoothing_narrow_tone", skinPortrait, "wabcut.retouch.skinSmoothing",
            {{"amount", 100.0}, {"detail", 20.0}, {"size", 8.0}, {"tone", 140.0}, {"range", 20.0}, {"softness", 30.0}}),
        one("skin_smoothing_mask_view", skinPortrait, "wabcut.retouch.skinSmoothing", {{"view", 1.0}}),
        one("skin_smoothing_zero", skinPortrait, "wabcut.retouch.skinSmoothing", {{"amount", 0.0}}),
        withSecond("face_retouch_default", skinPortrait, faceMapCard, "wabcut.ai.faceRetouch"),
        withSecond("face_retouch_full", skinPortrait, faceMapCard, "wabcut.ai.faceRetouch",
                   {{"smoothing", 100.0}, {"detail", 0.0}, {"blemish", 100.0}, {"teeth", 100.0}, {"eyes", 100.0}}),
        withSecond("face_retouch_blemish_only", skinPortrait, faceMapCard, "wabcut.ai.faceRetouch",
                   {{"smoothing", 0.0}, {"blemish", 100.0}, {"blemishSize", 12.0}, {"teeth", 0.0}, {"eyes", 0.0}}),
        withSecond("face_retouch_teeth_eyes_only", skinPortrait, faceMapCard, "wabcut.ai.faceRetouch",
                   {{"smoothing", 0.0}, {"blemish", 0.0}, {"teeth", 100.0}, {"eyes", 100.0}}),
        withSecond("face_retouch_view_skin", skinPortrait, faceMapCard, "wabcut.ai.faceRetouch", {{"view", 1.0}}),
        withSecond("face_retouch_view_mouth", skinPortrait, faceMapCard, "wabcut.ai.faceRetouch", {{"view", 2.0}}),
        withSecond("face_retouch_view_eyes", skinPortrait, faceMapCard, "wabcut.ai.faceRetouch", {{"view", 3.0}}),
        withSecond("face_retouch_zero", skinPortrait, faceMapCard, "wabcut.ai.faceRetouch",
                   {{"smoothing", 0.0}, {"blemish", 0.0}, {"teeth", 0.0}, {"eyes", 0.0}}),
        one("face_retouch_no_map", skinPortrait, "wabcut.ai.faceRetouch"),
        // 0.7 AI Eye Contact (Distort family): the evaluator bakes the frame's eye measurements into the parameters; the iris moves inside the opening.
        one("eye_contact_up", eyeCard, "wabcut.ai.eyeContact", eyeSettings(0.0, -10.0)),
        one("eye_contact_diagonal_rolled", eyeCardRolled, "wabcut.ai.eyeContact", eyeSettings(-6.0, -7.0, 8.0)),
        one("eye_contact_large", eyeCard, "wabcut.ai.eyeContact", eyeSettings(0.0, -15.0)),
        one("eye_contact_sideways", eyeCard, "wabcut.ai.eyeContact", eyeSettings(-12.0, 0.0)),
        one("eye_contact_view_map", eyeCard, "wabcut.ai.eyeContact", eyeSettings(0.0, -10.0, 0.0, 1.0)),
        one("eye_contact_zero_shift", eyeCard, "wabcut.ai.eyeContact", eyeSettings(0.0, 0.0)),
        one("eye_contact_no_eyes", eyeCard, "wabcut.ai.eyeContact", eyeSettings(0.0, -10.0, 0.0, 0.0, 0)),
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

// 0.7 (card 38): VR Reorient turns an equirectangular sphere (the picture here is only a stand-in for one: the maths do not
// care what is drawn), per eye, 360 or 180.
TEST_CASE("Effect library: VR Reorient matches its CPU reference (card 38)", "[render][gpu][library][vr]") {
    runCases({
        one("vr_reorient_yaw", colorful, "wabcut.vr.reorient", {{"yaw", 60.0}}),
        one("vr_reorient_all_angles", colorful, "wabcut.vr.reorient", {{"yaw", -45.0}, {"pitch", 30.0}, {"roll", 20.0}}),
        one("vr_reorient_180", colorful, "wabcut.vr.reorient", {{"projection", 1.0}, {"yaw", 20.0}, {"pitch", -10.0}}),
        one("vr_reorient_top_bottom", colorful, "wabcut.vr.reorient", {{"stereo", 1.0}, {"yaw", 30.0}, {"pitch", 10.0}}),
        one("vr_reorient_side_by_side", colorful, "wabcut.vr.reorient", {{"stereo", 2.0}, {"yaw", 15.0}, {"roll", 8.0}}),
        one("vr_reorient_none", colorful, "wabcut.vr.reorient"),
    });
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

TEST_CASE("Effect library: Skin Smoothing at 1080p stays real time (0.7)", "[render][gpu][library][perf]") {
    // The face of the 320 x 180 portrait enlarged to 1080p (a third of the frame is skin): render + readback, best of a few, reported.
    auto renderer = makeRenderer(OutputRole::Export);
    const auto face = [](int x, int y) { return skinPortrait(x * kW / 1920, y * kH / 1080); };
    qint64 best = std::numeric_limits<qint64>::max();
    CompositeLayer layer = layerOf(makeRgbaFrame(1920, 1080, face));
    layer.effects = {libraryEffectOf("wabcut.retouch.skinSmoothing", {{"amount", 100.0}, {"size", 8.0}})};
    for (int i = 0; i < 5; ++i) {
        QElapsedTimer timer;
        timer.start();
        auto image = renderer->render(compositeOf({1920, 1080}, {layer}));
        REQUIRE(image);
        best = std::min(best, timer.nsecsElapsed());
    }
    CompositeLayer plain = layerOf(makeRgbaFrame(1920, 1080, face));
    qint64 base = std::numeric_limits<qint64>::max();
    for (int i = 0; i < 5; ++i) {
        QElapsedTimer timer;
        timer.start();
        REQUIRE(renderer->render(compositeOf({1920, 1080}, {plain})));
        base = std::min(base, timer.nsecsElapsed());
    }
    WARN("Skin Smoothing at 1080p (size 8): " << best / 1.0e6 << " ms per frame incl. readback; the same frame without the effect "
                                               << base / 1.0e6 << " ms");
    {   // The figures also go to a file (a passing test prints no warnings): test-output/render/library/perf_skin_smoothing.txt.
        QDir().mkpath(outputDir() + QStringLiteral("/library"));
        QFile out(outputDir() + QStringLiteral("/library/perf_skin_smoothing.txt"));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(QByteArray("Skin Smoothing at 1080p (amount 100, size 8, a third of the frame is skin): ") + QByteArray::number(best / 1.0e6, 'f', 2) +
                      " ms per frame including readback; the same frame without the effect " + QByteArray::number(base / 1.0e6, 'f', 2) + " ms\n");
        }
    }
    CHECK(best < 1'000'000'000);           // a second would be a hang, not a cost
}

namespace {

/// A binary PPM (P6, 8-bit): its size and RGB bytes.
bool readPpm(const std::string& path, int& width, int& height, std::vector<std::uint8_t>& rgb) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    int maxValue = 0;
    in >> magic >> width >> height >> maxValue;
    in.get();
    if (magic != "P6" || width <= 0 || height <= 0 || maxValue != 255) {
        return false;
    }
    rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    in.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(in);
}

} // namespace

TEST_CASE("Effect library: AI Face Retouch on a real photo with the real face map (WABCUT_TEST_FACE_IMAGE, WABCUT_TEST_FACE_MAP)",
          "[render][gpu][library][live]") {
    // The photo and the face map the AI suite's live test made of it (both P6). The map goes through the evaluator's raster (the sRGB
    // encoding of its coverage) as the effect's second picture, like in playback.
    const char* photoPath = std::getenv("WABCUT_TEST_FACE_IMAGE");
    const char* mapPath = std::getenv("WABCUT_TEST_FACE_MAP");
    if (photoPath == nullptr || mapPath == nullptr || *photoPath == '\0' || *mapPath == '\0') {
        WARN("skipped: set WABCUT_TEST_FACE_IMAGE (a P6 photo of a face) and WABCUT_TEST_FACE_MAP (its face map, P6: R skin, G mouth, B eyes)");
        return;
    }
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> photo;
    int mw = 0;
    int mh = 0;
    std::vector<std::uint8_t> coverage;
    REQUIRE(readPpm(photoPath, w, h, photo));
    REQUIRE(readPpm(mapPath, mw, mh, coverage));
    REQUIRE(mw == w);
    REQUIRE(mh == h);
    auto renderer = makeRenderer(OutputRole::Export);
    const auto raster = makeFaceMapRaster(w, h, coverage);
    const std::shared_ptr<const PreparedComposite> matte = compositeOf({w, h}, {layerOf(raster->view(raster))});
    const auto picture = [&](int x, int y) {
        const std::uint8_t* p = &photo[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3];
        return std::array<std::uint8_t, 4>{p[0], p[1], p[2], 255};
    };
    const auto renderWith = [&](std::vector<std::pair<std::string_view, double>> settings, std::optional<QSize> size = {}) {
        CompositeLayer layer = layerOf(makeRgbaFrame(w, h, picture));
        LayerEffect effect = libraryEffectOf("wabcut.ai.faceRetouch", settings);
        effect.matte = matte;
        layer.effects = {effect};
        auto image = renderer->render(compositeOf({w, h}, {layer}), size);
        REQUIRE(image);
        return std::move(*image);
    };
    CompositeLayer plainLayer = layerOf(makeRgbaFrame(w, h, picture));
    auto original = renderer->render(compositeOf({w, h}, {plainLayer}));
    REQUIRE(original);
    const RenderedImage off = renderWith({{"smoothing", 0.0}, {"blemish", 0.0}, {"teeth", 0.0}, {"eyes", 0.0}});
    const RenderedImage soft = renderWith({}); // the defaults
    const RenderedImage full = renderWith({{"smoothing", 100.0}, {"detail", 0.0}, {"blemish", 100.0}, {"teeth", 100.0}, {"eyes", 100.0}});
    REQUIRE(off.rgba.size() == original->rgba.size());
    // Every amount at 0 is the picture as it was, byte for byte; the defaults and the full retouch change only what the map covers.
    CHECK(off.rgba == original->rgba);
    long long covered = 0;
    long long outsideChanged = 0;
    long long insideChanged = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            const bool inMap = coverage[3 * i] > 0 || coverage[3 * i + 1] > 0 || coverage[3 * i + 2] > 0;
            covered += inMap ? 1 : 0;
            const bool changed = !std::equal(original->rgba.begin() + static_cast<std::ptrdiff_t>(4 * i), original->rgba.begin() + static_cast<std::ptrdiff_t>(4 * i + 3),
                                             full.rgba.begin() + static_cast<std::ptrdiff_t>(4 * i));
            if (changed) {
                (inMap ? insideChanged : outsideChanged) += 1;
            }
        }
    }
    WARN("face map covers " << covered << " of " << static_cast<long long>(w) * h << " pixels; full retouch changed " << insideChanged << " of them and "
                            << outsideChanged << " outside");
    CHECK(covered > 5'000);
    CHECK(outsideChanged == 0); // the dogs, the suit and the flag are not touched, however skin-coloured
    CHECK(insideChanged > covered / 4);
    // The retouch is not violent: the mean change inside the face stays small and no channel moves by more than a third.
    double sum = 0.0;
    int largest = 0;
    long long n = 0;
    for (std::size_t i = 0; i < original->rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            const int d = std::abs(static_cast<int>(original->rgba[i + c]) - static_cast<int>(full.rgba[i + c]));
            if (d != 0) {
                sum += d;
                ++n;
                largest = std::max(largest, d);
            }
        }
    }
    WARN("full retouch: mean change of a changed value " << (n > 0 ? sum / static_cast<double>(n) : 0.0) << " levels, largest " << largest);
    CHECK(largest < 85);
    // For looking at: the original, the defaults and the full retouch, and the face enlarged side by side.
    const QString dir = outputDir() + QStringLiteral("/library");
    QDir().mkpath(dir);
    const auto asImage = [&](const RenderedImage& image) { return QImage(image.rgba.data(), w, h, w * 4, QImage::Format_RGBA8888).convertToFormat(QImage::Format_RGB888); };
    asImage(*original).save(dir + QStringLiteral("/real_face_original.png"));
    asImage(soft).save(dir + QStringLiteral("/real_face_retouch_default.png"));
    asImage(full).save(dir + QStringLiteral("/real_face_retouch_full.png"));
    int left = w, right = 0, top = h, bottom = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (coverage[3 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x))] > 128) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    const QRect face(std::max(0, left - 20), std::max(0, top - 20), std::min(w, right + 20) - std::max(0, left - 20), std::min(h, bottom + 20) - std::max(0, top - 20));
    QImage strip(face.width() * 3 * 2, face.height() * 2, QImage::Format_RGB888);
    strip.fill(Qt::black);
    for (int k = 0; k < 3; ++k) {
        const QImage part = (k == 0 ? asImage(*original) : k == 1 ? asImage(soft) : asImage(full)).copy(face).scaled(face.size() * 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (int y = 0; y < part.height(); ++y) {
            for (int x = 0; x < part.width(); ++x) {
                strip.setPixel(k * part.width() + x, y, part.pixel(x, y));
            }
        }
    }
    strip.save(dir + QStringLiteral("/real_face_compare.png"));
    // Timing: the retouch at the photo's size (1280 x 1024), best of a few runs, with the map's raster and the effect.
    qint64 best = std::numeric_limits<qint64>::max();
    for (int i = 0; i < 4; ++i) {
        CompositeLayer layer = layerOf(makeRgbaFrame(w, h, picture));
        LayerEffect effect = libraryEffectOf("wabcut.ai.faceRetouch", {{"smoothing", 100.0}, {"blemish", 100.0}, {"teeth", 100.0}, {"eyes", 100.0}});
        effect.matte = matte;
        layer.effects = {effect};
        QElapsedTimer timer;
        timer.start();
        REQUIRE(renderer->render(compositeOf({w, h}, {layer})));
        best = std::min(best, timer.nsecsElapsed());
    }
    WARN("AI Face Retouch at " << w << " x " << h << ": " << best / 1.0e6 << " ms per frame incl. readback");
    {
        QFile out(dir + QStringLiteral("/perf_face_retouch.txt"));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(QByteArray("AI Face Retouch at ") + QByteArray::number(w) + " x " + QByteArray::number(h) + " (everything at 100, the real face map): " +
                      QByteArray::number(best / 1.0e6, 'f', 2) + " ms per frame including readback\n");
        }
    }
    CHECK(best < 1'000'000'000);
}

TEST_CASE("Effect library: AI Eye Contact on a real photo with the real landmarks, looking down and corrected (WABCUT_TEST_FACE_IMAGE, WABCUT_TEST_GAZE_FACE)",
          "[render][gpu][library][live]") {
    // The photo and the eyes the AI suite's live test measured on it (a text file: per eye cx cy angle halfWidth upper lower irisX irisY irisRadius,
    // then turn score size). The photo's subject looks into the camera: the test makes the picture of the same subject looking down by moving each
    // iris with the effect itself (a shift down), then corrects that picture with the iris where it now is, and compares both with the original.
    const char* photoPath = std::getenv("WABCUT_TEST_FACE_IMAGE");
    const char* gazePath = std::getenv("WABCUT_TEST_GAZE_FACE");
    if (photoPath == nullptr || gazePath == nullptr || *photoPath == '\0' || *gazePath == '\0') {
        WARN("skipped: set WABCUT_TEST_FACE_IMAGE (a P6 photo of a face) and WABCUT_TEST_GAZE_FACE (its eyes, written by the AI suite's live landmark test)");
        return;
    }
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> photo;
    REQUIRE(readPpm(photoPath, w, h, photo));
    std::array<std::array<double, 9>, 2> eyes{};
    {
        std::ifstream in(gazePath);
        REQUIRE(in.good());
        for (auto& eye : eyes) {
            for (double& v : eye) {
                in >> v;
            }
        }
        REQUIRE(in.good());
    }
    // The compositor's parameters of both eyes (model/EyeContact.h layout); keys are stored so the string views stay valid.
    std::vector<std::string> keyStorage;
    for (int i = 0; i < 30; ++i) {
        keyStorage.push_back("#" + std::to_string(i));
    }
    // `irisDown` is added to each iris's y in the eye's frame, `shift[e]` is eye e's shift (y, in the eye's frame).
    const auto settingsFor = [&](const std::array<double, 2>& irisDown, const std::array<double, 2>& shift) {
        std::vector<std::pair<std::string_view, double>> s;
        s.emplace_back(keyStorage[6], 2.0);
        for (std::size_t e = 0; e < 2; ++e) {
            const std::size_t base = 8 + 11 * e;
            for (std::size_t k = 0; k < 9; ++k) {
                s.emplace_back(keyStorage[base + k], eyes[e][k] + (k == 7 ? irisDown[e] : 0.0));
            }
            s.emplace_back(keyStorage[base + 9], 0.0);
            s.emplace_back(keyStorage[base + 10], shift[e]);
        }
        return s;
    };
    auto renderer = makeRenderer(OutputRole::Export);
    using Settings = std::vector<std::pair<std::string_view, double>>;
    // The photo through a stack of Eye Contact effects (each with its own baked eyes), one after the other in linear light.
    const auto renderOf = [&](const std::vector<std::uint8_t>& rgba, const std::vector<Settings>& stack) {
        const auto picture = [&](int x, int y) {
            const std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4];
            return std::array<std::uint8_t, 4>{p[0], p[1], p[2], p[3]};
        };
        CompositeLayer layer = layerOf(makeRgbaFrame(w, h, picture));
        for (const Settings& settings : stack) {
            layer.effects.push_back(libraryEffectOf("wabcut.ai.eyeContact", settings));
        }
        auto image = renderer->render(compositeOf({w, h}, {layer}));
        REQUIRE(image);
        return std::move(*image);
    };
    // The eyes of a portrait are small: the test works on a crop round them enlarged four times (the eyes' numbers follow the crop: positions
    // are fractions of the frame, lengths fractions of its width).
    std::vector<std::uint8_t> rgba;
    {
        QImage whole(w, h, QImage::Format_RGB888);
        for (int y = 0; y < h; ++y) {
            std::copy(photo.begin() + static_cast<std::ptrdiff_t>(3 * static_cast<std::size_t>(y) * static_cast<std::size_t>(w)),
                      photo.begin() + static_cast<std::ptrdiff_t>(3 * (static_cast<std::size_t>(y) + 1) * static_cast<std::size_t>(w)), whole.scanLine(y));
        }
        const double left = std::max(0.0, std::min(eyes[0][0], eyes[1][0]) * w - eyes[0][3] * w - 24.0);
        const double right = std::min<double>(w, std::max(eyes[0][0], eyes[1][0]) * w + eyes[1][3] * w + 24.0);
        const double top = std::max(0.0, std::min(eyes[0][1], eyes[1][1]) * h - 0.04 * w);
        const double bottom = std::min<double>(h, std::max(eyes[0][1], eyes[1][1]) * h + 0.04 * w);
        const QRect crop(static_cast<int>(left), static_cast<int>(top), static_cast<int>(right - left), static_cast<int>(bottom - top));
        const QImage big = whole.copy(crop).scaled(crop.size() * 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888);
        for (auto& eye : eyes) {
            eye[0] = (eye[0] * w - crop.x()) / crop.width();
            eye[1] = (eye[1] * h - crop.y()) / crop.height();
            for (const std::size_t k : {3u, 4u, 5u, 6u, 7u, 8u}) {
                eye[k] *= static_cast<double>(w) / crop.width(); // lengths: fractions of the width of the new frame
            }
        }
        w = big.width();
        h = big.height();
        rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 255);
        for (int y = 0; y < h; ++y) {
            std::copy(big.constScanLine(y), big.constScanLine(y) + static_cast<std::ptrdiff_t>(w) * 4, rgba.begin() + static_cast<std::ptrdiff_t>(y) * w * 4);
        }
    }
    // The original as the renderer shows it, and the same with the effect and no eyes baked: byte for byte the same.
    const RenderedImage original = renderOf(rgba, {});
    const RenderedImage withoutEyes = renderOf(rgba, {{}});
    CHECK((withoutEyes.rgba == original.rgba));
    // Looking down: each iris 12 % of the eye's width lower.
    const std::array<double, 2> down{0.12 * 2.0 * eyes[0][3], 0.12 * 2.0 * eyes[1][3]};
    const Settings lower = settingsFor({0.0, 0.0}, down);
    const RenderedImage looking = renderOf(rgba, {lower});
    // Corrected: a second effect after the first, with the iris where it now is and the shift back up by what it was moved down.
    const Settings fix = settingsFor(down, {-down[0], -down[1]});
    const RenderedImage corrected = renderOf(rgba, {lower, fix});
    REQUIRE(original.rgba.size() == looking.rgba.size());
    // Where the eyes are (a box round each, the width of the eye) and everything else.
    const auto inWindow = [&](int x, int y, double margin) {
        for (const auto& eye : eyes) {
            const double r = eye[3] * w + margin;
            if (std::abs(x + 0.5 - eye[0] * w) < r && std::abs(y + 0.5 - eye[1] * h) < r) {
                return true;
            }
        }
        return false;
    };
    double errLooking = 0.0;
    double errCorrected = 0.0;
    long long n = 0;
    long long outsideChanged = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = 4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x));
            if (inWindow(x, y, 0.0)) {
                for (std::size_t c = 0; c < 3; ++c) {
                    errLooking += std::abs(static_cast<int>(looking.rgba[i + c]) - static_cast<int>(original.rgba[i + c]));
                    errCorrected += std::abs(static_cast<int>(corrected.rgba[i + c]) - static_cast<int>(original.rgba[i + c]));
                }
                ++n;
            } else if (!inWindow(x, y, 6.0)) {
                bool differs = false;
                for (std::size_t c = 0; c < 3; ++c) {
                    differs = differs || looking.rgba[i + c] != original.rgba[i + c] || corrected.rgba[i + c] != original.rgba[i + c];
                }
                outsideChanged += differs ? 1 : 0;
            }
        }
    }
    errLooking /= static_cast<double>(std::max<long long>(1, n) * 3);
    errCorrected /= static_cast<double>(std::max<long long>(1, n) * 3);
    WARN("eye windows: mean error against the original, looking down " << errLooking << " levels, corrected " << errCorrected << " levels; outside changed " << outsideChanged);
    CHECK(outsideChanged == 0);             // nothing but the eyes is touched
    CHECK(errLooking > 0.3);                // moving the iris changed the picture
    CHECK(errCorrected < errLooking * 1.25); // and the correction does not make it worse: it brings the iris back (resampled twice) to where the camera saw it
    // For looking at: the original, the iris moved down and the correction, the eyes enlarged four times, side by side.
    const QString dir = outputDir() + QStringLiteral("/library");
    QDir().mkpath(dir);
    const auto asImage = [&](const RenderedImage& image) { return QImage(image.rgba.data(), w, h, w * 4, QImage::Format_RGBA8888).convertToFormat(QImage::Format_RGB888); };
    QImage strip(w * 3, h, QImage::Format_RGB888);
    strip.fill(Qt::black);
    for (int k = 0; k < 3; ++k) {
        const QImage part = k == 0 ? asImage(original) : k == 1 ? asImage(looking) : asImage(corrected);
        for (int y = 0; y < part.height(); ++y) {
            for (int x = 0; x < part.width(); ++x) {
                strip.setPixel(k * part.width() + x, y, part.pixel(x, y));
            }
        }
    }
    strip.save(dir + QStringLiteral("/real_eye_contact_compare.png"));
    // Timing: both eyes at the photo's size, best of a few runs, with the upload and the readback.
    qint64 best = std::numeric_limits<qint64>::max();
    for (int i = 0; i < 4; ++i) {
        QElapsedTimer timer;
        timer.start();
        REQUIRE_FALSE(renderOf(rgba, {lower, fix}).rgba.empty());
        best = std::min(best, timer.nsecsElapsed());
    }
    WARN("AI Eye Contact (two effects of two eyes each) at " << w << " x " << h << ": " << best / 1.0e6 << " ms per frame incl. the upload and readback");
    CHECK(best < 1'000'000'000);
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

// ---- 0.6: Glow and the generators Premiere / After Effects editors reach for (card #70) ----------------------------

namespace {

/// Flat regions with hard outlines (Paint Bucket's picture): a pale blue disc in a dark ring on white, a dark bar, a
/// yellow right part and a transparent corner at the top left.
std::array<std::uint8_t, 4> cartoon(int x, int y) {
    const double r = std::hypot(x + 0.5 - 110.0, y + 0.5 - 90.0);
    if (x < 40 && y < 30) {
        return {0, 0, 0, 0};
    }
    if (r >= 50.0 && r < 56.0) {
        return {10, 10, 10, 255};
    }
    if (r < 50.0) {
        return {150, 200, 250, 255};
    }
    if (x >= 230 && x < 236) {
        return {10, 10, 10, 255};
    }
    if (x >= 236) {
        return {250, 230, 120, 255};
    }
    return {245, 245, 245, 255};
}

std::vector<Case> cases06() {
    return {
        one("glow", colorful, "wabcut.stylize.glow", {{"threshold", 0.55}, {"radius", 12.0}}),
        one("glow_ab_horizontal_screen", colorful, "wabcut.stylize.glow",
            {{"threshold", 0.5}, {"radius", 16.0}, {"colors", 1.0}, {"colorB.r", 0.9}, {"colorB.g", 0.2}, {"dimensions", 1.0},
             {"operation", 1.0}, {"intensity", 1.5}}),
        one("glow_alpha_on_top", card, "wabcut.stylize.glow",
            {{"basedOn", 1.0}, {"threshold", 0.2}, {"radius", 10.0}, {"composite", 0.0}, {"intensity", 2.0}}),
        one("glow_vertical_alone_blend", colorful, "wabcut.stylize.glow",
            {{"dimensions", 2.0}, {"composite", 2.0}, {"blend", 0.3}, {"operation", 3.0}}),
        one("colorama", colorful, "wabcut.generate.colorama"),
        one("colorama_fire_hue", colorful, "wabcut.generate.colorama",
            {{"palette", 4.0}, {"input", 2.0}, {"phaseShift", 90.0}, {"blend", 0.2}}),
        one("colorama_three_stops_steps", colorful, "wabcut.generate.colorama",
            {{"colors", 3.0}, {"interpolate", 0.0}, {"input", 1.0}, {"cycles", 2.0}, {"blendingMode", 2.0}}),
        one("colorama_alpha_ramp", card, "wabcut.generate.colorama", {{"input", 7.0}, {"palette", 1.0}}),
        one("fractal_noise", colorful, "wabcut.generate.fractalNoise"),
        one("fractal_noise_turbulent_spline", colorful, "wabcut.generate.fractalNoise",
            {{"fractalType", 1.0}, {"noiseType", 3.0}, {"complexity", 4.5}, {"rotation", 30.0}, {"evolution", 120.0},
             {"blendingMode", 5.0}, {"opacity", 0.7}, {"scale", 60.0}}),
        one("fractal_noise_dynamic_block_wrap", colorful, "wabcut.generate.fractalNoise",
            {{"fractalType", 2.0}, {"noiseType", 0.0}, {"cycleEvolution", 1.0}, {"evolution", 400.0}, {"contrast", 200.0},
             {"overflow", 2.0}, {"seed", 9.0}}),
        one("fractal_noise_linear_soft_invert", colorful, "wabcut.generate.fractalNoise",
            {{"fractalType", 3.0}, {"noiseType", 1.0}, {"overflow", 1.0}, {"invert", 1.0}, {"subRotation", 25.0},
             {"blendingMode", 3.0}}),
        one("cell_pattern", colorful, "wabcut.generate.cellPattern"),
        one("cell_pattern_crystals_border", colorful, "wabcut.generate.cellPattern",
            {{"pattern", 1.0}, {"border", 0.3}, {"size", 30.0}, {"evolution", 90.0}}),
        one("cell_pattern_plates_screen", colorful, "wabcut.generate.cellPattern",
            {{"pattern", 2.0}, {"blendingMode", 4.0}, {"evolution", 45.0}, {"seed", 3.0}}),
        one("cell_pattern_static_plates", colorful, "wabcut.generate.cellPattern", {{"pattern", 3.0}, {"disperse", 0.4}}),
        one("cell_pattern_crystallize", colorful, "wabcut.generate.cellPattern", {{"pattern", 4.0}, {"size", 25.0}}),
        one("cell_pattern_pillow_invert", colorful, "wabcut.generate.cellPattern",
            {{"pattern", 5.0}, {"invert", 1.0}, {"contrast", 150.0}, {"overflow", 1.0}}),
        one("cell_pattern_tubular_multiply", colorful, "wabcut.generate.cellPattern",
            {{"pattern", 6.0}, {"blendingMode", 3.0}, {"opacity", 0.8}}),
        one("lightning", colorful, "wabcut.generate.lightning", {{"time", 0.5}}),
        one("lightning_branchy_add", colorful, "wabcut.generate.lightning",
            {{"branching", 0.8}, {"rebranching", 0.4}, {"detail", 3.0}, {"blendingMode", 1.0}, {"time", 1.3}, {"seed", 7.0},
             {"width", 14.0}, {"fixedEndpoint", 0.0}}),
        one("lightning_screen_on_card", card, "wabcut.generate.lightning",
            {{"blendingMode", 2.0}, {"start.x", 0.1}, {"start.y", 0.5}, {"end.x", 0.9}, {"end.y", 0.5}, {"time", 2.0}}),
        one("scribble", colorful, "wabcut.generate.scribble", {{"spacing", 14.0}}),
        one("scribble_edge_progress", colorful, "wabcut.generate.scribble",
            {{"fillType", 1.0}, {"end", 0.6}, {"strokeWidth", 3.0}, {"edgeWidth", 16.0}}),
        one("scribble_reveal_curvy", colorful, "wabcut.generate.scribble",
            {{"composite", 2.0}, {"curvature", 0.4}, {"angle", 10.0}, {"spacing", 12.0}, {"strokeWidth", 8.0}, {"jitter", 3.0},
             {"wiggles", 4.0}, {"time", 0.75}}),
        one("paint_bucket", cartoon, "wabcut.generate.paintBucket", {{"point.x", 0.35}, {"point.y", 0.5}}),
        one("paint_bucket_feather_behind", cartoon, "wabcut.generate.paintBucket",
            {{"point.x", 0.05}, {"point.y", 0.9}, {"stroke", 1.0}, {"softness", 6.0}, {"color.g", 0.6}, {"blendingMode", 1.0}}),
        one("paint_bucket_stroke", cartoon, "wabcut.generate.paintBucket",
            {{"point.x", 0.35}, {"point.y", 0.5}, {"stroke", 4.0}, {"strokeWidth", 4.0}, {"color.b", 1.0}}),
        one("paint_bucket_spread_multiply", cartoon, "wabcut.generate.paintBucket",
            {{"point.x", 0.9}, {"point.y", 0.5}, {"stroke", 2.0}, {"strokeWidth", 6.0}, {"blendingMode", 3.0}}),
        one("paint_bucket_choke_fill_only", cartoon, "wabcut.generate.paintBucket",
            {{"point.x", 0.35}, {"point.y", 0.5}, {"stroke", 3.0}, {"strokeWidth", 6.0}, {"blendingMode", 5.0}}),
        one("paint_bucket_transparency_invert", cartoon, "wabcut.generate.paintBucket",
            {{"point.x", 0.02}, {"point.y", 0.02}, {"selector", 2.0}, {"tolerance", 20.0}, {"invert", 1.0}, {"opacity", 0.6}}),
        one("paint_bucket_view_threshold", cartoon, "wabcut.generate.paintBucket",
            {{"point.x", 0.35}, {"point.y", 0.5}, {"viewThreshold", 1.0}}),
    };
}

ref::LibraryParams paramsOf(const LayerEffect& e) {
    ref::LibraryParams q{};
    std::copy(e.params.begin(), e.params.end(), q.begin());
    return q;
}

ref::Image referenceOf(Picture picture, const LayerEffect& e, std::span<const ref::LibraryPath> paths = {}) {
    return ref::libraryEffect(toReference(linearFromRgba8(kW, kH, picture)), static_cast<ref::LibraryEffect>(e.op),
                              paramsOf(e), nullptr, paths);
}

double imageDifference(const ref::Image& a, const ref::Image& b) {
    double most = 0.0;
    for (std::size_t i = 0; i < a.rgba.size() && i < b.rgba.size(); ++i) {
        most = std::max(most, std::abs(static_cast<double>(a.rgba[i]) - static_cast<double>(b.rgba[i])));
    }
    return most;
}

} // namespace

TEST_CASE("Effect library: Glow and the 0.6 generators match their CPU reference", "[render][gpu][library]") {
    runCases(cases06());
}

TEST_CASE("Effect library: Scribble draws in and along its mask path and leaves the rest (0.6)", "[render][gpu][library]") {
    // A triangle mask (normalised frame coordinates, moved by its transform) is Scribble's path, not a mix.
    auto mask = std::make_shared<MaskSpec>();
    MaskShapeSpec shape;
    shape.polygon = {{0.2, 0.2}, {0.8, 0.3}, {0.5, 0.85}};
    shape.affine = {1.0, 0.0, 0.05, 0.0, 1.0, -0.05};
    mask->shapes.push_back(shape);
    mask->aspect = static_cast<double>(kW) / kH;
    mask->seal();
    std::vector<ref::LibraryPath> paths{{}};
    for (const Vec2& v : shape.polygon) {
        paths[0].push_back({(v.x + 0.05) * kW, (v.y - 0.05) * kH});
    }
    auto renderer = makeRenderer(OutputRole::Export);
    auto plain = renderer->render(compositeOf({kW, kH}, {layerOf(makeRgbaFrame(kW, kH, colorful))}));
    REQUIRE(plain);
    for (const double fill : {0.0, 1.0, 3.0}) {
        DYNAMIC_SECTION("fill type " << fill) {
            LayerEffect e = libraryEffectOf("wabcut.generate.scribble",
                                            {{"fillType", fill}, {"spacing", 9.0}, {"strokeWidth", 3.0}, {"edgeWidth", 12.0},
                                             {"color.g", 1.0}});
            e.mask = mask;
            CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, colorful));
            layer.effects = {e};
            auto image = renderer->render(compositeOf({kW, kH}, {layer}));
            REQUIRE(image);
            const LinearImage expected = fromReference(referenceOf(colorful, e, paths));
            const std::vector<std::uint8_t> rgb = encodeOverBlack(expected, outputTransformFor(ColorPipeline{}, OutputRole::Export));
            savePng(*image, QStringLiteral("library/scribble_mask_%1.png").arg(fill));
            const RenderMatch m = compareToReference(*image, rgb);
            WARN("scribble in a mask: PSNR " << m.psnr << " dB");
            CHECK(m.psnr >= 40.0);
            CHECK(m.mismatched <= 0.003);
            // Far from the triangle the picture is untouched; the strokes are yellow (red + green) on it.
            CHECK(maxDiff(pixelOf(*image, 5, 175), pixelOf(*plain, 5, 175)) <= 1.5 / 255.0);
            CHECK(maxDiff(pixelOf(*image, 300, 10), pixelOf(*plain, 300, 10)) <= 1.5 / 255.0);
            int yellow = 0;
            for (int y = 0; y < kH; ++y) {
                for (int x = 0; x < kW; ++x) {
                    const Rgb c = pixelOf(*image, x, y);
                    yellow += c[0] > 0.95 && c[1] > 0.95 && c[2] < 0.1 ? 1 : 0;
                }
            }
            CHECK(yellow > 200);
        }
    }
}

TEST_CASE("Effect library: Fractal Noise Cell Pattern and Lightning are the same for a seed and time (0.6)",
          "[render][library]") {
    const LayerEffect noise = libraryEffectOf("wabcut.generate.fractalNoise", {{"evolution", 30.0}, {"seed", 4.0}});
    CHECK(imageDifference(referenceOf(colorful, noise), referenceOf(colorful, noise)) == 0.0);
    const LayerEffect otherSeed = libraryEffectOf("wabcut.generate.fractalNoise", {{"evolution", 30.0}, {"seed", 5.0}});
    CHECK(imageDifference(referenceOf(colorful, noise), referenceOf(colorful, otherSeed)) > 0.05);
    const LayerEffect evolved = libraryEffectOf("wabcut.generate.fractalNoise", {{"evolution", 60.0}, {"seed", 4.0}});
    CHECK(imageDifference(referenceOf(colorful, noise), referenceOf(colorful, evolved)) > 0.01);
    // Cycle Evolution: a whole cycle later the noise is back.
    const LayerEffect cycled = libraryEffectOf("wabcut.generate.fractalNoise",
                                               {{"evolution", 30.0}, {"seed", 4.0}, {"cycleEvolution", 1.0}, {"cycle", 2.0}});
    const LayerEffect cycledLater = libraryEffectOf("wabcut.generate.fractalNoise",
                                                    {{"evolution", 30.0 + 720.0}, {"seed", 4.0}, {"cycleEvolution", 1.0}, {"cycle", 2.0}});
    CHECK(imageDifference(referenceOf(colorful, cycled), referenceOf(colorful, cycledLater)) < 1e-4);

    const LayerEffect cells = libraryEffectOf("wabcut.generate.cellPattern", {{"seed", 2.0}});
    const LayerEffect otherCells = libraryEffectOf("wabcut.generate.cellPattern", {{"seed", 3.0}});
    CHECK(imageDifference(referenceOf(colorful, cells), referenceOf(colorful, cells)) == 0.0);
    CHECK(imageDifference(referenceOf(colorful, cells), referenceOf(colorful, otherCells)) > 0.05);

    // Lightning: the same bolt for a seed and time; Speed makes a new shape over time, Stability 1 holds it.
    const auto bolt = [](double time, double seed, double stability) {
        ref::LibraryParams q = paramsOf(libraryEffectOf("wabcut.generate.lightning",
                                                        {{"time", time}, {"seed", seed}, {"stability", stability}}));
        return ref::lightningStrokes(q, kW, kH);
    };
    const auto same = [](const std::vector<ref::StrokeSegment>& a, const std::vector<ref::StrokeSegment>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].ax != b[i].ax || a[i].ay != b[i].ay || a[i].bx != b[i].bx || a[i].by != b[i].by) {
                return false;
            }
        }
        return true;
    };
    CHECK(same(bolt(0.5, 1.0, 0.3), bolt(0.5, 1.0, 0.3)));
    CHECK_FALSE(same(bolt(0.5, 1.0, 0.3), bolt(0.5, 2.0, 0.3)));
    CHECK_FALSE(same(bolt(0.5, 1.0, 0.3), bolt(0.9, 1.0, 0.3)));
    CHECK(same(bolt(0.5, 1.0, 1.0), bolt(3.0, 1.0, 1.0)));
    const auto segments = bolt(0.5, 1.0, 0.3);
    REQUIRE_FALSE(segments.empty());
    // Fixed Endpoint: the bolt starts and ends on its points (frame pixels).
    CHECK(std::abs(segments.front().ax - 0.2 * kW) < 1e-9);
    CHECK(std::abs(segments.front().ay - 0.15 * kH) < 1e-9);

    // The GPU draws the same frame twice the same.
    auto renderer = makeRenderer(OutputRole::Export);
    CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, colorful));
    layer.effects = {libraryEffectOf("wabcut.generate.lightning", {{"time", 0.4}}),
                     libraryEffectOf("wabcut.generate.fractalNoise", {{"blendingMode", 5.0}, {"opacity", 0.5}})};
    auto first = renderer->render(compositeOf({kW, kH}, {layer}));
    auto second = renderer->render(compositeOf({kW, kH}, {layer}));
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->rgba == second->rgba);
}

TEST_CASE("Effect library: the 0.6 effects read 8-bit and 10-bit sources alike", "[render][gpu][library]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const ColorInfo tags{ColorMatrix::BT709, ColorRange::Full, ColorPrimaries::BT709, TransferFunction::BT709,
                         ChromaLocation::Left};
    const auto rgbOf = [](Picture picture) {
        return [picture](int x, int y) {
            const auto c = picture(x, y);
            return Rgb{c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
        };
    };
    const std::vector<std::pair<std::string, LayerEffect>> effects = {
        {"glow", libraryEffectOf("wabcut.stylize.glow", {{"threshold", 0.5}})},
        {"colorama", libraryEffectOf("wabcut.generate.colorama", {})},
        {"fractal_noise", libraryEffectOf("wabcut.generate.fractalNoise", {{"blendingMode", 5.0}})},
        {"cell_pattern", libraryEffectOf("wabcut.generate.cellPattern", {{"blendingMode", 3.0}})},
        {"lightning", libraryEffectOf("wabcut.generate.lightning", {{"time", 0.2}})},
        {"scribble", libraryEffectOf("wabcut.generate.scribble", {})},
        {"paint_bucket", libraryEffectOf("wabcut.generate.paintBucket", {{"point.x", 0.35}, {"point.y", 0.5}, {"tolerance", 30.0}})},
    };
    for (const auto& [name, effect] : effects) {
        DYNAMIC_SECTION(name) {
            const Picture picture = name == "paint_bucket" ? cartoon : colorful;
            std::vector<std::vector<std::uint8_t>> renders;
            for (const media::PixelFormat format : {media::PixelFormat::YUV444P, media::PixelFormat::YUV444P10}) {
                CompositeLayer layer = layerOf(makeFrame(format, kW, kH, tags, rgbOf(picture)));
                layer.effects = {effect};
                auto image = renderer->render(compositeOf({kW, kH}, {layer}));
                REQUIRE(image);
                renders.push_back(image->rgba);
            }
            double squared = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i < renders[0].size(); ++i) {
                if (i % 4 == 3) {
                    continue;
                }
                const double d = static_cast<double>(renders[0][i]) - static_cast<double>(renders[1][i]);
                squared += d * d;
                ++count;
            }
            const double mse = squared / std::max<std::size_t>(count, 1);
            const double psnr = mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
            WARN(name << ": 8-bit against 10-bit source PSNR " << psnr << " dB");
            CHECK(psnr >= 35.0);
        }
    }
}

TEST_CASE("Effect library: 1080p frame time of Fractal Noise Cell Pattern Glow and Paint Bucket (0.6)",
          "[render][gpu][library][perf]") {
    // Best of three, render + readback at 1080p: a budget for the heaviest 0.6 effects (the GPU may be shared, so it
    // is generous; the times are reported).
    auto renderer = makeRenderer(OutputRole::Export);
    const auto big = [](int x, int y) { return colorful(x * kW / 1920, y * kH / 1080); };
    const std::vector<std::pair<std::string, LayerEffect>> effects = {
        {"fractal_noise", libraryEffectOf("wabcut.generate.fractalNoise", {{"noiseType", 3.0}})},
        {"cell_pattern", libraryEffectOf("wabcut.generate.cellPattern", {})},
        {"glow", libraryEffectOf("wabcut.stylize.glow", {{"radius", 30.0}})},
        {"paint_bucket", libraryEffectOf("wabcut.generate.paintBucket", {{"tolerance", 40.0}})},
        {"lightning", libraryEffectOf("wabcut.generate.lightning", {{"branching", 0.8}, {"detail", 3.0}})},
        {"scribble", libraryEffectOf("wabcut.generate.scribble", {})},
    };
    QString report;
    for (const auto& [name, effect] : effects) {
        CompositeLayer layer = layerOf(makeRgbaFrame(1920, 1080, big));
        layer.effects = {effect};
        qint64 best = std::numeric_limits<qint64>::max();
        for (int i = 0; i < 3; ++i) {
            QElapsedTimer timer;
            timer.start();
            REQUIRE(renderer->render(compositeOf({1920, 1080}, {layer})));
            best = std::min(best, timer.nsecsElapsed());
        }
        WARN(name << " at 1080p: " << best / 1.0e6 << " ms per frame incl. readback");
        report += QStringLiteral("%1: %2 ms\n").arg(QString::fromStdString(name)).arg(best / 1.0e6, 0, 'f', 1);
        CHECK(best < 250'000'000);
    }
    QFile file(outputDir() + QStringLiteral("/library/perf-06.txt"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(report.toUtf8());
    }
}
