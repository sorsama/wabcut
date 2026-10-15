// Golden-image tests of the compositor's effect and transition passes (shaders/fx.frag) against their CPU
// twins (EffectsReference.cpp), plus analytic checks: crop on a transformed layer, blur radius independent of the
// preview divisor, and bypassed effects passing through. Rendered pairs land in test-output/render/effects/.

#include "EffectsReference.h"
#include "RenderTestSupport.h"

#include <wabcut/render/Compositor.h>
#include <wabcut/render/Title.h>
#include <wabcut/model/Blend.h>
#include <wabcut/render/reference/Blend.h>
#include <wabcut/render/reference/UltraKey.h>

#include <QColor>
#include <QDir>
#include <QFont>
#include <QImage>
#include <QPainter>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

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

/// A busy opaque picture: gradients, a checkerboard and hard lines (for blur/sharpen).
std::array<std::uint8_t, 4> busy(int x, int y) {
    const bool check = ((x / 20) + (y / 20)) % 2 == 0;
    const bool line = x % 40 == 7 || y % 30 == 11;
    if (line) {
        return {245, 245, 240, 255};
    }
    return {byte(x / double(kW)), byte(0.15 + 0.7 * y / double(kH)), check ? std::uint8_t(200) : std::uint8_t(40), 255};
}

/// A coloured card in the middle with transparent margins (for crop edges and shadows).
std::array<std::uint8_t, 4> card(int x, int y) {
    const bool inside = x >= 80 && x < 240 && y >= 45 && y < 135;
    if (!inside) {
        return {0, 0, 0, 0};
    }
    return {byte(0.9 - (x - 80) / 320.0), byte(0.3 + (y - 45) / 200.0), 60, 255};
}

/// A second picture for transitions.
std::array<std::uint8_t, 4> stripes(int x, int y) {
    return {byte(0.2 + 0.6 * ((x / 16) % 2)), 90, byte(0.9 - 0.7 * y / double(kH)), 255};
}

using Picture = std::array<std::uint8_t, 4> (*)(int, int);

CompositeLayer layerFrom(Picture picture) {
    return layerOf(makeRgbaFrame(kW, kH, picture));
}

QString effectsDir() {
    const QString dir = outputDir() + QStringLiteral("/effects");
    QDir().mkpath(dir);
    return dir;
}

void saveReference(const std::vector<std::uint8_t>& rgb, int w, int h, const QString& name) {
    QImage image(rgb.data(), w, h, w * 3, QImage::Format_RGB888);
    image.save(effectsDir() + QStringLiteral("/") + name);
}

double renderAndCompare(OffscreenRenderer& renderer, const std::shared_ptr<PreparedComposite>& composite,
                        const LinearImage& expected, const std::string& name) {
    auto image = renderer.render(composite);
    REQUIRE(image);
    const OutputTransform transform = outputTransformFor(composite->color, OutputRole::Export);
    const std::vector<std::uint8_t> reference = encodeOverBlack(expected, transform);
    savePng(*image, QStringLiteral("effects/%1.png").arg(QString::fromStdString(name)));
    saveReference(reference, kW, kH, QStringLiteral("%1.reference.png").arg(QString::fromStdString(name)));
    return psnrRgb(*image, reference);
}

LayerEffect effect(EffectType type, std::initializer_list<double> params) {
    LayerEffect e;
    e.type = type;
    std::copy(params.begin(), params.end(), e.params.begin());
    return e;
}

/// A glow for the blend modes (C.15): warm stripes, transparent at the left, opaque from x = 240.
std::array<std::uint8_t, 4> glow(int x, int y) {
    return {byte(0.95 - 0.5 * y / double(kH)), byte(0.35 + 0.5 * x / double(kW)), byte(0.6 * ((x / 24) % 2)),
            byte(std::clamp((x - 40) / 200.0, 0.0, 1.0))};
}

} // namespace

TEST_CASE("Effects: each stack effect matches its CPU reference", "[render][gpu][effects]") {
    auto renderer = makeRenderer(OutputRole::Export);
    QDir().mkpath(effectsDir());

    struct Case {
        std::string name;
        Picture picture;
        std::vector<LayerEffect> effects;
        double minPsnr;
    };
    const std::vector<Case> cases = {
        {"crop", busy, {effect(EffectType::Crop, {0.1, 0.2, 0.15, 0.05, 0.0})}, 45.0},
        {"crop_feather", busy, {effect(EffectType::Crop, {0.1, 0.1, 0.1, 0.1, 12.0})}, 45.0},
        {"blur", busy, {effect(EffectType::GaussianBlur, {12.0, 1.0})}, 45.0},
        {"blur_transparent_edges", busy, {effect(EffectType::GaussianBlur, {18.0, 0.0})}, 45.0},
        {"correction", busy, {effect(EffectType::BasicCorrection, {1.0, 30.0, 150.0, 40.0, -20.0})}, 45.0},
        {"correction_flat", busy, {effect(EffectType::BasicCorrection, {-0.5, -60.0, 40.0, -30.0, 25.0})}, 45.0},
        {"vignette", busy, {effect(EffectType::Vignette, {-2.0, 50.0, 0.0, 50.0})}, 45.0},
        {"vignette_round_light", busy, {effect(EffectType::Vignette, {1.5, 30.0, 80.0, 20.0})}, 45.0},
        {"vignette_square", busy, {effect(EffectType::Vignette, {-3.0, 60.0, -70.0, 30.0})}, 45.0},
        {"sharpen", busy, {effect(EffectType::Sharpen, {150.0, 2.0})}, 45.0},
        {"drop_shadow", card, {effect(EffectType::DropShadow, {0.7, 135.0, 12.0, 9.0, 0.1, 0.0, 0.3})}, 45.0},
        {"border", card, {effect(EffectType::Border, {6.0, 1.0, 1.0, 1.0, 1.0})}, 45.0}, // C.15 facecam layouts
        {"border_then_shadow", card,
         {effect(EffectType::Border, {4.0, 0.95, 0.75, 0.1, 0.8}), effect(EffectType::DropShadow, {0.6, 135.0, 10.0, 8.0, 0.0, 0.0, 0.0})},
         45.0},
        {"stack_crop_blur_correction", busy,
         {effect(EffectType::Crop, {0.05, 0.05, 0.05, 0.05, 6.0}), effect(EffectType::GaussianBlur, {6.0, 0.0}),
          effect(EffectType::BasicCorrection, {0.5, 20.0, 120.0, 0.0, 0.0})},
         45.0},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            CompositeLayer layer = layerFrom(c.picture);
            layer.effects = c.effects;
            auto composite = compositeOf({kW, kH}, {layer});
            LinearImage expected = linearFromRgba8(kW, kH, c.picture);
            for (const LayerEffect& e : c.effects) {
                expected = referenceEffect(expected, e);
            }
            const double psnr = renderAndCompare(*renderer, composite, expected, c.name);
            WARN(c.name << ": PSNR " << psnr << " dB");
            CHECK(psnr >= c.minPsnr);
        }
    }
}

