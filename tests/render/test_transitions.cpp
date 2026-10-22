// 0.2: Premiere's transition library on the GPU (shaders/fx_transition.frag) against its CPU twin
// (render/reference/Transitions.h), every kind with its options (border, reverse, anti-aliasing, directions, centre,
// custom settings), data-driven in one test case; plus the ends of every pattern (t = 0 is A, t = 1 is B) and the cost
// at 1080p. Rendered pairs land in test-output/render/transitions/.

#include "EffectsReference.h"
#include "RenderTestSupport.h"

#include <wabcut/render/reference/Transitions.h>

#include <QDir>
#include <QElapsedTimer>
#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

std::uint8_t byte(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

/// A busy opaque picture (gradients, checks, lines): A.
std::array<std::uint8_t, 4> busy(int x, int y) {
    const bool check = ((x / 20) + (y / 20)) % 2 == 0;
    if (x % 40 == 7 || y % 30 == 11) {
        return {245, 245, 240, 255};
    }
    return {byte(x / double(kW)), byte(0.15 + 0.7 * y / double(kH)), check ? std::uint8_t(200) : std::uint8_t(40), 255};
}

/// Stripes in other colours: B.
std::array<std::uint8_t, 4> stripes(int x, int y) {
    return {byte(0.2 + 0.6 * ((x / 16) % 2)), 90, byte(0.9 - 0.7 * y / double(kH)), 255};
}

QString outputFolder() {
    const QString dir = outputDir() + QStringLiteral("/transitions");
    QDir().mkpath(dir);
    return dir;
}

LayerTransition mixOf(TransitionKind kind, double progress) {
    LayerTransition m;
    m.kind = kind;
    m.progress = progress;
    m.seed = 4242;
    return m;
}

struct Case {
    std::string name;
    LayerTransition mix;
};

/// Builders for the option combinations.
LayerTransition with(LayerTransition m, const std::function<void(LayerTransition&)>& change) {
    change(m);
    return m;
}

std::vector<Case> libraryCases() {
    using K = TransitionKind;
    const auto border = [](double width) {
        return [width](LayerTransition& m) {
            m.borderWidth = width;
            m.borderColor = {0.9, 0.85, 0.1};
            m.antiAliasing = 2;
        };
    };
    const auto reverse = [](LayerTransition& m) { m.reverse = true; };
    const auto direction = [](int d) { return [d](LayerTransition& m) { m.direction = d; }; };
    const auto cells = [](double x, double y) { return [x, y](LayerTransition& m) { m.custom = {x, y, 0.0, 0.0}; }; };
    const auto aa = [](int q) { return [q](LayerTransition& m) { m.antiAliasing = q; }; };
    std::vector<Case> cases = {
        // Dissolve.
        {"cross_dissolve_40", mixOf(K::CrossDissolve, 0.4)},
        {"additive_dissolve_30", mixOf(K::AdditiveDissolve, 0.3)},
        {"additive_dissolve_50", mixOf(K::AdditiveDissolve, 0.5)},
        {"non_additive_dissolve_45", mixOf(K::NonAdditiveDissolve, 0.45)},
        {"non_additive_dissolve_70", mixOf(K::NonAdditiveDissolve, 0.7)},
        {"dip_to_color_35", with(mixOf(K::DipToColor, 0.35), [](LayerTransition& m) { m.color = {0.6, 0.05, 0.3}; })},
        // Iris.
        {"iris_box_40", mixOf(K::IrisBox, 0.4)},
        {"iris_box_border_offcentre", with(with(mixOf(K::IrisBox, 0.55), border(6.0)),
                                          [](LayerTransition& m) { m.center = {0.3, 0.7}; })},
        {"iris_cross_30", mixOf(K::IrisCross, 0.3)},
        {"iris_cross_70_reverse", with(mixOf(K::IrisCross, 0.7), reverse)},
        {"iris_diamond_50_high", with(mixOf(K::IrisDiamond, 0.5), aa(3))},
        {"iris_round_35", mixOf(K::IrisRound, 0.35)},
        {"iris_round_border_reverse", with(with(mixOf(K::IrisRound, 0.6), border(4.0)), reverse)},
        // Page Peel.
        {"page_turn_30", mixOf(K::PageTurn, 0.3)},
        {"page_turn_65_se", with(mixOf(K::PageTurn, 0.65), direction(2))},
        {"page_peel_35", mixOf(K::PagePeel, 0.35)},
        {"page_peel_70_ne_reverse", with(with(mixOf(K::PagePeel, 0.7), direction(1)), reverse)},
        // Slide.
        {"band_slide_40", with(mixOf(K::BandSlide, 0.4), cells(7.0, 0.0))},
        {"band_slide_60_vertical", with(with(mixOf(K::BandSlide, 0.6), cells(5.0, 0.0)), direction(1))},
        {"center_split_35", mixOf(K::CenterSplit, 0.35)},
        {"center_split_border", with(mixOf(K::CenterSplit, 0.5), border(5.0))},
        {"push_30", mixOf(K::Push, 0.3)},
        {"push_55_north_to_south", with(mixOf(K::Push, 0.55), direction(1))},
        {"push_70_reverse", with(mixOf(K::Push, 0.7), reverse)},
        {"slide_45", mixOf(K::Slide, 0.45)},
        {"slide_60_diagonal_border", with(with(mixOf(K::Slide, 0.6), direction(5)), border(4.0))},
        {"slide_40_reverse", with(with(mixOf(K::Slide, 0.4), direction(3)), reverse)},
        {"split_40", mixOf(K::Split, 0.4)},
        {"split_60_vertical", with(mixOf(K::Split, 0.6), direction(1))},
        {"whip_35", with(mixOf(K::Whip, 0.35), [](LayerTransition& m) { m.custom[0] = 50.0; })},
        {"whip_62_south_to_north", with(with(mixOf(K::Whip, 0.62), direction(3)),
                                        [](LayerTransition& m) { m.custom[0] = 80.0; })},
        // Wipe.
        {"band_wipe_45", with(mixOf(K::BandWipe, 0.45), cells(7.0, 0.0))},
        {"band_wipe_vertical_border", with(with(with(mixOf(K::BandWipe, 0.3), cells(4.0, 0.0)), direction(1)), border(3.0))},
        {"barn_doors_40", mixOf(K::BarnDoors, 0.4)},
        {"barn_doors_vertical_reverse", with(with(mixOf(K::BarnDoors, 0.65), direction(1)), reverse)},
        {"checker_wipe_30", with(mixOf(K::CheckerWipe, 0.3), cells(8.0, 6.0))},
        {"checker_wipe_70_east_to_west", with(with(mixOf(K::CheckerWipe, 0.7), cells(5.0, 3.0)), direction(2))},
        {"checker_board_25", with(mixOf(K::CheckerBoard, 0.25), cells(8.0, 6.0))},
        {"checker_board_65_north", with(with(mixOf(K::CheckerBoard, 0.65), cells(6.0, 4.0)), direction(1))},
        {"clock_wipe_30", mixOf(K::ClockWipe, 0.3)},
        {"clock_wipe_80_from_east_border", with(with(mixOf(K::ClockWipe, 0.8), direction(1)), border(4.0))},
        {"gradient_wipe_linear_soft", with(mixOf(K::GradientWipe, 0.45), cells(0.0, 10.0))},
        {"gradient_wipe_radial_hard", with(mixOf(K::GradientWipe, 0.55), cells(2.0, 0.0))},
        {"gradient_wipe_diagonal_reverse", with(with(mixOf(K::GradientWipe, 0.35), cells(3.0, 25.0)), reverse)},
        {"inset_40", mixOf(K::Inset, 0.4)},
        {"inset_60_from_southeast_border", with(with(mixOf(K::Inset, 0.6), direction(2)), border(5.0))},
        {"paint_splatter_35", mixOf(K::PaintSplatter, 0.35)},
        {"paint_splatter_75", mixOf(K::PaintSplatter, 0.75)},
        {"pinwheel_30", with(mixOf(K::Pinwheel, 0.3), cells(8.0, 0.0))},
        {"pinwheel_60_three_wedges", with(with(mixOf(K::Pinwheel, 0.6), cells(3.0, 0.0)), aa(1))},
        {"radial_wipe_40", mixOf(K::RadialWipe, 0.4)},
        {"radial_wipe_70_from_southwest", with(mixOf(K::RadialWipe, 0.7), direction(3))},
        {"random_blocks_40", with(mixOf(K::RandomBlocks, 0.4), cells(10.0, 10.0))},
        {"random_blocks_75", with(mixOf(K::RandomBlocks, 0.75), cells(16.0, 9.0))},
        {"random_wipe_35", mixOf(K::RandomWipe, 0.35)},
        {"random_wipe_60_north_to_south", with(mixOf(K::RandomWipe, 0.6), direction(1))},
        {"spiral_boxes_30", with(mixOf(K::SpiralBoxes, 0.3), cells(8.0, 6.0))},
        {"spiral_boxes_85", with(mixOf(K::SpiralBoxes, 0.85), cells(5.0, 4.0))},
        {"venetian_blinds_40", with(with(mixOf(K::VenetianBlinds, 0.4), cells(8.0, 0.0)), direction(1))},
        {"venetian_blinds_west_to_east", with(with(mixOf(K::VenetianBlinds, 0.6), cells(6.0, 0.0)), direction(0))},
        {"wedge_wipe_35", mixOf(K::WedgeWipe, 0.35)},
        {"wedge_wipe_70_from_west_border", with(with(mixOf(K::WedgeWipe, 0.7), direction(3)), border(3.0))},
        {"wipe_border_reverse", with(with(with(mixOf(K::Wipe, 0.4), border(6.0)), reverse),
                                     [](LayerTransition& m) { m.angleDegrees = 30.0; })},
        {"zig_zag_blocks_40", with(mixOf(K::ZigZagBlocks, 0.4), cells(8.0, 6.0))},
        {"zig_zag_blocks_south_to_north", with(with(mixOf(K::ZigZagBlocks, 0.55), cells(6.0, 5.0)), direction(3))},
        // Zoom and the creator transitions.
        {"cross_zoom_30", mixOf(K::CrossZoom, 0.3)},
        {"cross_zoom_52", mixOf(K::CrossZoom, 0.52)},
        {"cross_zoom_75_offcentre", with(mixOf(K::CrossZoom, 0.75), [](LayerTransition& m) { m.center = {0.35, 0.4}; })},
        {"zoom_blur_in_35", with(mixOf(K::ZoomBlur, 0.35), [](LayerTransition& m) { m.custom[0] = 60.0; })},
        {"zoom_blur_out_62", with(with(mixOf(K::ZoomBlur, 0.62), direction(1)), [](LayerTransition& m) { m.custom[0] = 40.0; })},
        {"light_leak_40", with(mixOf(K::LightLeak, 0.4), [](LayerTransition& m) {
             m.color = {1.0, 0.27, 0.03};
             m.custom[0] = 1.0;
         })},
        {"flash_45", with(mixOf(K::Flash, 0.45), [](LayerTransition& m) {
             m.color = {1.0, 1.0, 1.0};
             m.custom[0] = 1.0;
         })},
        {"glitch_37", with(mixOf(K::Glitch, 0.37), [](LayerTransition& m) { m.custom = {70.0, 12.0, 0.0, 0.0}; })},
        {"glitch_58", with(mixOf(K::Glitch, 0.58), [](LayerTransition& m) { m.custom = {100.0, 20.0, 0.0, 0.0}; })},
    };
    return cases;
}

} // namespace

