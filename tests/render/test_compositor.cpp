// Compositor on the real GPU (D3D11 offscreen on Windows, no window): pixel formats, matrices and ranges,
// alpha, transforms, opacity and layer order. Expected values come from the standards' forward equations and
// hand-computed geometry, plus the CPU reference (ColorPipeline.h) for exact agreement.

#include "RenderTestSupport.h"

#include <wabcut/render/Compositor.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;
using media::PixelFormat;

namespace {

constexpr int kBarWidth = 64;
constexpr int kBarsWidth = 8 * kBarWidth;
constexpr int kBarsHeight = 64;
constexpr double kTol = 2.5 / 255.0; // 8-bit quantization on both sides

const ColorInfo kRec709Limited{ColorMatrix::BT709, ColorRange::Limited, ColorPrimaries::BT709,
                               TransferFunction::BT709, ChromaLocation::Left};

Rgb bars(int x, int) {
    return barColor(x / kBarWidth);
}

// Independent transfer formulas (not the library's).
double bt709Decode(double v) {
    return v < 0.081 ? v / 4.5 : std::pow((v + 0.099) / 1.099, 1.0 / 0.45);
}
double bt709Encode(double l) {
    return l < 0.018 ? 4.5 * l : 1.099 * std::pow(l, 0.45) - 0.099;
}
double srgbDecode(double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
double srgbEncode(double l) {
    return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}
Rgb map(const Rgb& c, double (*f)(double)) {
    return {f(c[0]), f(c[1]), f(c[2])};
}

RenderedImage renderOne(OffscreenRenderer& renderer, std::shared_ptr<PreparedComposite> composite,
                        std::optional<QSize> size = {}) {
    auto image = renderer.render(composite, size);
    REQUIRE(image.has_value());
    return std::move(*image);
}

void checkPixel(const RenderedImage& image, int x, int y, const Rgb& expected, double tol = kTol) {
    const Rgb got = pixelOf(image, x, y);
    INFO("pixel (" << x << ", " << y << ") got " << describe(got) << " expected " << describe(expected));
    CHECK(maxDiff(got, expected) <= tol);
}

} // namespace

TEST_CASE("Colour bars decode correctly through every pixel format", "[render][gpu][formats]") {
    const PixelFormat format =
        GENERATE(PixelFormat::NV12, PixelFormat::P010, PixelFormat::YUV420P, PixelFormat::YUV420P10,
                 PixelFormat::YUV422P, PixelFormat::YUV422P10, PixelFormat::YUV444P, PixelFormat::YUV444P10,
                 PixelFormat::YUVA420P, PixelFormat::YUVA444P10);
    INFO("format " << media::pixelFormatName(format));
    auto renderer = makeRenderer();
    INFO("backend " << renderer->backendName() << " on " << renderer->adapterName());
    const media::VideoFrame frame = makeFrame(format, kBarsWidth, kBarsHeight, kRec709Limited, bars);

    // Export through Rec.709: a Rec.709 source passes through, so the output equals the source R'G'B'.
    auto composite = compositeOf({kBarsWidth, kBarsHeight}, {layerOf(frame)});
    const RenderedImage rec709 = renderOne(*renderer, composite);
    // A managed sRGB output: decode BT.709, encode sRGB.
    ColorPipeline srgb;
    srgb.exportTransform = OutputTransform::SRGB;
    const RenderedImage srgbImage =
        renderOne(*renderer, compositeOf({kBarsWidth, kBarsHeight}, {layerOf(frame)}, srgb));

    const SampleEncoding encoding = media::pixelFormatBitDepth(format) == 8 ? SampleEncoding::Bits8
                                    : format == PixelFormat::P010           ? SampleEncoding::Bits10High
                                                                            : SampleEncoding::Bits10Low;
    const ResolvedColor resolved = resolveSourceColor(kRec709Limited, ColorPipeline{}, false);
    const int bits = media::pixelFormatBitDepth(format);
    const double sampleMax = bits == 8 ? 255.0 : 65535.0;
    const double codeScale = format == PixelFormat::P010 ? 64.0 : 1.0;
    for (int bar = 0; bar < 8; ++bar) {
        const int x = bar * kBarWidth + kBarWidth / 2;
        const Rgb source = barColor(bar);
        INFO("bar " << bar);
        checkPixel(rec709, x, kBarsHeight / 2, source);
        checkPixel(srgbImage, x, kBarsHeight / 2, map(map(source, bt709Decode), srgbEncode));
        // Exact agreement with the CPU reference of the same code values.
        const auto codes = encodeYuv(source, ColorMatrix::BT709, ColorRange::Limited, bits);
        const Rgb reference =
            referencePixel({codes[0] * codeScale / sampleMax, codes[1] * codeScale / sampleMax,
                            codes[2] * codeScale / sampleMax},
                           1.0, resolved, encoding, false, OutputTransform::Rec709);
        checkPixel(rec709, x, kBarsHeight / 2, reference, 1.01 / 255.0);
    }
    if (format == PixelFormat::NV12) {
        CHECK(savePng(rec709, QStringLiteral("bars_nv12_rec709.png")));
        CHECK(savePng(srgbImage, QStringLiteral("bars_nv12_srgb.png")));
    }
}

TEST_CASE("RGBA8 frames are sRGB-encoded with straight alpha", "[render][gpu][formats]") {
    auto renderer = makeRenderer();
    const media::VideoFrame frame = makeRgbaFrame(kBarsWidth, kBarsHeight, [](int x, int) {
        const Rgb c = barColor(x / kBarWidth);
        return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(std::lround(c[0] * 255)),
                                           static_cast<std::uint8_t>(std::lround(c[1] * 255)),
                                           static_cast<std::uint8_t>(std::lround(c[2] * 255)), 255};
    });
    ColorPipeline srgb;
    srgb.exportTransform = OutputTransform::SRGB;
    const RenderedImage same =
        renderOne(*renderer, compositeOf({kBarsWidth, kBarsHeight}, {layerOf(frame)}, srgb));
    const RenderedImage rec709 =
        renderOne(*renderer, compositeOf({kBarsWidth, kBarsHeight}, {layerOf(frame)}));
    for (int bar = 0; bar < 8; ++bar) {
        const int x = bar * kBarWidth + kBarWidth / 2;
        const Rgb c = barColor(bar);
        const auto level = [](double v) { return static_cast<double>(std::lround(v * 255)) / 255.0; };
        const Rgb quantized{level(c[0]), level(c[1]), level(c[2])};
        checkPixel(same, x, 10, quantized, 1.01 / 255.0);
        checkPixel(rec709, x, 10, map(map(quantized, srgbDecode), bt709Encode));
    }
}

