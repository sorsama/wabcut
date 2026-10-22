// Nested composites on the GPU (C.8, I.8; CompositeLayer::nested): a nested or multicam clip's picture is its own
// composite, rendered by a child compositor on the nested canvas and placed like a frame of that size. Checked against
// the same layers composited flat (identity placement: the nesting itself adds nothing), against CPU references
// (placement with Motion, a nest of a nest at a preview divisor, a transition with a nested side), effects on the
// group, redactions inside the group, the float nest (I.9: values above 1 survive it), and the fail-closed rule: a nested
// composite that cannot be rendered leaves the layer out. Rendered pictures land in test-output/render/nested/.

#include "EffectsReference.h"
#include "RenderTestSupport.h"

#include <wabcut/render/Caption.h>
#include <wabcut/render/Compositor.h>
#include <wabcut/render/Mask.h>

#include <QDir>
#include <QFileInfo>
#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

constexpr int kW = 640;
constexpr int kH = 360;

std::uint8_t byte(double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

/// A busy opaque picture: gradients, a checkerboard and lines.
std::array<std::uint8_t, 4> busy(int x, int y) {
    const bool check = ((x / 24) + (y / 24)) % 2 == 0;
    if (x % 50 == 9 || y % 40 == 13) {
        return {240, 240, 230, 255};
    }
    return {byte(x / double(kW)), byte(0.2 + 0.6 * y / double(kH)), check ? std::uint8_t(190) : std::uint8_t(50), 255};
}

std::array<std::uint8_t, 4> solid(std::uint8_t r, std::uint8_t g, std::uint8_t b) { return {r, g, b, 255}; }

QString nestedDir() {
    const QString dir = outputDir() + QStringLiteral("/nested");
    QDir().mkpath(dir);
    return dir;
}

std::shared_ptr<const PreparedComposite> nestedOf(CanvasSize canvas, std::vector<CompositeLayer> layers) {
    auto n = std::make_shared<PreparedComposite>();
    n->canvas = canvas;
    n->layers = std::move(layers);
    return n;
}

CompositeLayer nestedLayer(std::shared_ptr<const PreparedComposite> nested, LayerTransform transform = {}, double opacity = 1.0) {
    CompositeLayer l;
    l.nested = std::move(nested);
    l.transform = transform;
    l.opacity = opacity;
    return l;
}

RenderedImage renderOf(OffscreenRenderer& renderer, CanvasSize canvas, std::vector<CompositeLayer> layers, const char* name,
                       CompositeStats* stats = nullptr) {
    auto composite = compositeOf(canvas, std::move(layers));
    if (stats) {
        auto timed = renderer.renderTimed(composite);
        REQUIRE(timed);
        *stats = timed->composite;
    }
    auto image = renderer.render(composite);
    REQUIRE(image);
    savePng(*image, QStringLiteral("nested/%1.png").arg(QString::fromLatin1(name)));
    return std::move(*image);
}

double psnrOf(const RenderedImage& a, const RenderedImage& b) {
    std::vector<std::uint8_t> rgb;
    rgb.reserve(static_cast<std::size_t>(b.width) * b.height * 3);
    for (int y = 0; y < b.height; ++y) {
        for (int x = 0; x < b.width; ++x) {
            const std::uint8_t* p = b.pixel(x, y);
            rgb.insert(rgb.end(), {p[0], p[1], p[2]});
        }
    }
    return psnrRgb(a, rgb);
}

} // namespace