TEST_CASE("Effects: a border is the width outside the alpha edge, rounded around corners (C.15)", "[render][gpu][effects]") {
    // The card spans x 80..240, y 45..135: an 8 px white border.
    auto renderer = makeRenderer(OutputRole::Export);
    CompositeLayer layer = layerFrom(card);
    layer.effects = {effect(EffectType::Border, {8.0, 1.0, 1.0, 1.0, 1.0})};
    auto image = renderer->render(compositeOf({kW, kH}, {layer}));
    REQUIRE(image);
    const auto white = [&](int x, int y) { return pixelOf(*image, x, y)[1] > 0.95; };
    const auto black = [&](int x, int y) { return pixelOf(*image, x, y)[1] < 0.05; };
    CHECK(white(76, 90));   // 4 px left of the edge
    CHECK(white(160, 140)); // 5 px below
    CHECK(black(69, 90));   // 11 px left: outside the border
    CHECK(black(160, 147));
    CHECK(white(75, 40));   // 7.1 px from the corner: inside the disc
    CHECK(black(73, 38));   // 9.9 px from the corner: a square dilation would draw here, a rounded one does not
    CHECK_FALSE(white(160, 90)); // the card itself shows, not the border
}

TEST_CASE("Effects: stack order matters and is honoured", "[render][gpu][effects]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const LayerEffect blurE = effect(EffectType::GaussianBlur, {15.0, 1.0});
    const LayerEffect cropE = effect(EffectType::Crop, {0.25, 0.25, 0.25, 0.25, 0.0});
    for (const bool cropFirst : {true, false}) {
        CompositeLayer layer = layerFrom(busy);
        layer.effects = cropFirst ? std::vector<LayerEffect>{cropE, blurE} : std::vector<LayerEffect>{blurE, cropE};
        LinearImage expected = linearFromRgba8(kW, kH, busy);
        for (const LayerEffect& e : layer.effects) {
            expected = referenceEffect(expected, e);
        }
        const double psnr = renderAndCompare(*renderer, compositeOf({kW, kH}, {layer}), expected,
                                             cropFirst ? "order_crop_then_blur" : "order_blur_then_crop");
        INFO("crop first " << cropFirst << ": PSNR " << psnr);
        CHECK(psnr >= 45.0);
    }
    // Crop then blur leaves a soft border; blur then crop a hard one.
    CompositeLayer soft = layerFrom(busy);
    soft.effects = {cropE, blurE};
    CompositeLayer hard = layerFrom(busy);
    hard.effects = {blurE, cropE};
    auto a = renderer->render(compositeOf({kW, kH}, {soft}));
    auto b = renderer->render(compositeOf({kW, kH}, {hard}));
    REQUIRE(a);
    REQUIRE(b);
    const int x = 80 - 3; // just outside the crop edge
    CHECK(pixelOf(*a, x, kH / 2)[0] + pixelOf(*a, x, kH / 2)[1] > 0.05);
    CHECK(pixelOf(*b, x, kH / 2)[0] + pixelOf(*b, x, kH / 2)[1] + pixelOf(*b, x, kH / 2)[2] < 0.01);
}

TEST_CASE("Transitions: dissolve, dips and wipes match their CPU reference", "[render][gpu][effects]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const LinearImage a = linearFromRgba8(kW, kH, busy);
    const LinearImage b = linearFromRgba8(kW, kH, stripes);
    struct Case {
        std::string name;
        LayerTransition mix;
        int sides; // 2, or 1 = A only, -1 = B only
    };
    const auto mixOf = [](TransitionKind kind, double progress, double angle = 0.0, double feather = 0.0,
                          double color = 0.0) {
        LayerTransition m;
        m.kind = kind;
        m.progress = progress;
        m.angleDegrees = angle;
        m.feather = feather;
        m.color = {color, color, color};
        return m;
    };
    const std::vector<Case> cases = {
        {"dissolve_25", mixOf(TransitionKind::CrossDissolve, 0.25), 2},
        {"dissolve_70", mixOf(TransitionKind::CrossDissolve, 0.7), 2},
        {"dip_black_30", mixOf(TransitionKind::DipToColor, 0.3), 2},
        {"dip_black_80", mixOf(TransitionKind::DipToColor, 0.8), 2},
        {"dip_white_40", mixOf(TransitionKind::DipToColor, 0.4, 0.0, 0.0, 1.0), 2},
        {"wipe_hard_50", mixOf(TransitionKind::Wipe, 0.5), 2},
        {"wipe_feather_40", mixOf(TransitionKind::Wipe, 0.4, 0.0, 0.2), 2},
        {"wipe_diagonal_60", mixOf(TransitionKind::Wipe, 0.6, 135.0, 0.1), 2},
        {"dissolve_tail_only_60", mixOf(TransitionKind::CrossDissolve, 0.6), 1},
        {"dissolve_head_only_60", mixOf(TransitionKind::CrossDissolve, 0.6), -1},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            std::vector<CompositeLayer> layers;
            LayerTransition mix = c.mix;
            mix.layerCount = c.sides == 2 ? 2 : 1;
            mix.incomingOnly = c.sides == -1;
            if (c.sides >= 1) {
                layers.push_back(layerFrom(busy));
            }
            if (c.sides == 2 || c.sides == -1) {
                layers.push_back(layerFrom(stripes));
            }
            layers.front().transition = mix;
            const LinearImage expected =
                referenceTransition(c.sides >= 1 ? &a : nullptr, c.sides != 1 ? &b : nullptr, mix);
            const double psnr = renderAndCompare(*renderer, compositeOf({kW, kH}, layers), expected, c.name);
            WARN(c.name << ": PSNR " << psnr << " dB");
            CHECK(psnr >= 45.0);
        }
    }
}