TEST_CASE("Transition library: every kind and option matches its CPU reference (0.2)", "[render][gpu][transitions]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const QString folder = outputFolder();
    const LinearImage a = linearFromRgba8(kW, kH, busy);
    const LinearImage b = linearFromRgba8(kW, kH, stripes);
    for (const Case& c : libraryCases()) {
        DYNAMIC_SECTION(c.name) {
            std::vector<CompositeLayer> layers{layerOf(makeRgbaFrame(kW, kH, busy)), layerOf(makeRgbaFrame(kW, kH, stripes))};
            layers.front().transition = c.mix;
            auto image = renderer->render(compositeOf({kW, kH}, layers));
            REQUIRE(image);
            const LinearImage expected = referenceTransition(&a, &b, c.mix);
            const std::vector<std::uint8_t> rgb = encodeOverBlack(expected, outputTransformFor(ColorPipeline{}, OutputRole::Export));
            savePng(*image, QStringLiteral("transitions/%1.png").arg(QString::fromStdString(c.name)));
            QImage(rgb.data(), kW, kH, kW * 3, QImage::Format_RGB888)
                .save(folder + QStringLiteral("/%1.reference.png").arg(QString::fromStdString(c.name)));
            const RenderMatch m = compareToReference(*image, rgb);
            WARN(c.name << ": PSNR " << m.psnr << " dB, " << m.mismatched * 100.0 << " % of pixels off");
            CHECK(m.psnr >= 40.0);
            CHECK(m.mismatched <= 0.003);
        }
    }
}