TEST_CASE("Nested composites: at identity placement they equal the same layers composited flat", "[render][gpu][nested]") {
    (void)nestedDir();
    auto renderer = makeRenderer(OutputRole::Export);
    const media::VideoFrame background = makeRgbaFrame(kW, kH, busy);
    const media::VideoFrame box = makeRgbaFrame(200, 120, [](int x, int y) { return solid(byte(0.9 - x / 400.0), 60, byte(0.3 + y / 300.0)); });
    const LayerTransform pip{{90.0, -40.0}, 0.8, 12.0, {0.0, 0.0}};
    std::vector<CompositeLayer> inner{layerOf(background), layerOf(box, pip, 0.7)};
    inner[0].effects = {};
    const RenderedImage flat = renderOf(*renderer, {kW, kH}, inner, "flat");
    CompositeStats stats;
    const RenderedImage nested =
        renderOf(*renderer, {kW, kH}, {nestedLayer(nestedOf({kW, kH}, inner))}, "nested_identity", &stats);
    const double psnr = psnrOf(nested, flat);
    UNSCOPED_INFO(std::format("nested vs flat: PSNR {:.2f} dB", psnr));
    CHECK(psnr >= 45.0);
    CHECK(stats.nestedComposites == 1);
    CHECK(stats.layersDrawn == 3); // the group plus the two inside it
    CHECK(stats.layersFailed == 0);

    // A group with one layer and a blur equals that layer with the blur (the group's effects run on its picture).
    LayerEffect blur;
    blur.type = EffectType::GaussianBlur;
    blur.params = {9.0, 1.0};
    CompositeLayer blurred = layerOf(background);
    blurred.effects = {blur};
    const RenderedImage direct = renderOf(*renderer, {kW, kH}, {blurred}, "blur_direct");
    CompositeLayer group = nestedLayer(nestedOf({kW, kH}, {layerOf(background)}));
    group.effects = {blur};
    const RenderedImage viaGroup = renderOf(*renderer, {kW, kH}, {group}, "blur_group");
    CHECK(psnrOf(viaGroup, direct) >= 45.0);
}

TEST_CASE("Nested composites: placed with Motion, nested twice, at a preview divisor", "[render][gpu][nested]") {
    (void)nestedDir();
    // The nested sequence: 960x540, left half red, right half blue, opaque.
    const media::VideoFrame halves = makeRgbaFrame(960, 540, [](int x, int) { return x < 480 ? solid(230, 20, 20) : solid(20, 30, 220); });
    const auto inner = nestedOf({960, 540}, {layerOf(halves)});
    const media::VideoFrame grey = makeRgbaFrame(kW, kH, [](int, int) { return solid(90, 90, 90); });
    for (const int divisor : {1, 2}) {
        INFO("divisor " << divisor);
        OffscreenOptions options;
        options.compositor.previewDivisor = divisor;
        auto created = OffscreenRenderer::create(options);
        REQUIRE(created);
        OffscreenRenderer& renderer = **created;
        const Rgb greyOut = pixelOf(renderOf(renderer, {kW, kH}, {layerOf(grey)}, "grey"), 10, 10); // through the export transform
        // At 25 %, 100 px right of centre: 240x135 parent pixels centred at (420, 180).
        const RenderedImage once = renderOf(renderer, {kW, kH},
                                            {layerOf(grey), nestedLayer(inner, LayerTransform{{100.0, 0.0}, 0.25, 0.0, {0.0, 0.0}})},
                                            divisor == 1 ? "placed" : "placed_half");
        const Rgb red = pixelOf(once, 360, 180);
        const Rgb blue = pixelOf(once, 480, 180);
        const Rgb outside = pixelOf(once, 200, 180);
        CHECK(red[0] > 0.8);
        CHECK(red[2] < 0.2);
        CHECK(blue[2] > 0.75);
        CHECK(blue[0] < 0.2);
        CHECK(maxDiff(outside, greyOut) <= 2.0 / 255);
        // The box spans x 300..540: just outside and just inside its left edge.
        CHECK(maxDiff(pixelOf(once, 290, 180), greyOut) <= 2.0 / 255);
        CHECK(maxDiff(pixelOf(once, 310, 180), red) <= 4.0 / 255);

        // The same nest inside another nest at 200 %: back to 50 % overall, still centred at (420, 180).
        const auto outer = nestedOf({kW, kH}, {nestedLayer(inner, LayerTransform{{100.0, 0.0}, 0.5, 0.0, {0.0, 0.0}})});
        CompositeStats stats;
        const RenderedImage twice = renderOf(renderer, {kW, kH}, {layerOf(grey), nestedLayer(outer)},
                                             divisor == 1 ? "nested_twice" : "nested_twice_half", &stats);
        CHECK(stats.nestedComposites == 2);
        CHECK(pixelOf(twice, 240, 180)[0] > 0.8); // the red half now spans 180..420
        CHECK(pixelOf(twice, 600, 180)[2] > 0.75);
        CHECK(maxDiff(pixelOf(twice, 150, 180), greyOut) <= 2.0 / 255);
    }
}