TEST_CASE("Transitions: each side keeps its own effects", "[render][gpu][effects]") {
    auto renderer = makeRenderer(OutputRole::Export);
    CompositeLayer outgoing = layerFrom(busy);
    outgoing.effects = {effect(EffectType::BasicCorrection, {-1.0, 0.0, 0.0, 0.0, 0.0})}; // grey, darker
    CompositeLayer incoming = layerFrom(stripes);
    incoming.effects = {effect(EffectType::GaussianBlur, {9.0, 1.0})};
    LayerTransition mix;
    mix.kind = TransitionKind::CrossDissolve;
    mix.progress = 0.5;
    outgoing.transition = mix;
    const LinearImage a = referenceEffect(linearFromRgba8(kW, kH, busy), outgoing.effects[0]);
    const LinearImage b = referenceEffect(linearFromRgba8(kW, kH, stripes), incoming.effects[0]);
    const double psnr = renderAndCompare(*renderer, compositeOf({kW, kH}, {outgoing, incoming}),
                                         referenceTransition(&a, &b, mix), "dissolve_with_effects");
    INFO("PSNR " << psnr);
    CHECK(psnr >= 45.0);
}

TEST_CASE("Effects: crop is relative to a transformed layer's own frame", "[render][gpu][effects]") {
    auto renderer = makeRenderer(OutputRole::Export);
    // A half-size picture-in-picture, centred: it covers canvas x in [80, 240).
    CompositeLayer pip = layerFrom(busy);
    pip.transform.scale = 0.5;
    pip.effects = {effect(EffectType::Crop, {0.5, 0.0, 0.0, 0.0, 0.0})}; // its left half goes
    auto image = renderer->render(compositeOf({kW, kH}, {pip}));
    REQUIRE(image);
    savePng(*image, QStringLiteral("effects/crop_pip.png"));
    const auto sum = [&](int x, int y) {
        const Rgb c = pixelOf(*image, x, y);
        return c[0] + c[1] + c[2];
    };
    CHECK(sum(100, kH / 2) < 0.01); // left half of the PiP: cropped
    CHECK(sum(200, kH / 2) > 0.1);  // right half: shown
    CHECK(sum(40, kH / 2) < 0.01);  // outside the PiP: nothing
    CHECK(sum(280, kH / 2) < 0.01);
}

TEST_CASE("Effects: blur radius is in sequence pixels at any preview resolution", "[render][gpu][effects]") {
    // A hard vertical edge, blurred with radius 24 px: the 10-90 % rise must span the same sequence distance at
    // full and half preview resolution.
    const auto edge = [](int x, int) -> std::array<std::uint8_t, 4> {
        return x < kW / 2 ? std::array<std::uint8_t, 4>{0, 0, 0, 255} : std::array<std::uint8_t, 4>{255, 255, 255, 255};
    };
    double rise[2] = {0, 0};
    for (int divisor : {1, 2}) {
        auto renderer = makeRenderer(OutputRole::Export);
        renderer->compositor().setPreviewDivisor(divisor);
        CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, edge));
        layer.effects = {effect(EffectType::GaussianBlur, {24.0, 1.0})};
        auto image = renderer->render(compositeOf({kW, kH}, {layer}), QSize(kW, kH));
        REQUIRE(image);
        // Linear-light ramp along the middle row, in output (= sequence) pixels.
        double x10 = -1;
        double x90 = -1;
        for (int x = 1; x < kW; ++x) {
            const double v = transferToLinear(TransferFunction::BT709, pixelOf(*image, x, kH / 2)[0]);
            if (x10 < 0 && v >= 0.1) {
                x10 = x;
            }
            if (x90 < 0 && v >= 0.9) {
                x90 = x;
            }
        }
        rise[divisor - 1] = x90 - x10;
    }
    WARN("10-90 % rise: full " << rise[0] << " px, half " << rise[1] << " px");
    // sigma = 8 px: an erf edge rises 10-90 % over 2.56 sigma = 20.5 px.
    CHECK(std::abs(rise[0] - 20.5) <= 2.0);
    CHECK(std::abs(rise[1] - rise[0]) <= 3.0);
}

TEST_CASE("Effects: bypassed effects pass through and are counted", "[render][gpu][effects]") {
    auto renderer = makeRenderer(OutputRole::Export);
    CompositeLayer plain = layerFrom(busy);
    CompositeLayer bypassed = layerFrom(busy);
    bypassed.bypassedEffects = 2;
    auto a = renderer->render(compositeOf({kW, kH}, {plain}));
    auto b = renderer->render(compositeOf({kW, kH}, {bypassed}));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->rgba == b->rgba);
    auto timing = renderer->renderTimed(compositeOf({kW, kH}, {bypassed}));
    REQUIRE(timing);
    CHECK(timing->composite.bypassedEffects == 2);
    CHECK(timing->composite.effectPasses == 0);

    CompositeLayer withEffects = layerFrom(busy);
    withEffects.effects = {effect(EffectType::GaussianBlur, {6.0, 1.0}), effect(EffectType::DropShadow, {0.5, 90.0, 4.0, 3.0, 0, 0, 0})};
    auto counted = renderer->renderTimed(compositeOf({kW, kH}, {withEffects}));
    REQUIRE(counted);
    CHECK(counted->composite.effectsApplied == 2);
    CHECK(counted->composite.effectPasses == 1 + 2 + 4); // layer, blur (2), shadow (make, blur x2, over)
}

