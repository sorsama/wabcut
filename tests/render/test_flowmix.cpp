// P.2 on the GPU: the flow mix (shaders/flowmix.frag; Time Interpolation's Optical Flow and Morph Cut) against its CPU
// reference motion::interpolate on the same 8-bit input in linear light, its cross-dissolve fallback, and the layer warp
// of the stabilizers (CompositeLayer::warp). Rendered pairs land in test-output/render/flowmix/.

#include "../motion/MovingTexture.h"
#include "EffectsReference.h"
#include "RenderTestSupport.h"

#include <wabcut/motion/Flow.h>
#include <wabcut/motion/Interpolate.h>
#include <wabcut/render/Compositor.h>

#include <QDir>
#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

std::uint8_t byteOf(double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

/// The moving texture at (dx, dy) as an opaque RGBA8 picture (a tint so the channels differ).
std::function<std::array<std::uint8_t, 4>(int, int)> texturePicture(const motion::test::MovingTexture& t, double dx, double dy) {
    return [&t, dx, dy](int x, int y) {
        const double v = t.at(x - dx, y - dy);
        return std::array<std::uint8_t, 4>{byteOf(v), byteOf(0.2 + 0.6 * v), byteOf(1.0 - v), 255};
    };
}

/// The 8-bit picture as the analysis sees it: its grey (the green channel stands in for luma here).
motion::Plane greyOf(const std::function<std::array<std::uint8_t, 4>(int, int)>& picture) {
    motion::Plane p(kW, kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            p(x, y) = static_cast<float>(picture(x, y)[1] / 255.0);
        }
    }
    return p;
}

motion::RgbaImage toMotion(const LinearImage& in) {
    motion::RgbaImage out(in.width, in.height);
    for (std::size_t i = 0; i < in.px.size(); ++i) {
        for (std::size_t c = 0; c < 4; ++c) {
            out.px[4 * i + c] = static_cast<float>(in.px[i][c]);
        }
    }
    return out;
}

LinearImage fromMotion(const motion::RgbaImage& in) {
    LinearImage out(in.width, in.height);
    for (std::size_t i = 0; i < out.px.size(); ++i) {
        for (std::size_t c = 0; c < 4; ++c) {
            out.px[i][c] = in.px[4 * i + c];
        }
    }
    return out;
}

double renderAndCompare(OffscreenRenderer& renderer, const std::shared_ptr<PreparedComposite>& composite,
                        const LinearImage& expected, const std::string& name) {
    auto image = renderer.render(composite);
    REQUIRE(image);
    const std::vector<std::uint8_t> reference = encodeOverBlack(expected, outputTransformFor(composite->color, OutputRole::Export));
    QDir().mkpath(outputDir() + QStringLiteral("/flowmix"));
    savePng(*image, QStringLiteral("flowmix/%1.png").arg(QString::fromStdString(name)));
    QImage(reference.data(), kW, kH, kW * 3, QImage::Format_RGB888)
        .save(outputDir() + QStringLiteral("/flowmix/%1.reference.png").arg(QString::fromStdString(name)));
    return psnrRgb(*image, reference);
}

} // namespace

TEST_CASE("Flow mix matches its CPU reference on a moving texture", "[render][gpu][motion]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const motion::test::MovingTexture texture(21);
    const auto pa = texturePicture(texture, 0.0, 0.0);
    const auto pb = texturePicture(texture, 7.0, -3.0);
    auto pair = std::make_shared<const motion::FlowPair>(
        motion::estimateFlowPair(greyOf(pa), greyOf(pb), motion::analysisFlowOptions()));
    REQUIRE_FALSE(pair->empty());
    const LinearImage a = linearFromRgba8(kW, kH, pa);
    const LinearImage b = linearFromRgba8(kW, kH, pb);
    for (const double t : {0.25, 0.5, 0.8}) {
        DYNAMIC_SECTION("t " << t) {
            LayerTransition mix;
            mix.kind = TransitionKind::FlowMorph;
            mix.progress = t;
            mix.motion = pair;
            CompositeLayer outgoing = layerOf(makeRgbaFrame(kW, kH, pa));
            outgoing.transition = mix;
            const LinearImage expected = fromMotion(motion::interpolate(toMotion(a), toMotion(b), *pair, t));
            const double psnr = renderAndCompare(*renderer, compositeOf({kW, kH}, {outgoing, layerOf(makeRgbaFrame(kW, kH, pb))}),
                                                 expected, "flow_" + std::to_string(static_cast<int>(t * 100)));
            WARN("flow mix at " << t << ": PSNR " << psnr << " dB");
            CHECK(psnr >= 40.0);
        }
    }
}