TEST_CASE("Nested composites: a transition with a nested side, and opacity, match the CPU reference", "[render][gpu][nested]") {
    (void)nestedDir();
    auto renderer = makeRenderer(OutputRole::Export);
    const auto red = [](int, int) { return solid(220, 30, 40); };
    const auto blue = [](int, int) { return solid(30, 60, 210); };
    const media::VideoFrame a = makeRgbaFrame(kW, kH, red);
    const media::VideoFrame b = makeRgbaFrame(kW, kH, blue);
    LayerTransition mix;
    mix.kind = TransitionKind::CrossDissolve;
    mix.progress = 0.3;
    mix.layerCount = 2;
    CompositeLayer outgoing = nestedLayer(nestedOf({kW, kH}, {layerOf(a)}));
    outgoing.transition = mix;
    const RenderedImage image = renderOf(*renderer, {kW, kH}, {outgoing, layerOf(b)}, "dissolve_nested");
    const LinearImage la = linearFromRgba8(kW, kH, red);
    const LinearImage lb = linearFromRgba8(kW, kH, blue);
    const LinearImage expected = referenceTransition(&la, &lb, mix);
    const OutputTransform transform = outputTransformFor(ColorPipeline{}, OutputRole::Export);
    CHECK(psnrRgb(image, encodeOverBlack(expected, transform)) >= 42.0);

    // A group at 50 % opacity over black: the reference is the picture at half coverage.
    const RenderedImage half = renderOf(*renderer, {kW, kH}, {nestedLayer(nestedOf({kW, kH}, {layerOf(a)}), {}, 0.5)}, "opacity_nested");
    LinearImage scaled = la;
    for (Rgba& p : scaled.px) {
        for (double& c : p) {
            c *= 0.5;
        }
    }
    CHECK(psnrRgb(half, encodeOverBlack(scaled, transform)) >= 42.0);
}

TEST_CASE("Nested composites: a nested sequence's caption, placed, scaled and faded with the nest, against its golden (I.9)",
          "[render][gpu][nested][captions]") {
    auto renderer = makeRenderer();
    // The nested sequence: a busy picture and its own caption (a top-placed, yellow, outlined style with a box).
    CaptionFrame frame;
    frame.text = "Inside the nest: \xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5"; // + Thai
    frame.frameWidth = kW;
    frame.frameHeight = kH;
    frame.style.sizePx = 40.0;
    frame.style.fill = {1.0, 0.85, 0.1};
    frame.style.strokeWidth = 2.0;
    frame.style.position = CaptionPosition::Top;
    auto raster = CaptionRenderer::shared().render(frame);
    CompositeLayer caption;
    caption.view = raster->view(raster);
    caption.graphics = true;
    const auto nested = nestedOf({kW, kH}, {layerOf(makeRgbaFrame(kW, kH, busy)), caption});
    LayerTransform placed;
    placed.scale = 0.6;
    placed.position = {120.0, 60.0};
    placed.rotationDegrees = 4.0;
    const RenderedImage image =
        renderOf(*renderer, {kW, kH}, {layerOf(makeRgbaFrame(kW, kH, [](int, int) { return solid(20, 24, 40); })),
                                       nestedLayer(nested, placed, 0.85)},
                 "nested_caption");
    const QString golden = QStringLiteral(WABCUT_RENDER_GOLDENS) + QStringLiteral("/nested/nested_caption.png");
    if (qEnvironmentVariableIsSet("WABCUT_UPDATE_GOLDENS")) {
        QDir().mkpath(QFileInfo(golden).absolutePath());
        const QImage view(image.rgba.data(), image.width, image.height, image.width * 4, QImage::Format_RGBA8888);
        REQUIRE(view.save(golden));
    }
    QImage reference(golden);
    INFO("golden " << golden.toStdString() << " (WABCUT_UPDATE_GOLDENS=1 writes it)");
    REQUIRE_FALSE(reference.isNull());
    reference = reference.convertToFormat(QImage::Format_RGB888);
    REQUIRE(reference.width() == image.width);
    REQUIRE(reference.height() == image.height);
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image.width) * image.height * 3);
    for (int y = 0; y < image.height; ++y) {
        std::memcpy(rgb.data() + static_cast<std::size_t>(y) * image.width * 3, reference.constScanLine(y),
                    static_cast<std::size_t>(image.width) * 3);
    }
    const double psnr = psnrRgb(image, rgb);
    WARN(std::format("nested caption: PSNR {:.1f} dB against its golden", psnr));
    CHECK(psnr >= 40.0); // text rasterisation and GPU filtering differences only
}