namespace {

/// A green-screen facecam for the Ultra Key (0.1.1): a lit green backdrop with a floor shadow, a skin-toned head with
/// dark hair and a green-spilled rim, a half-transparent strip at the left and a transparent one at the right.
std::array<std::uint8_t, 4> greenScreen(int x, int y) {
    if (x >= kW - 12) {
        return {0, 0, 0, 0};
    }
    const std::uint8_t a = x < 16 ? std::uint8_t(128) : std::uint8_t(255);
    const double dx = (x - 160) / 70.0;
    const double dy = (y - 95) / 80.0;
    const double r = std::sqrt(dx * dx + dy * dy);
    if (r < 0.9) {
        if (y < 40) {
            return {40, 30, 28, a}; // hair, darker than the key
        }
        return {byte(0.86 - 0.1 * dy), byte(0.66 - 0.05 * dy), byte(0.55), a};
    }
    if (r < 1.0) { // the rim: skin mixed with the screen's green
        const double k = (r - 0.9) / 0.1;
        return {byte(0.86 * (1.0 - k) + 0.16 * k), byte(0.66 * (1.0 - k) + 0.78 * k), byte(0.55 * (1.0 - k) + 0.27 * k), a};
    }
    const double light = 0.06 * std::sin(x / 37.0) + 0.04 * (y / double(kH));
    if (y > 150 && x > 120 && x < 260) { // the floor shadow
        return {byte(0.136), byte(0.66 + light), byte(0.23), a}; // the screen at 85 %
    }
    return {byte(0.16 + 0.5 * light), byte(0.78 + light), byte(0.27 + 0.3 * light), a};
}

ref::Image toReference(const LinearImage& image) {
    ref::Image out(image.width, image.height);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            out.setD(x, y, image.at(x, y));
        }
    }
    return out;
}

LinearImage fromReference(const ref::Image& image) {
    LinearImage out(image.width, image.height);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            out.at(x, y) = image.atD(x, y);
        }
    }
    return out;
}

std::vector<std::uint8_t> rgbOf(const RenderedImage& image) {
    std::vector<std::uint8_t> rgb;
    rgb.reserve(image.rgba.size() / 4 * 3);
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        rgb.insert(rgb.end(), {image.rgba[i], image.rgba[i + 1], image.rgba[i + 2]});
    }
    return rgb;
}

double psnr24(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double se = 0.0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        se += d * d;
    }
    const double mse = se / static_cast<double>(std::max<std::size_t>(1, a.size()));
    return mse <= 0.0 ? 100.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// The LayerEffect of reference parameters (EffectType::UltraKey's order: the descriptor's).
LayerEffect ultraKeyEffect(const ref::UltraKeyParams& k) {
    return effect(EffectType::UltraKey,
                  {static_cast<double>(static_cast<int>(k.output)), k.keyColor[0], k.keyColor[1], k.keyColor[2],
                   k.transparency, k.highlight, k.shadow, k.tolerance, k.pedestal, k.choke, k.soften, k.contrast,
                   k.midPoint, k.desaturate, k.range, k.spill, k.luma, k.saturation, k.hue, k.luminance});
}

} // namespace

TEST_CASE("Ultra Key: the GPU key matches the CPU reference (render/reference/UltraKey.h) in every output (0.1.1)",
          "[render][gpu][effects][key]") {
    auto renderer = makeRenderer(OutputRole::Export);
    QDir().mkpath(effectsDir());
    const ref::Rgb screen{0.16, 0.78, 0.27}; // the backdrop's encoded colour, as an eyedropper picks it
    struct Case {
        std::string name;
        ref::UltraKeyParams params;
    };
    std::vector<Case> cases;
    {
        ref::UltraKeyParams k;
        k.keyColor = screen;
        cases.push_back({"ultra_key_defaults", k});
        ref::UltraKeyParams cleanup = k;
        cleanup.choke = 100.0; // 1.6 px at 320 wide
        cleanup.soften = 100.0;
        cleanup.contrast = 30.0;
        cleanup.midPoint = 45.0;
        cases.push_back({"ultra_key_cleanup", cleanup});
        ref::UltraKeyParams tuned = k;
        tuned.transparency = 70.0;
        tuned.highlight = 40.0;
        tuned.shadow = 80.0;
        tuned.tolerance = 35.0;
        tuned.pedestal = 20.0;
        tuned.desaturate = 60.0;
        tuned.range = 70.0;
        tuned.spill = 80.0;
        tuned.luma = 80.0;
        tuned.saturation = 130.0;
        tuned.hue = 25.0;
        tuned.luminance = 90.0;
        cases.push_back({"ultra_key_spill_color", tuned});
        ref::UltraKeyParams alpha = cleanup;
        alpha.output = ref::UltraKeyOutput::AlphaChannel;
        cases.push_back({"ultra_key_alpha_channel", alpha});
        ref::UltraKeyParams color = tuned;
        color.output = ref::UltraKeyOutput::ColorChannel;
        cases.push_back({"ultra_key_color_channel", color});
        ref::UltraKeyParams red = k; // the skin is the key's colour: the red channel is the suppressed one
        red.keyColor = {0.86, 0.66, 0.55};
        cases.push_back({"ultra_key_red_key", red});
        ref::UltraKeyParams blue = tuned;
        blue.keyColor = {0.1, 0.2, 0.85};
        cases.push_back({"ultra_key_blue_key", blue});
    }
    const LinearImage picture = linearFromRgba8(kW, kH, greenScreen);
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            CompositeLayer layer = layerFrom(greenScreen);
            layer.effects = {ultraKeyEffect(c.params)};
            const LinearImage expected = fromReference(ref::ultraKey(toReference(picture), c.params));
            const double psnr = renderAndCompare(*renderer, compositeOf({kW, kH}, {layer}), expected, c.name);
            WARN(c.name << ": PSNR " << psnr << " dB");
            CHECK(psnr >= 45.0);
        }
    }
}

