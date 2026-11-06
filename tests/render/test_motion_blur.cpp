// Motion Blur (0.7, model/MotionBlur.h) on the real GPU: a layer with CompositeLayer::blur is drawn once per instant of the
// shutter and the draws averaged. Against the CPU reference (render/reference/MotionBlur.h) and hand-counted coverage.

#include "RenderTestSupport.h"

#include <wabcut/render/Compositor.h>
#include <wabcut/render/OffscreenRenderer.h>
#include <wabcut/render/reference/MotionBlur.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;
using Catch::Approx;

namespace {

constexpr int kCanvasWidth = 128;
constexpr int kCanvasHeight = 64;
constexpr int kSquare = 16;

media::VideoFrame whiteSquare() {
    return makeRgbaFrame(kSquare, kSquare, [](int, int) { return std::array<std::uint8_t, 4>{255, 255, 255, 255}; });
}

ColorPipeline linearOut() {
    ColorPipeline linear;
    linear.exportTransform = OutputTransform::Linear;
    return linear;
}

LayerTransform at(double x, double y = 0.0) {
    LayerTransform t;
    t.position = {x, y};
    return t;
}

/// The instants of a move from -30 to +30 pixels in 5 pixel steps: 13 placements.
std::vector<LayerTransform> sweep() {
    std::vector<LayerTransform> out;
    for (int i = 0; i < 13; ++i)
        out.push_back(at(-30.0 + 5.0 * i));
    return out;
}

RenderedImage draw(OffscreenRenderer& renderer, const std::vector<LayerTransform>& blur, double opacity = 1.0) {
    CompositeLayer layer = layerOf(whiteSquare(), blur.empty() ? LayerTransform{} : blur[blur.size() / 2], opacity);
    layer.blur = blur;
    auto image = renderer.render(compositeOf({kCanvasWidth, kCanvasHeight}, {std::move(layer)}, linearOut()));
    REQUIRE(image.has_value());
    return std::move(*image);
}

/// The reference average of the same instants, as RGB per pixel (linear, over black).
reference::Image referenceOf(const std::vector<LayerTransform>& blur, double opacity = 1.0) {
    reference::Image square = reference::Image::filled(kSquare, kSquare, {1.0f, 1.0f, 1.0f, 1.0f});
    std::vector<reference::Placement2D> placements;
    for (const LayerTransform& t : blur) {
        const Affine2D m = layerToCanvas(t, kSquare, kSquare, 1.0, CanvasSize{kCanvasWidth, kCanvasHeight});
        placements.push_back({m.a, m.b, m.c, m.d, m.e, m.f});
    }
    reference::Image average = reference::motionBlurred(square, kCanvasWidth, kCanvasHeight, placements);
    for (float& v : average.rgba)
        v = static_cast<float>(v * opacity);
    return average;
}

double largestDifference(const RenderedImage& gpu, const reference::Image& cpu) {
    double worst = 0.0;
    for (int y = 0; y < kCanvasHeight; ++y) {
        for (int x = 0; x < kCanvasWidth; ++x) {
            const Rgb got = pixelOf(gpu, x, y);
            const reference::Pixel want = cpu.at(x, y);
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(got[static_cast<std::size_t>(c)] - static_cast<double>(want[static_cast<std::size_t>(c)])));
        }
    }
    return worst;
}

} // namespace

TEST_CASE("Motion Blur: no instants, or one, draws the layer exactly as before", "[render][gpu][motionblur]") {
    auto renderer = makeRenderer();
    const RenderedImage plain = draw(*renderer, {});
    CHECK(pixelOf(plain, 64, 32)[0] == Approx(1.0).margin(1.0 / 255.0));
    // One instant is no blur: the same bytes (the blurred path is for two or more).
    const RenderedImage single = draw(*renderer, {at(0.0)});
    CHECK(single.rgba == plain.rgba);
    // Two identical instants average to the same picture (each carries half the layer).
    const RenderedImage twice = draw(*renderer, {at(0.0), at(0.0)});
    CHECK(largestDifference(twice, referenceOf({at(0.0)})) <= 1.5 / 255.0);
    // A layer that moves less than the pixel grid sees leaves the picture alone in the evaluator; here a blur of 2 instants
    // 0.01 pixel apart is still the same square to within the 8-bit output.
    const RenderedImage tiny = draw(*renderer, {at(-0.005), at(0.005)});
    CHECK(largestDifference(tiny, referenceOf({at(0.0)})) <= 2.5 / 255.0);
}