TEST_CASE("Transition library: one-sided transitions mix with transparent (0.2)", "[render][gpu][transitions]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const LinearImage a = linearFromRgba8(kW, kH, busy);
    const LinearImage b = linearFromRgba8(kW, kH, stripes);
    for (const auto kind : {TransitionKind::IrisRound, TransitionKind::Push, TransitionKind::PagePeel}) {
        for (const bool incoming : {false, true}) {
            LayerTransition mix = mixOf(kind, 0.4);
            mix.layerCount = 1;
            mix.incomingOnly = incoming;
            CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, incoming ? stripes : busy));
            layer.transition = mix;
            auto image = renderer->render(compositeOf({kW, kH}, {layer}));
            REQUIRE(image);
            const LinearImage expected = referenceTransition(incoming ? nullptr : &a, incoming ? &b : nullptr, mix);
            const RenderMatch m = compareToReference(*image, encodeOverBlack(expected, outputTransformFor(ColorPipeline{}, OutputRole::Export)));
            INFO("kind " << static_cast<int>(kind) << (incoming ? " incoming" : " outgoing") << ": PSNR " << m.psnr);
            CHECK(m.psnr >= 40.0);
            CHECK(m.mismatched <= 0.003);
        }
    }
}

TEST_CASE("Transition library: every pattern starts as A and ends as B, whatever its options (0.2)",
          "[render][transitions]") {
    // The CPU reference alone (it is the GPU's oracle above): t = 0 is exactly A, t = 1 exactly B for every kind with
    // the widest anti-aliasing and a border, also reversed. Dissolves through a colour, the flash and the light leak are
    // A and B at the ends too.
    namespace ref = wabcut::render::reference;
    const LinearImage la = linearFromRgba8(64, 36, busy);
    const LinearImage lb = linearFromRgba8(64, 36, stripes);
    const ref::Image a = toReference(la);
    const ref::Image b = toReference(lb);
    for (int k = 0; k <= static_cast<int>(TransitionKind::Glitch); ++k) {
        for (const bool reversed : {false, true}) {
            ref::TransitionSpec spec;
            spec.pattern = static_cast<ref::TransitionPattern>(k);
            spec.borderWidth = 6.0;
            spec.borderColor = {1.0, 0.0, 0.0};
            spec.antiAliasing = 3;
            spec.reverse = reversed;
            spec.custom = {7.0, 5.0, 0.0, 0.0};
            spec.color = {1.0, 1.0, 1.0};
            spec.seed = 17;
            if (spec.pattern == ref::TransitionPattern::GradientWipe) {
                spec.custom = {2.0, 20.0, 0.0, 0.0};
            }
            INFO("kind " << k << (reversed ? " reversed" : ""));
            spec.progress = 0.0;
            CHECK(ref::compare(ref::transition(&a, &b, spec), a).maxAbs < 1e-6);
            spec.progress = 1.0;
            CHECK(ref::compare(ref::transition(&a, &b, spec), b).maxAbs < 1e-6);
        }
    }
}