TEST_CASE("YUVA planar frames carry straight alpha (8- and 10-bit)", "[render][gpu][formats]") {
    const int bits = GENERATE(8, 10);
    auto renderer = makeRenderer();
    const FrameView view = makeYuvaView(
        64, 64, bits, kRec709Limited, [](int, int) { return Rgb{0.75, 0.75, 0.75}; },
        [](int x, int) { return x < 32 ? 1.0 : 0.5; });
    const RenderedImage image = renderOne(*renderer, compositeOf({64, 64}, {layerOf(view)}));
    checkPixel(image, 16, 32, {0.75, 0.75, 0.75});
    // Half alpha over black: linear light halves.
    const double half = bt709Encode(0.5 * bt709Decode(0.75));
    checkPixel(image, 48, 32, {half, half, half});
}

TEST_CASE("BT.601 vs BT.709 matrices and limited vs full range", "[render][gpu][color]") {
    auto renderer = makeRenderer();
    const ColorInfo rec601{ColorMatrix::BT601, ColorRange::Limited, ColorPrimaries::BT709,
                           TransferFunction::BT709, ChromaLocation::Left};
    const ColorInfo fullRange{ColorMatrix::BT709, ColorRange::Full, ColorPrimaries::BT709,
                              TransferFunction::BT709, ChromaLocation::Left};

    const auto renderFrame = [&](const media::VideoFrame& frame) {
        return renderOne(*renderer, compositeOf({kBarsWidth, kBarsHeight}, {layerOf(frame)}));
    };
    const RenderedImage as601 =
        renderFrame(makeFrame(PixelFormat::YUV444P, kBarsWidth, kBarsHeight, rec601, bars));
    const RenderedImage asFull =
        renderFrame(makeFrame(PixelFormat::YUV444P, kBarsWidth, kBarsHeight, fullRange, bars));
    // Rec.709 code values tagged as BT.601: decoded with the wrong matrix on purpose.
    media::VideoFrame mislabeled =
        makeFrame(PixelFormat::YUV444P, kBarsWidth, kBarsHeight, kRec709Limited, bars);
    mislabeled.color = rec601;
    const RenderedImage wrongMatrix = renderFrame(mislabeled);
    // Full-range code values tagged limited.
    media::VideoFrame wrongRangeFrame =
        makeFrame(PixelFormat::YUV444P, kBarsWidth, kBarsHeight, fullRange, bars);
    wrongRangeFrame.color = kRec709Limited;
    const RenderedImage wrongRange = renderFrame(wrongRangeFrame);

    for (int bar = 0; bar < 8; ++bar) {
        const int x = bar * kBarWidth + kBarWidth / 2;
        INFO("bar " << bar);
        checkPixel(as601, x, 32, barColor(bar));
        checkPixel(asFull, x, 32, barColor(bar));
        // The mislabeled renders agree with decoding the same codes under the declared tag.
        const auto codes709 = encodeYuv(barColor(bar), ColorMatrix::BT709, ColorRange::Limited, 8);
        const Rgb expect601 = referencePixel({codes709[0] / 255.0, codes709[1] / 255.0, codes709[2] / 255.0},
                                             1.0, resolveSourceColor(rec601, {}, false),
                                             SampleEncoding::Bits8, false, OutputTransform::Rec709);
        checkPixel(wrongMatrix, x, 32, expect601, 1.01 / 255.0);
    }
    // Matrix and range really matter: green bar (index 3) and white bar (index 0).
    CHECK(maxDiff(pixelOf(wrongMatrix, 3 * kBarWidth + 32, 32), barColor(3)) > 0.05);
    // 75% white full-range (Y = 191) read as limited -> (191 - 16) / 219 = 0.799.
    CHECK(pixelOf(wrongRange, 32, 32)[0] > 0.78);
    CHECK(pixelOf(wrongRange, 7 * kBarWidth + 32, 32)[0] == 0.0); // black clips at 0
}