TEST_CASE("Ultra Key: the GPU key of the reference's green-screen card matches its goldens (0.1.1)",
          "[render][gpu][effects][key]") {
    // tests/render_reference/test_ultra_key.cpp's 32 x 32 card (a grey | skin square over the screen, a 6 px linear-light
    // edge ramp), handed to the GPU as 8-bit sRGB like every test layer; the goldens were keyed from its exact values.
    const ref::Rgb screen{0.10, 0.60, 0.20};
    const ref::Rgb grey{0.5, 0.5, 0.5};
    const ref::Rgb skin{0.85, 0.65, 0.55};
    const auto card = [&](int x, int y) {
        const double px = x + 0.5;
        const double py = y + 0.5;
        const double inside = std::min({px - 8.0, 24.0 - px, py - 8.0, 24.0 - py});
        const double c = std::clamp((inside + 3.0) / 6.0, 0.0, 1.0);
        const ref::Rgb& fg = x < 16 ? grey : skin;
        std::array<std::uint8_t, 4> out{0, 0, 0, 255};
        for (std::size_t i = 0; i < 3; ++i) {
            out[i] = byte(ref::linearToSrgb(c * ref::toLinear(fg[i]) + (1.0 - c) * ref::toLinear(screen[i])));
        }
        return out;
    };
    auto renderer = makeRenderer(OutputRole::Export);
    const std::pair<const char*, ref::UltraKeyOutput> goldens[] = {{"ultra_key_composite", ref::UltraKeyOutput::Composite},
                                                                   {"ultra_key_alpha", ref::UltraKeyOutput::AlphaChannel}};
    for (const auto& [name, output] : goldens) {
        const auto golden = ref::readGolden(std::filesystem::path(WABCUT_REFERENCE_GOLDENS) / (std::string(name) + ".wri"));
        REQUIRE(golden);
        ref::UltraKeyParams k;
        k.keyColor = screen;
        k.output = output;
        CompositeLayer layer = layerOf(makeRgbaFrame(32, 32, card));
        layer.effects = {ultraKeyEffect(k)};
        const auto composite = compositeOf({32, 32}, {layer});
        auto image = renderer->render(composite);
        REQUIRE(image);
        const OutputTransform transform = outputTransformFor(composite->color, OutputRole::Export);
        // The golden was keyed from the card's exact values; the 8-bit input moves the steep matte edge a little. So:
        // the GPU against the reference on the same 8-bit input (the shader's error), and against the golden no
        // further than that reference is from it (the input's quantisation).
        const std::vector<std::uint8_t> gpu = rgbOf(*image);
        const std::vector<std::uint8_t> goldenRgb = encodeOverBlack(fromReference(*golden), transform);
        const ref::Image cpu = ref::ultraKey(toReference(linearFromRgba8(32, 32, card)), k);
        const std::vector<std::uint8_t> cpuRgb = encodeOverBlack(fromReference(cpu), transform);
        const double gpuVsCpu = psnr24(gpu, cpuRgb);
        const double gpuVsGolden = psnr24(gpu, goldenRgb);
        const double cpuVsGolden = psnr24(cpuRgb, goldenRgb);
        WARN(name << ": GPU vs the reference on the 8-bit card " << gpuVsCpu << " dB; vs the golden " << gpuVsGolden
                  << " dB (the reference on the 8-bit card vs the golden: " << cpuVsGolden << " dB)");
        CHECK(gpuVsCpu >= 45.0);
        CHECK(gpuVsGolden >= cpuVsGolden - 1.0);
    }
}

TEST_CASE("Ultra Key: the preview's renderer draws the key as the export's does (0.1.1)", "[render][gpu][effects][key]") {
    // One compositor serves the program monitor (OutputRole::Display) and export: the same passes, the same pixels.
    ref::UltraKeyParams k;
    k.keyColor = {0.16, 0.78, 0.27};
    k.choke = 100.0;
    k.soften = 100.0;
    k.spill = 80.0;
    k.hue = 20.0;
    CompositeLayer layer = layerFrom(greenScreen);
    layer.effects = {ultraKeyEffect(k)};
    auto exported = makeRenderer(OutputRole::Export)->render(compositeOf({kW, kH}, {layer}));
    auto previewed = makeRenderer(OutputRole::Display)->render(compositeOf({kW, kH}, {layer}));
    REQUIRE(exported);
    REQUIRE(previewed);
    const double psnr = psnr24(rgbOf(*previewed), rgbOf(*exported));
    WARN("Ultra Key preview vs export: PSNR " << psnr << " dB");
    CHECK(psnr >= 50.0);
}

TEST_CASE("Ultra Key: the key cuts the screen out and keeps the subject (0.1.1)", "[render][gpu][effects][key]") {
    auto renderer = makeRenderer(OutputRole::Export);
    ref::UltraKeyParams k;
    k.keyColor = {0.16, 0.78, 0.27};
    k.output = ref::UltraKeyOutput::AlphaChannel;
    CompositeLayer layer = layerFrom(greenScreen);
    layer.effects = {ultraKeyEffect(k)};
    auto image = renderer->render(compositeOf({kW, kH}, {layer}));
    REQUIRE(image);
    CHECK(pixelOf(*image, 60, 100)[1] < 0.05);  // the screen: transparent
    CHECK(pixelOf(*image, 240, 170)[1] < 0.05); // the floor shadow too
    CHECK(pixelOf(*image, 160, 100)[1] > 0.95); // the face
    CHECK(pixelOf(*image, 160, 25)[1] > 0.95);  // dark hair stays opaque
}

namespace {

/// `top` at `opacity` in `mode` over `bottom`, render/reference/Blend.h (Dissolve keyed by `seed` and `frame`).
LinearImage referenceBlendOver(const LinearImage& bottom, const LinearImage& top, wabcut::BlendMode mode, double opacity,
                               std::uint64_t seed = 0, std::int64_t frame = 0) {
    return fromReference(ref::blend(toReference(bottom), toReference(top), mode, opacity, seed, frame));
}

constexpr std::uint64_t kBlendClip = 0x9E3779B97F4A7C15ull; // Dissolve's seed: every 16-bit word non-zero
constexpr std::int64_t kBlendFrame = 7;

/// A composite at frame kBlendFrame (Dissolve's pattern changes per frame).
std::shared_ptr<PreparedComposite> blendComposite(std::vector<CompositeLayer> layers, CanvasSize canvas = {kW, kH}) {
    auto composite = compositeOf(canvas, std::move(layers));
    composite->frameDuration = Ticks{1000};
    composite->targetTime = Ticks{kBlendFrame * 1000};
    return composite;
}

} // namespace