TEST_CASE("Motion Blur: a moving square smears by the share of the instants that cover each pixel", "[render][gpu][motionblur]") {
    auto renderer = makeRenderer();
    const std::vector<LayerTransform> blur = sweep();
    const RenderedImage image = draw(*renderer, blur);
    CHECK(savePng(image, QStringLiteral("motion_blur_sweep.png")));

    // Hand-counted: 13 instants, the square (16 wide) centred at 64 + p for p = -30 ... +30 step 5 covers [56 + p, 72 + p).
    const auto share = [&](int x) { return pixelOf(image, x, 32)[0]; };
    CHECK(share(64) == Approx(3.0 / 13.0).margin(0.02)); // p = -5, 0, 5 cover pixel 64 (centre 64.5)
    CHECK(share(30) == Approx(1.0 / 13.0).margin(0.02)); // only p = -30
    CHECK(share(100) == Approx(1.0 / 13.0).margin(0.02)); // only p = +30
    CHECK(share(20) == Approx(0.0).margin(0.01));        // left of the smear (it starts at 26)
    CHECK(share(108) == Approx(0.0).margin(0.01));       // right of it (it ends at 102)
    CHECK(pixelOf(image, 64, 10)[0] == Approx(0.0).margin(0.01)); // above the square's rows (24 .. 40)
    // The smear is as wide as the path plus the square: 60 + 16 = 76 pixels of non-black pixels on the middle row.
    int lit = 0;
    for (int x = 0; x < kCanvasWidth; ++x)
        lit += share(x) > 0.5 / 255.0 ? 1 : 0;
    CHECK(lit >= 75);
    CHECK(lit <= 78);

    // The whole picture against the CPU reference of the same instants.
    CHECK(largestDifference(image, referenceOf(blur)) <= 0.03);
}

TEST_CASE("Motion Blur: opacity scales the average and the layer's own placement is not drawn", "[render][gpu][motionblur]") {
    auto renderer = makeRenderer();
    const std::vector<LayerTransform> blur = sweep();
    const RenderedImage half = draw(*renderer, blur, 0.5);
    CHECK(pixelOf(half, 64, 32)[0] == Approx(0.5 * 3.0 / 13.0).margin(0.02));
    CHECK(largestDifference(half, referenceOf(blur, 0.5)) <= 0.03);
    // The layer's `transform` (here the middle instant) is only its placement for effects: a blur that does not include
    // that place leaves it dark.
    CompositeLayer layer = layerOf(whiteSquare(), at(0.0), 1.0);
    layer.blur = {at(-50.0), at(-40.0)};
    auto image = renderer->render(compositeOf({kCanvasWidth, kCanvasHeight}, {std::move(layer)}, linearOut()));
    REQUIRE(image.has_value());
    CHECK(pixelOf(*image, 64, 32)[0] == Approx(0.0).margin(0.01));
}

TEST_CASE("Motion Blur: scale and rotation instants blur too, and a layer above stays sharp", "[render][gpu][motionblur]") {
    auto renderer = makeRenderer();
    // A zoom from 100 % to 200 % across the shutter, on a layer below an unblurred square on top.
    std::vector<LayerTransform> zoom;
    for (int i = 0; i < 9; ++i) {
        LayerTransform t;
        t.scale = 1.0 + 0.125 * i;
        zoom.push_back(t);
    }
    CompositeLayer below = layerOf(whiteSquare(), zoom[4], 1.0);
    below.blur = zoom;
    media::VideoFrame red = makeRgbaFrame(8, 8, [](int, int) { return std::array<std::uint8_t, 4>{255, 0, 0, 255}; });
    CompositeLayer above = layerOf(red, at(0.0), 1.0);
    auto image = renderer->render(compositeOf({kCanvasWidth, kCanvasHeight}, {std::move(below), std::move(above)}, linearOut()));
    REQUIRE(image.has_value());
    CHECK(savePng(*image, QStringLiteral("motion_blur_zoom.png")));
    // The red square (8 x 8) is untouched in the middle; the white zoom shows around it with a soft edge: its half width
    // runs 8 .. 16 pixels over the nine instants, so a pixel 6 px from the centre is covered by all of them and one 14 px
    // out only by the last two (half widths 14.0 and 16.0 ... of 9).
    CHECK(pixelOf(*image, 64, 32)[0] == Approx(1.0).margin(0.01));
    CHECK(pixelOf(*image, 64, 32)[1] == Approx(0.0).margin(0.01)); // red, the layer above
    const double inner = pixelOf(*image, 64 + 6, 32)[1];   // centre 6.5 px out: inside the smallest square (half width 8)
    const double outer = pixelOf(*image, 64 + 14, 32)[1];  // centre 14.5 px out: only the scales above 1.8125 (1.875, 2.0)
    CHECK(inner == Approx(1.0).margin(0.02));
    CHECK(outer == Approx(2.0 / 9.0).margin(0.03));
    // Rotation: a quarter turn over the shutter smears a square into a disc-like shape (corners reach further than the sides).
    std::vector<LayerTransform> spin;
    for (int i = 0; i < 12; ++i) {
        LayerTransform t;
        t.rotationDegrees = 90.0 * i / 11.0;
        spin.push_back(t);
    }
    CompositeLayer turning = layerOf(whiteSquare(), spin[0], 1.0);
    turning.blur = spin;
    auto turned = renderer->render(compositeOf({kCanvasWidth, kCanvasHeight}, {std::move(turning)}, linearOut()));
    REQUIRE(turned.has_value());
    CHECK(pixelOf(*turned, 64, 32)[0] == Approx(1.0).margin(0.02)); // the centre stays covered by every instant
    const double corner = pixelOf(*turned, 64 + 7, 32 + 7)[0];     // inside the square's corner only near 0 and 90 degrees
    CHECK(corner < 0.5);
    CHECK(corner > 0.05);
}