TEST_CASE("Nested composites stay float: values above 1 survive the nest (I.9)", "[render][gpu][nested]") {
    (void)nestedDir();
    auto renderer = makeRenderer(OutputRole::Export);
    const media::VideoFrame picture = makeRgbaFrame(kW, kH, busy);
    const auto exposure = [](double stops) {
        LayerEffect e;
        e.type = EffectType::BasicCorrection;
        e.params = {stops, 0.0, 100.0, 0.0, 0.0}; // exposure, contrast, saturation, temperature, tint
        return e;
    };
    const RenderedImage direct = renderOf(*renderer, {kW, kH}, {layerOf(picture)}, "float_direct");
    // Four times brighter inside the nest (linear values up to ~3.5), a quarter of that outside it: the picture again,
    // only when the nested canvas keeps what is above 1 (it was clamped to [0, 1] at the nest's edge before I.9).
    CompositeLayer leaf = layerOf(picture);
    leaf.effects = {exposure(2.0)};
    CompositeLayer group = nestedLayer(nestedOf({kW, kH}, {leaf}));
    group.effects = {exposure(-2.0)};
    const RenderedImage viaNest = renderOf(*renderer, {kW, kH}, {group}, "float_nest");
    const double psnr = psnrOf(viaNest, direct);
    UNSCOPED_INFO(std::format("+2 stops inside, -2 outside vs the picture: PSNR {:.2f} dB", psnr));
    CHECK(psnr >= 45.0);
    // The bright lines (240/255) come back too, not a quarter of the clamped 1.0.
    CHECK(maxDiff(pixelOf(viaNest, 9, 100), pixelOf(direct, 9, 100)) <= 3.0 / 255);
    // Twice nested: still float.
    CompositeLayer outer = nestedLayer(nestedOf({kW, kH}, {nestedLayer(nestedOf({kW, kH}, {leaf}))}));
    outer.effects = {exposure(-2.0)};
    CHECK(psnrOf(renderOf(*renderer, {kW, kH}, {outer}, "float_nest_twice"), direct) >= 45.0);
}

TEST_CASE("Nested composites: redactions inside apply; one that cannot render leaves the layer out (fail closed)",
          "[render][gpu][nested][redaction]") {
    (void)nestedDir();
    auto renderer = makeRenderer(OutputRole::Export);
    const media::VideoFrame picture = makeRgbaFrame(kW, kH, busy);
    Mask region;
    region.shape = MaskShape::Rectangle;
    region.set("center.x", 0.3);
    region.set("center.y", 0.4);
    region.set("size.x", 0.2);
    region.set("size.y", 0.25);
    CompositeLayer leaf = layerOf(picture);
    LayerRedaction r;
    r.style = LayerRedaction::Style::Solid;
    r.color = {0.0, 1.0, 0.0};
    r.mask = maskSpecAt(std::vector<Mask>{region}, Ticks{}, static_cast<double>(kW) / kH);
    REQUIRE(r.mask);
    leaf.redactions = {r};
    CompositeStats stats;
    // The group at 50 % and moved: the redaction follows the picture inside it.
    const RenderedImage image = renderOf(*renderer, {kW, kH},
                                         {nestedLayer(nestedOf({kW, kH}, {leaf}), LayerTransform{{-80.0, 30.0}, 0.5, 0.0, {0.0, 0.0}})},
                                         "redaction_inside", &stats);
    CHECK(stats.redactionsApplied == 1);
    // Frame point (0.3, 0.4) of the nested canvas: (192, 144) there, so (320 - 80 + 0.5 * (192 - 320), 180 + 30 + 0.5 * (144 - 180)).
    CHECK(maxDiff(pixelOf(image, 176, 192), {0.0, 1.0, 0.0}) <= 2.0 / 255);

    // A nested composite with no canvas (as if it could not be built) is left out, not drawn without what it holds.
    const media::VideoFrame grey = makeRgbaFrame(kW, kH, [](int, int) { return solid(90, 90, 90); });
    const Rgb greyOut = pixelOf(renderOf(*renderer, {kW, kH}, {layerOf(grey)}, "grey_alone"), 10, 10);
    CompositeStats failed;
    const RenderedImage without = renderOf(*renderer, {kW, kH}, {layerOf(grey), nestedLayer(nestedOf({0, 0}, {leaf}))},
                                           "fail_closed", &failed);
    CHECK(failed.layersFailed == 1);
    CHECK(maxDiff(pixelOf(without, 192, 144), greyOut) <= 2.0 / 255);
}