TEST_CASE("Blend modes: all 27 match render/reference/Blend.h over an opaque and a partly transparent canvas (C.15, 0.1.2)",
          "[render][gpu][effects][blend]") {
    auto renderer = makeRenderer(OutputRole::Export);
    QDir().mkpath(effectsDir());
    const LinearImage glowLinear = linearFromRgba8(kW, kH, glow);
    const std::pair<const char*, Picture> backdrops[] = {{"opaque", busy}, {"card", card}};
    for (const auto& [backdropName, backdrop] : backdrops) {
        const LinearImage below = linearFromRgba8(kW, kH, backdrop);
        for (std::size_t m = 0; m < wabcut::kBlendModeCount; ++m) {
            const auto mode = static_cast<wabcut::BlendMode>(m);
            const std::string name = std::string("blend_") + std::string(wabcut::toString(mode)) + "_over_" + backdropName;
            DYNAMIC_SECTION(name) {
                CompositeLayer top = layerFrom(glow);
                top.opacity = 0.7;
                top.blend = mode;
                top.clipId = kBlendClip;
                const LinearImage expected = referenceBlendOver(below, glowLinear, mode, 0.7, kBlendClip, kBlendFrame);
                const auto composite = blendComposite({layerFrom(backdrop), top});
                const double psnr = renderAndCompare(*renderer, composite, expected, name);
                // The all-or-nothing modes switch where a value crosses a threshold (Dissolve: the hash against alpha;
                // Hard Mix: Cb + Cs against 1; Darker / Lighter Color: two lumas). The canvas is RGBA16F, so a pixel within
                // its precision of the threshold may land on the other side: those modes are checked by how few pixels
                // differ, the others by PSNR.
                const bool stepMode = mode == wabcut::BlendMode::Dissolve || mode == wabcut::BlendMode::HardMix ||
                                      mode == wabcut::BlendMode::DarkerColor || mode == wabcut::BlendMode::LighterColor;
                auto image = renderer->render(composite);
                REQUIRE(image);
                const std::vector<std::uint8_t> gpu = rgbOf(*image);
                const std::vector<std::uint8_t> reference =
                    encodeOverBlack(expected, outputTransformFor(composite->color, OutputRole::Export));
                std::size_t off = 0;
                for (std::size_t i = 0; i + 2 < gpu.size(); i += 3) {
                    int worst = 0;
                    for (std::size_t c = 0; c < 3; ++c) {
                        worst = std::max(worst, std::abs(int(gpu[i + c]) - int(reference[i + c])));
                    }
                    off += worst > 2 ? 1 : 0;
                }
                const double offShare = static_cast<double>(off) / static_cast<double>(gpu.size() / 3);
                WARN(name << ": PSNR " << psnr << " dB, " << off << " pixels off by more than 2 code values");
                if (stepMode) {
                    CHECK(offShare <= 0.002);
                } else {
                    CHECK(psnr >= 45.0);
                }
            }
        }
    }
}

TEST_CASE("Blend modes: Dissolve at an alpha the canvas holds exactly is the reference's pattern (0.1.2)",
          "[render][gpu][effects][blend]") {
    // An opaque layer at 50 %: alpha 0.5 is exact in RGBA16F, so every pixel's hash falls on the reference's side.
    auto renderer = makeRenderer(OutputRole::Export);
    CompositeLayer top = layerFrom(stripes);
    top.opacity = 0.5;
    top.blend = wabcut::BlendMode::Dissolve;
    top.clipId = kBlendClip;
    const LinearImage expected = referenceBlendOver(linearFromRgba8(kW, kH, busy), linearFromRgba8(kW, kH, stripes),
                                                    wabcut::BlendMode::Dissolve, 0.5, kBlendClip, kBlendFrame);
    const double psnr = renderAndCompare(*renderer, blendComposite({layerFrom(busy), top}), expected, "blend_dissolve_exact");
    WARN("Dissolve at 50 %: PSNR " << psnr << " dB");
    CHECK(psnr >= 55.0);
    // Another frame draws another pattern.
    auto other = blendComposite({layerFrom(busy), top});
    other->targetTime = Ticks{(kBlendFrame + 1) * 1000};
    auto a = renderer->render(blendComposite({layerFrom(busy), top}));
    auto b = renderer->render(other);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->rgba != b->rgba);
}

TEST_CASE("Blend modes: stacked shader blends, one on a layer with effects, under a Normal layer (0.1.2)",
          "[render][gpu][effects][blend]") {
    auto renderer = makeRenderer(OutputRole::Export);
    CompositeLayer multiply = layerFrom(glow);
    multiply.blend = wabcut::BlendMode::Multiply;
    multiply.opacity = 0.8;
    CompositeLayer difference = layerFrom(stripes);
    difference.blend = wabcut::BlendMode::Difference;
    difference.opacity = 0.5;
    difference.effects = {effect(EffectType::Crop, {0.1, 0.1, 0.1, 0.1, 0.0})};
    CompositeLayer hue = layerFrom(glow);
    hue.blend = wabcut::BlendMode::Hue;
    CompositeLayer cardOnTop = layerFrom(card);
    LinearImage expected = linearFromRgba8(kW, kH, busy);
    expected = referenceBlendOver(expected, linearFromRgba8(kW, kH, glow), wabcut::BlendMode::Multiply, 0.8);
    expected = referenceBlendOver(expected, referenceEffect(linearFromRgba8(kW, kH, stripes), difference.effects[0]),
                                  wabcut::BlendMode::Difference, 0.5);
    expected = referenceBlendOver(expected, linearFromRgba8(kW, kH, glow), wabcut::BlendMode::Hue, 1.0);
    expected = referenceBlendOver(expected, linearFromRgba8(kW, kH, card), wabcut::BlendMode::Normal, 1.0);
    const auto layers = std::vector<CompositeLayer>{layerFrom(busy), multiply, difference, hue, cardOnTop};
    auto timed = renderer->renderTimed(blendComposite(layers));
    REQUIRE(timed);
    CHECK(timed->composite.shaderBlends == 3);
    CHECK(timed->composite.unsupportedBlends == 0);
    const double psnr = renderAndCompare(*renderer, blendComposite(layers), expected, "blend_stacked");
    WARN("stacked blends: PSNR " << psnr << " dB");
    CHECK(psnr >= 45.0);
}

TEST_CASE("Blend modes: the preview's renderer blends as the export's does (0.1.2)", "[render][gpu][effects][blend]") {
    CompositeLayer overlay = layerFrom(glow);
    overlay.blend = wabcut::BlendMode::Overlay;
    overlay.opacity = 0.7;
    CompositeLayer dissolve = layerFrom(stripes);
    dissolve.blend = wabcut::BlendMode::Dissolve;
    dissolve.opacity = 0.6;
    dissolve.clipId = kBlendClip;
    const auto layers = std::vector<CompositeLayer>{layerFrom(busy), overlay, dissolve};
    auto exported = makeRenderer(OutputRole::Export)->render(blendComposite(layers));
    auto previewed = makeRenderer(OutputRole::Display)->render(blendComposite(layers));
    REQUIRE(exported);
    REQUIRE(previewed);
    const double psnr = psnr24(rgbOf(*previewed), rgbOf(*exported));
    WARN("blend modes preview vs export: PSNR " << psnr << " dB");
    CHECK(psnr >= 50.0);
}