TEST_CASE("Transition library: GPU cost of a transition at 1080p (0.2)", "[render][gpu][transitions][perf]") {
    // Each transition is one pass of fx_transition.frag over the canvas (the heaviest: Whip's 24 taps, the zooms' 32).
    auto renderer = makeRenderer(OutputRole::Export);
    const auto full = [](int x, int y) { return busy(x % kW, y % kH); };
    for (const auto kind : {TransitionKind::IrisRound, TransitionKind::PaintSplatter, TransitionKind::Whip,
                            TransitionKind::CrossZoom, TransitionKind::PagePeel}) {
        std::vector<CompositeLayer> layers{layerOf(makeRgbaFrame(1920, 1080, full)), layerOf(makeRgbaFrame(1920, 1080, full))};
        layers.front().transition = mixOf(kind, 0.45);
        layers.front().transition->custom[0] = 50.0;
        auto composite = compositeOf({1920, 1080}, layers);
        REQUIRE(renderer->render(composite)); // warm up: pipelines, uploads
        QElapsedTimer timer;
        timer.start();
        constexpr int kFrames = 10;
        for (int i = 0; i < kFrames; ++i) {
            REQUIRE(renderer->render(composite));
        }
        const double ms = timer.nsecsElapsed() / 1e6 / kFrames;
        WARN("transition kind " << static_cast<int>(kind) << ": " << ms << " ms per 1080p frame (render + readback)");
        CHECK(ms < 200.0);
    }
}