TEST_CASE("Premultiplied over-blend of straight and premultiplied alpha", "[render][gpu][alpha]") {
    auto renderer = makeRenderer();
    const auto solid = [](std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
        return makeRgbaFrame(32, 32, [=](int, int) { return std::array<std::uint8_t, 4>{r, g, b, a}; });
    };
    const double a = 128.0 / 255.0;
    // Linear light: red * a + blue * (1 - a), sRGB-transfer sources, Rec.709 export.
    const Rgb expected{bt709Encode(a), 0.0, bt709Encode(1.0 - a)};

    const RenderedImage straight = renderOne(
        *renderer, compositeOf({32, 32}, {layerOf(solid(0, 0, 255, 255)), layerOf(solid(255, 0, 0, 128))}));
    checkPixel(straight, 16, 16, expected);

    // The same colour stored premultiplied.
    auto premultView = frameViewOf(solid(128, 0, 0, 128));
    REQUIRE(premultView.has_value());
    premultView->alpha = AlphaMode::Premultiplied;
    const RenderedImage premult =
        renderOne(*renderer, compositeOf({32, 32}, {layerOf(solid(0, 0, 255, 255)), layerOf(*premultView)}));
    checkPixel(premult, 16, 16, expected, 3.0 / 255.0);

    // Over nothing, with alpha kept in the output.
    auto straightOut = makeRenderer(OutputRole::Export, OutputAlpha::Straight);
    const RenderedImage kept =
        renderOne(*straightOut, compositeOf({32, 32}, {layerOf(solid(255, 0, 0, 128))}));
    checkPixel(kept, 16, 16, {1.0, 0.0, 0.0});
    CHECK(std::abs(alphaOf(kept, 16, 16) - a) <= 1.01 / 255.0);
    auto premultOut = makeRenderer(OutputRole::Export, OutputAlpha::Premultiplied);
    const RenderedImage pm = renderOne(*premultOut, compositeOf({32, 32}, {layerOf(solid(255, 0, 0, 128))}));
    checkPixel(pm, 16, 16, {a, 0.0, 0.0}, 1.01 / 255.0);
}