TEST_CASE("Blend modes: GPU cost of a shader-blended layer at 1080p (0.1.2)", "[render][gpu][effects][blend][perf]") {
    OffscreenOptions options;
    options.timestamps = true;
    auto created = OffscreenRenderer::create(options);
    REQUIRE(created);
    OffscreenRenderer& renderer = **created;
    const media::VideoFrame below = makeRgbaFrame(1920, 1080, [](int x, int y) {
        return std::array<std::uint8_t, 4>{byte(x / 1920.0), byte(y / 1080.0), byte(0.5), 255};
    });
    const media::VideoFrame above = makeRgbaFrame(1920, 1080, [](int x, int y) {
        return std::array<std::uint8_t, 4>{byte(0.9 - y / 2160.0), byte(0.3 + x / 3840.0), byte(0.6 * ((x / 48) % 2)), 255};
    });
    constexpr int kLayers = 4;
    const auto timeOf = [&](wabcut::BlendMode mode) {
        std::vector<CompositeLayer> layers{layerOf(below)};
        for (int i = 0; i < kLayers; ++i) {
            CompositeLayer layer = layerOf(above);
            layer.opacity = 0.8;
            layer.blend = mode;
            layers.push_back(layer);
        }
        auto composite = blendComposite(std::move(layers), {1920, 1080});
        std::vector<double> gpu;
        for (int i = 0; i < 35; ++i) {
            auto t = renderer.renderTimed(composite);
            REQUIRE(t);
            if (i >= 5 && t->gpuMs >= 0) {
                gpu.push_back(t->gpuMs);
            }
        }
        std::sort(gpu.begin(), gpu.end());
        return gpu.empty() ? -1.0 : gpu[gpu.size() / 2];
    };
    const double normal = timeOf(wabcut::BlendMode::Normal);
    const double overlay = timeOf(wabcut::BlendMode::Overlay);
    const double hue = timeOf(wabcut::BlendMode::Hue);
    const double dissolve = timeOf(wabcut::BlendMode::Dissolve);
    const auto perLayer = [&](double ms) { return (ms - normal) / kLayers; };
    WARN("1080p, " << kLayers << " layers over a background: Normal " << normal << " ms, Overlay " << overlay << " ms, Hue "
                   << hue << " ms, Dissolve " << dissolve << " ms; per shader-blended layer: Overlay " << perLayer(overlay)
                   << " ms, Hue " << perLayer(hue) << " ms, Dissolve " << perLayer(dissolve) << " ms ("
                   << renderer.adapterName() << ")");
    std::ofstream report((effectsDir() + QStringLiteral("/blend-perf.md")).toStdString(), std::ios::trunc);
    report << "# Shader blend GPU cost, 1920x1080 (" << renderer.backendName() << ", " << renderer.adapterName() << ")\n\n"
           << "Median GPU time of a whole frame, " << kLayers << " layers at 80 % over a background, 30 frames each:\n\n"
           << "| mode | frame GPU ms | per blended layer vs Normal |\n|---|---|---|\n"
           << "| Normal (fixed function) | " << normal << " | - |\n"
           << "| Overlay | " << overlay << " | " << perLayer(overlay) << " |\n"
           << "| Hue | " << hue << " | " << perLayer(hue) << " |\n"
           << "| Dissolve | " << dissolve << " | " << perLayer(dissolve) << " |\n";
    CHECK(normal >= 0.0);
}

namespace {

/// A luma matte: grey from black at the left to white at the right, opaque.
std::array<std::uint8_t, 4> lumaRamp(int x, int) {
    const std::uint8_t v = byte(x / double(kW - 1));
    return {v, v, v, 255};
}

} // namespace

TEST_CASE("Track Matte Key: alpha and luma mattes, reversed, empty and invalid, at its place in the stack (C.15, 0.1.2)",
          "[render][gpu][effects][matte]") {
    // docs/internal/c15-parity-handoff.md: m = alpha or clamp(Lum(premultiplied matte)), 1 - m reversed, layer *= m; an empty
    // matte is transparent black; a matte track that is not valid shows nothing.
    auto renderer = makeRenderer(OutputRole::Export);
    QDir().mkpath(effectsDir());
    struct Case {
        std::string name;
        Picture matte; // null: an empty matte
        bool luma;
        bool reverse;
        bool valid;
        bool blurAfter;
    };
    const std::vector<Case> cases = {
        {"track_matte_alpha", card, false, false, true, false},
        {"track_matte_alpha_reversed", card, false, true, true, false},
        {"track_matte_luma", lumaRamp, true, false, true, false},
        {"track_matte_luma_reversed", lumaRamp, true, true, true, false},
        {"track_matte_empty", nullptr, false, false, true, false},
        {"track_matte_empty_reversed", nullptr, true, true, true, false},
        {"track_matte_invalid_track", card, false, true, false, false},
        {"track_matte_then_blur", card, false, false, true, true},
    };
    const LinearImage picture = linearFromRgba8(kW, kH, busy);
    const LayerEffect blur = effect(EffectType::GaussianBlur, {9.0, 0.0});
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            LayerEffect key = effect(EffectType::TrackMatte, {2.0, c.luma ? 1.0 : 0.0, c.reverse ? 1.0 : 0.0, c.valid ? 1.0 : 0.0});
            if (c.matte) {
                key.matte = compositeOf({kW, kH}, {layerFrom(c.matte)});
            }
            CompositeLayer layer = layerFrom(busy);
            layer.effects = {key};
            if (c.blurAfter) {
                layer.effects.push_back(blur);
            }
            const LinearImage matte = c.matte ? linearFromRgba8(kW, kH, c.matte) : LinearImage(kW, kH);
            LinearImage expected(kW, kH);
            for (int y = 0; y < kH; ++y) {
                for (int x = 0; x < kW; ++x) {
                    const Rgba& mp = matte.at(x, y);
                    double m = c.luma ? std::clamp(0.2126 * mp[0] + 0.7152 * mp[1] + 0.0722 * mp[2], 0.0, 1.0)
                                      : std::clamp(mp[3], 0.0, 1.0);
                    m = c.reverse ? 1.0 - m : m;
                    m = c.valid ? m : 0.0;
                    for (std::size_t i = 0; i < 4; ++i) {
                        expected.at(x, y)[i] = picture.at(x, y)[i] * m;
                    }
                }
            }
            if (c.blurAfter) {
                expected = referenceEffect(expected, blur);
            }
            const double psnr = renderAndCompare(*renderer, compositeOf({kW, kH}, {layer}), expected, c.name);
            WARN(c.name << ": PSNR " << psnr << " dB");
            CHECK(psnr >= 45.0);
        }
    }
}