TEST_CASE("Flow mix shows the true in-between picture better than a dissolve", "[render][gpu][motion]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const motion::test::MovingTexture texture(5);
    const auto pa = texturePicture(texture, 0.0, 0.0);
    const auto pb = texturePicture(texture, 8.0, 4.0);
    const auto truth = texturePicture(texture, 4.0, 2.0);
    auto pair = std::make_shared<const motion::FlowPair>(
        motion::estimateFlowPair(greyOf(pa), greyOf(pb), motion::analysisFlowOptions()));
    const auto mixed = [&](TransitionKind kind) {
        LayerTransition mix;
        mix.kind = kind;
        mix.progress = 0.5;
        mix.motion = kind == TransitionKind::FlowMorph ? pair : nullptr;
        CompositeLayer outgoing = layerOf(makeRgbaFrame(kW, kH, pa));
        outgoing.transition = mix;
        auto image = renderer->render(compositeOf({kW, kH}, {outgoing, layerOf(makeRgbaFrame(kW, kH, pb))}));
        REQUIRE(image);
        return std::move(*image);
    };
    const std::vector<std::uint8_t> expected = encodeOverBlack(linearFromRgba8(kW, kH, truth),
                                                               outputTransformFor(ColorPipeline{}, OutputRole::Export));
    const double flow = psnrRgb(mixed(TransitionKind::FlowMorph), expected);
    const double dissolve = psnrRgb(mixed(TransitionKind::CrossDissolve), expected);
    INFO("flow " << flow << " dB, dissolve " << dissolve << " dB");
    CHECK(flow > dissolve + 10.0);
}

TEST_CASE("Flow mix without motion or one side mixes as a cross dissolve", "[render][gpu][motion]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const motion::test::MovingTexture texture(8);
    const auto pa = texturePicture(texture, 0.0, 0.0);
    const auto pb = texturePicture(texture, 3.0, 1.0);
    LayerTransition mix;
    mix.kind = TransitionKind::FlowMorph;
    mix.progress = 0.4;
    CompositeLayer outgoing = layerOf(makeRgbaFrame(kW, kH, pa));
    outgoing.transition = mix;
    LayerTransition dissolve = mix;
    dissolve.kind = TransitionKind::CrossDissolve;
    const LinearImage a = linearFromRgba8(kW, kH, pa);
    const LinearImage b = linearFromRgba8(kW, kH, pb);
    const double psnr = renderAndCompare(*renderer, compositeOf({kW, kH}, {outgoing, layerOf(makeRgbaFrame(kW, kH, pb))}),
                                         referenceTransition(&a, &b, dissolve), "flow_without_motion");
    CHECK(psnr >= 45.0);
    // One side (a motion there, but nothing to warp toward): mixed with transparent like a dissolve.
    LayerTransition one = mix;
    one.motion = std::make_shared<const motion::FlowPair>(
        motion::estimateFlowPair(greyOf(pa), greyOf(pb), motion::analysisFlowOptions()));
    one.layerCount = 1;
    CompositeLayer alone = layerOf(makeRgbaFrame(kW, kH, pa));
    alone.transition = one;
    LayerTransition oneDissolve = one;
    oneDissolve.kind = TransitionKind::CrossDissolve;
    CHECK(renderAndCompare(*renderer, compositeOf({kW, kH}, {alone}), referenceTransition(&a, nullptr, oneDissolve),
                           "flow_one_side") >= 45.0);
}

TEST_CASE("A layer's warp moves its frame in fractions of the frame", "[render][gpu][motion]") {
    // A white card on transparent black: warped by a translation of 10 % of the width and a perspective pin.
    auto renderer = makeRenderer(OutputRole::Export);
    const auto card = [](int x, int y) {
        const bool inside = x >= 100 && x < 220 && y >= 50 && y < 130;
        return inside ? std::array<std::uint8_t, 4>{255, 255, 255, 255} : std::array<std::uint8_t, 4>{0, 0, 0, 0};
    };
    CompositeLayer moved = layerOf(makeRgbaFrame(kW, kH, card));
    moved.warp = {1.0, 0.0, 0.1, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    auto image = renderer->render(compositeOf({kW, kH}, {moved}));
    REQUIRE(image);
    CHECK(pixelOf(*image, 125, 90)[0] < 0.05); // left of the moved card (it starts at 132)
    CHECK(pixelOf(*image, 140, 90)[0] > 0.95);
    CHECK(pixelOf(*image, 245, 90)[0] > 0.95); // its right edge moved to 252
    CHECK(pixelOf(*image, 258, 90)[0] < 0.05);
    // Downscaled frames warp the same (the warp is in fractions): a half-size frame placed at full size.
    CompositeLayer half = layerOf(makeRgbaFrame(kW / 2, kH / 2, [&](int x, int y) { return card(2 * x, 2 * y); }));
    half.transform.scale = 2.0;
    half.warp = moved.warp;
    auto scaled = renderer->render(compositeOf({kW, kH}, {half}));
    REQUIRE(scaled);
    CHECK(pixelOf(*scaled, 125, 90)[0] < 0.05);
    CHECK(pixelOf(*scaled, 245, 90)[0] > 0.95);
    // A perspective warp: w grows with x, so the right side shrinks toward the origin.
    CompositeLayer pinned = layerOf(makeRgbaFrame(kW, kH, card));
    pinned.warp = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.5, 0.0, 1.0}; // x' = x / (1 + 0.5 x), y' = y / (1 + 0.5 x)
    auto perspective = renderer->render(compositeOf({kW, kH}, {pinned}));
    REQUIRE(perspective);
    // The card's left edge x = 100/320 maps to 0.3125 / 1.15625 = 0.2703 (86.5 px); its right edge 220/320 to 0.5116
    // (163.7 px); vertically at the left edge 50/180 -> 0.2402 (43 px).
    CHECK(pixelOf(*perspective, 84, 90)[0] < 0.05);
    CHECK(pixelOf(*perspective, 90, 90)[0] > 0.95);
    CHECK(pixelOf(*perspective, 160, 90)[0] > 0.95);
    CHECK(pixelOf(*perspective, 168, 90)[0] < 0.05);
}