TEST_CASE("Layer transform: scale, clockwise rotation, anchor and position", "[render][gpu][transform]") {
    auto renderer = makeRenderer();
    // 64x32 frame: red | green over blue | white quadrants (sRGB, opaque).
    const media::VideoFrame frame = makeRgbaFrame(64, 32, [](int x, int y) {
        const bool right = x >= 32;
        const bool bottom = y >= 16;
        if (!bottom) {
            return right ? std::array<std::uint8_t, 4>{0, 255, 0, 255}
                         : std::array<std::uint8_t, 4>{255, 0, 0, 255};
        }
        return right ? std::array<std::uint8_t, 4>{255, 255, 255, 255}
                     : std::array<std::uint8_t, 4>{0, 0, 255, 255};
    });
    ColorPipeline srgb;
    srgb.exportTransform = OutputTransform::SRGB;
    LayerTransform t;
    t.scale = 2.0;
    t.rotationDegrees = 90.0;
    t.position = {10, -20};
    const RenderedImage image = renderOne(*renderer, compositeOf({256, 256}, {layerOf(frame, t)}, srgb));
    CHECK(savePng(image, QStringLiteral("transform_rot90_scale2.png")));
    // Hand-computed (see test_color_math): quadrant centres land at
    checkPixel(image, 154, 76, {1, 0, 0});  // top-left  -> top-right
    checkPixel(image, 154, 140, {0, 1, 0}); // top-right -> bottom-right
    checkPixel(image, 122, 76, {0, 0, 1});  // bottom-left -> top-left
    checkPixel(image, 122, 140, {1, 1, 1}); // bottom-right -> bottom-left
    // Outside the rotated 64x128 footprint centred at (138, 108): black.
    checkPixel(image, 138, 30, {0, 0, 0});
    checkPixel(image, 90, 108, {0, 0, 0});
    checkPixel(image, 180, 108, {0, 0, 0});

    // Anchor: the anchor point (top-left quadrant centre) sits on the position; scale 1, no rotation.
    LayerTransform anchored;
    anchored.anchor = {-16, -8};
    const RenderedImage a = renderOne(*renderer, compositeOf({256, 256}, {layerOf(frame, anchored)}, srgb));
    checkPixel(a, 128, 128, {1, 0, 0});
    checkPixel(a, 160, 128, {0, 1, 0}); // 32 px to the right is the green quadrant centre
    checkPixel(a, 128, 144, {0, 0, 1});

    // Antialiased edges: a half-pixel offset leaves a 50 % coverage column.
    LayerTransform halfPixel;
    halfPixel.position = {0.5, 0.0};
    const media::VideoFrame white =
        makeRgbaFrame(16, 16, [](int, int) { return std::array<std::uint8_t, 4>{255, 255, 255, 255}; });
    ColorPipeline linear;
    linear.exportTransform = OutputTransform::Linear;
    const RenderedImage edge =
        renderOne(*renderer, compositeOf({32, 32}, {layerOf(white, halfPixel)}, linear));
    CHECK(std::abs(pixelOf(edge, 8, 16)[0] - 0.5) < 0.02); // left edge column, half covered
    CHECK(pixelOf(edge, 16, 16)[0] > 0.99);
    CHECK(std::abs(pixelOf(edge, 24, 16)[0] - 0.5) < 0.02); // right edge column
    CHECK(pixelOf(edge, 25, 16)[0] < 0.01);
}