TEST_CASE("Effects: screenshot grid of effects, transitions and titles", "[render][gpu][effects][grid]") {
    auto renderer = makeRenderer(OutputRole::Export);
    struct Cell {
        QString label;
        std::shared_ptr<PreparedComposite> composite;
    };
    std::vector<Cell> cells;
    const auto single = [&](const QString& label, Picture picture, std::vector<LayerEffect> effects) {
        CompositeLayer layer = layerFrom(picture);
        layer.effects = std::move(effects);
        cells.push_back({label, compositeOf({kW, kH}, {layer})});
    };
    single(QStringLiteral("Original"), busy, {});
    single(QStringLiteral("Crop + feather"), busy, {effect(EffectType::Crop, {0.1, 0.1, 0.1, 0.1, 12.0})});
    single(QStringLiteral("Gaussian Blur 12"), busy, {effect(EffectType::GaussianBlur, {12.0, 1.0})});
    single(QStringLiteral("Basic Correction"), busy, {effect(EffectType::BasicCorrection, {1.0, 30.0, 150.0, 40.0, -20.0})});
    single(QStringLiteral("Vignette"), busy, {effect(EffectType::Vignette, {-2.0, 50.0, 0.0, 50.0})});
    single(QStringLiteral("Sharpen"), busy, {effect(EffectType::Sharpen, {200.0, 2.0})});
    {
        // The card over a light ground, so the (black) shadow shows.
        CompositeLayer ground = layerOf(makeRgbaFrame(kW, kH, [](int, int) {
            return std::array<std::uint8_t, 4>{225, 228, 235, 255};
        }));
        CompositeLayer shadowed = layerFrom(card);
        shadowed.effects = {effect(EffectType::DropShadow, {0.8, 135.0, 12.0, 9.0, 0.0, 0.0, 0.0})};
        cells.push_back({QStringLiteral("Drop Shadow"), compositeOf({kW, kH}, {ground, shadowed})});
    }
    single(QStringLiteral("Crop > Blur > Correction"), busy,
           {effect(EffectType::Crop, {0.05, 0.05, 0.05, 0.05, 6.0}), effect(EffectType::GaussianBlur, {6.0, 0.0}),
            effect(EffectType::BasicCorrection, {0.5, 20.0, 120.0, 0.0, 0.0})});
    const auto transition = [&](const QString& label, TransitionKind kind, double progress, double angle = 0.0,
                                double feather = 0.0, double color = 0.0) {
        LayerTransition mix;
        mix.kind = kind;
        mix.progress = progress;
        mix.angleDegrees = angle;
        mix.feather = feather;
        mix.color = {color, color, color};
        CompositeLayer a = layerFrom(busy);
        a.transition = mix;
        cells.push_back({label, compositeOf({kW, kH}, {a, layerFrom(stripes)})});
    };
    transition(QStringLiteral("Cross Dissolve 50%"), TransitionKind::CrossDissolve, 0.5);
    transition(QStringLiteral("Dip to Black 30%"), TransitionKind::DipToColor, 0.3);
    transition(QStringLiteral("Dip to White 70%"), TransitionKind::DipToColor, 0.7, 0.0, 0.0, 1.0);
    transition(QStringLiteral("Wipe 50%, feather"), TransitionKind::Wipe, 0.5, 0.0, 0.15);
    transition(QStringLiteral("Wipe 135 deg"), TransitionKind::Wipe, 0.55, 135.0, 0.05);
    const auto title = [&](const QString& label, std::string words, double size, double stroke, bool shadow) {
        TitleStyle style;
        style.text = std::move(words);
        style.sizePx = size;
        style.strokeWidth = stroke;
        style.stroke = {0.05, 0.1, 0.4};
        style.fill = {1.0, 0.95, 0.4};
        style.frameWidth = kW;
        style.frameHeight = kH;
        auto raster = TitleRenderer::shared().render(style);
        CompositeLayer text = layerOf(raster->view(raster));
        if (shadow) {
            text.effects = {effect(EffectType::DropShadow, {0.85, 135.0, 5.0, 6.0, 0.0, 0.0, 0.0})};
        }
        CompositeLayer background = layerFrom(stripes);
        background.effects = {effect(EffectType::GaussianBlur, {6.0, 1.0})};
        cells.push_back({label, compositeOf({kW, kH}, {background, text})});
    };
    title(QStringLiteral("Title: stroke"), "WABCUT", 64.0, 4.0, false);
    title(QStringLiteral("Title: Thai + shadow"), "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A",
          56.0, 0.0, true);

    constexpr int columns = 5;
    constexpr int labelHeight = 26;
    const int rows = (static_cast<int>(cells.size()) + columns - 1) / columns;
    QImage grid(columns * kW, rows * (kH + labelHeight), QImage::Format_RGB32);
    grid.fill(QColor(24, 26, 32));
    QPainter painter(&grid);
    QFont font;
    font.setPixelSize(15);
    painter.setFont(font);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        auto image = renderer->render(cells[i].composite);
        REQUIRE(image);
        const int col = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;
        const QImage picture(image->rgba.data(), image->width, image->height, image->width * 4, QImage::Format_RGBA8888);
        painter.drawImage(col * kW, row * (kH + labelHeight) + labelHeight, picture);
        painter.setPen(QColor(230, 230, 235));
        painter.drawText(QRect(col * kW + 8, row * (kH + labelHeight), kW - 16, labelHeight), Qt::AlignVCenter, cells[i].label);
    }
    painter.end();
    CHECK(grid.save(effectsDir() + QStringLiteral("/grid.png")));
}