TEST_CASE("Opacity blends in linear light", "[render][gpu][opacity]") {
    auto renderer = makeRenderer();
    const auto solid = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        return makeRgbaFrame(32, 32, [=](int, int) { return std::array<std::uint8_t, 4>{r, g, b, 255}; });
    };
    ColorPipeline srgb;
    srgb.exportTransform = OutputTransform::SRGB;
    const RenderedImage image = renderOne(
        *renderer,
        compositeOf({32, 32}, {layerOf(solid(255, 0, 0)), layerOf(solid(0, 255, 0), {}, 0.25)}, srgb));
    checkPixel(image, 16, 16, {srgbEncode(0.75), srgbEncode(0.25), 0.0});
    // Opacity 0 and 1 are exact.
    const RenderedImage none = renderOne(
        *renderer,
        compositeOf({32, 32}, {layerOf(solid(255, 0, 0)), layerOf(solid(0, 255, 0), {}, 0.0)}, srgb));
    checkPixel(none, 16, 16, {1, 0, 0}, 0.5 / 255.0);
}

TEST_CASE("Layers stack bottom (V1) to top", "[render][gpu][layers]") {
    auto renderer = makeRenderer();
    const auto solid = [](int size, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        return makeRgbaFrame(size, size, [=](int, int) { return std::array<std::uint8_t, 4>{r, g, b, 255}; });
    };
    ColorPipeline srgb;
    srgb.exportTransform = OutputTransform::SRGB;
    const CompositeLayer red = layerOf(solid(128, 255, 0, 0));
    const CompositeLayer green = layerOf(solid(64, 0, 255, 0));
    const CompositeLayer blue = layerOf(solid(32, 0, 0, 255));
    const RenderedImage upward = renderOne(*renderer, compositeOf({128, 128}, {red, green, blue}, srgb));
    checkPixel(upward, 64, 64, {0, 0, 1});
    checkPixel(upward, 40, 64, {0, 1, 0});
    checkPixel(upward, 10, 10, {1, 0, 0});
    const RenderedImage downward = renderOne(*renderer, compositeOf({128, 128}, {blue, green, red}, srgb));
    checkPixel(downward, 64, 64, {1, 0, 0});
    checkPixel(downward, 40, 64, {1, 0, 0});
    // Empty composite = black.
    const RenderedImage empty = renderOne(*renderer, compositeOf({128, 128}, {}, srgb));
    checkPixel(empty, 64, 64, {0, 0, 0}, 0.0);
}

TEST_CASE("Preview divisor and downscaled outputs keep colours", "[render][gpu][scale]") {
    auto renderer = makeRenderer();
    const media::VideoFrame frame =
        makeFrame(PixelFormat::NV12, kBarsWidth, kBarsHeight, kRec709Limited, bars);
    // Output at a quarter size: the canvas is mip-filtered, bar centres keep their colour.
    const RenderedImage small =
        renderOne(*renderer, compositeOf({kBarsWidth, kBarsHeight}, {layerOf(frame)}), QSize(128, 16));
    for (int bar = 0; bar < 8; ++bar) {
        checkPixel(small, bar * 16 + 8, 8, barColor(bar), 3.0 / 255.0);
    }
    // Half-resolution canvas, upscaled to the full output.
    renderer->compositor().setPreviewDivisor(2);
    const RenderedImage half = renderOne(*renderer, compositeOf({kBarsWidth, kBarsHeight}, {layerOf(frame)}));
    CHECK(half.width == kBarsWidth);
    for (int bar = 0; bar < 8; ++bar) {
        checkPixel(half, bar * kBarWidth + kBarWidth / 2, 32, barColor(bar));
    }
}

TEST_CASE("Invalid requests fail cleanly", "[render][gpu]") {
    auto renderer = makeRenderer();
    auto bad = compositeOf({0, 0}, {});
    CHECK_FALSE(renderer->render(bad).has_value());
    auto future = compositeOf({16, 16}, {});
    future->color.version = ColorPipeline::kCurrentVersion + 1;
    const auto result = renderer->render(future);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::VersionMismatch);
    // An invalid frame skips the layer, the rest still renders.
    CompositeLayer broken;
    auto partial = compositeOf({16, 16}, {broken});
    CHECK(renderer->render(partial).has_value());
}
