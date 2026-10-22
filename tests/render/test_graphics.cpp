// Motion graphics rasterised (C.13, I.9 Part 0.5; render/GraphicRaster.h): hand-built draw lists against analytic
// references - solid, linear and radial fills, strokes with dashes, trim paths, blend modes, opacity of a whole item,
// drop shadows, masks of one and two owners, images and their fits, text measured as drawn and animated per cluster
// (Thai marks stay with their consonant), the design canvas fitted into the frame, the raster cache - and the raster
// through the compositor on the GPU against its CPU conversion. Images land in <build>/test-output/render/graphics/.

#include "RenderTestSupport.h"

#include <wabcut/interchange/Lottie.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/render/GraphicRaster.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;
using wabcut::edit::applyPresetToLayer;
using wabcut::edit::PresetEasing;
using wabcut::edit::PresetKind;
using wabcut::edit::PresetOptions;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

/// A rectangle path centred on the origin.
std::vector<MaskVertex> rect(double w, double h) {
    return {MaskVertex{-w / 2, -h / 2}, MaskVertex{w / 2, -h / 2}, MaskVertex{w / 2, h / 2}, MaskVertex{-w / 2, h / 2}};
}

DrawItem shape(std::vector<MaskVertex> path, Rgba color, double x, double y) {
    DrawItem item;
    item.kind = DrawKind::Shape;
    item.path = std::move(path);
    item.fill.kind = PaintKind::Solid;
    item.fill.color = color;
    item.transform.e = x;
    item.transform.f = y;
    return item;
}

DrawList listOf(std::vector<DrawItem> items, int w = kW, int h = kH) {
    DrawList list;
    list.width = w;
    list.height = h;
    list.items = std::move(items);
    return list;
}

/// Straight (unpremultiplied) RGBA of a raster pixel, 0..1.
std::array<double, 4> at(const GraphicRaster& r, int x, int y) {
    const std::uint8_t* p = r.rgba().data() + (static_cast<std::size_t>(y) * r.width() + x) * 4;
    const double a = p[3] / 255.0;
    if (a <= 0.0) {
        return {0, 0, 0, 0};
    }
    return {p[0] / 255.0 / a, p[1] / 255.0 / a, p[2] / 255.0 / a, a};
}

double alphaAt(const GraphicRaster& r, int x, int y) { return at(r, x, y)[3]; }

bool near(double a, double b, double tolerance = 2.5 / 255.0) { return std::abs(a - b) <= tolerance; }

QImage imageOf(const GraphicRaster& r) {
    QImage image(r.rgba().data(), r.width(), r.height(), r.width() * 4, QImage::Format_RGBA8888_Premultiplied);
    return image.copy();
}

void save(const GraphicRaster& r, const QString& name) {
    QDir().mkpath(outputDir() + QStringLiteral("/graphics"));
    imageOf(r).save(outputDir() + QStringLiteral("/graphics/") + name);
}

/// Ink (alpha > 0.5) bounding box.
QRect inkOf(const GraphicRaster& r) {
    QRect box;
    for (int y = 0; y < r.height(); ++y) {
        for (int x = 0; x < r.width(); ++x) {
            if (alphaAt(r, x, y) > 0.5) {
                box = box.united(QRect(x, y, 1, 1));
            }
        }
    }
    return box;
}

DrawItem textItem(const std::string& text, double size, double x, double y, TextAlign align = TextAlign::Left) {
    DrawItem item;
    item.kind = DrawKind::Text;
    DrawTextRun run;
    run.text = text;
    run.fontSize = size;
    run.fill = {1.0, 1.0, 1.0, 1.0};
    item.text.runs = {run};
    item.text.align = align;
    item.transform.e = x;
    item.transform.f = y;
    return item;
}

class SolidPictures final : public GraphicImageSource {
public:
    QImage picture(MediaId media) const override {
        if (media.value != 7) {
            return {};
        }
        QImage image(40, 20, QImage::Format_RGBA8888); // left half red, right half green
        for (int y = 0; y < 20; ++y) {
            for (int x = 0; x < 40; ++x) {
                image.setPixelColor(x, y, x < 20 ? QColor(255, 0, 0) : QColor(0, 255, 0));
            }
        }
        return image;
    }
};

} // namespace

TEST_CASE("Graphics: solid, linear and radial fills against their formulas", "[render][graphics]") {
    // Solid.
    auto solid = rasterizeGraphic(listOf({shape(rect(100, 60), {0.2, 0.6, 0.9, 1.0}, 160, 90)}), kW, kH);
    CHECK(near(at(*solid, 160, 90)[0], 0.2));
    CHECK(near(at(*solid, 160, 90)[2], 0.9));
    CHECK(alphaAt(*solid, 160, 90) == 1.0);
    CHECK(alphaAt(*solid, 20, 20) == 0.0);
    CHECK(alphaAt(*solid, 112, 90) == 1.0); // inside the edge (x 110..210)
    CHECK(alphaAt(*solid, 108, 90) == 0.0);
    // Linear: black to white along x over the item, in encoded values.
    DrawItem linear = shape(rect(200, 40), {}, 160, 90);
    linear.fill.kind = PaintKind::Linear;
    linear.fill.stops = {{0.0, {0, 0, 0, 1}}, {1.0, {1, 1, 1, 1}}};
    linear.fill.start = {-100, 0};
    linear.fill.end = {100, 0};
    auto ramp = rasterizeGraphic(listOf({linear}), kW, kH);
    save(*ramp, QStringLiteral("linear.png"));
    for (int x = 70; x <= 250; x += 20) {
        const double expected = (x + 0.5 - 60.0) / 200.0;
        INFO("x " << x);
        CHECK(near(at(*ramp, x, 90)[1], expected, 1.5 / 255.0 + 1.0 / 200.0));
    }
    // Radial: the centre is the first stop, the circle the last; beyond it padded.
    DrawItem radial = shape(rect(160, 160), {}, 160, 90);
    radial.fill.kind = PaintKind::Radial;
    radial.fill.stops = {{0.0, {1, 0, 0, 1}}, {1.0, {0, 0, 1, 1}}};
    radial.fill.start = {0, 0};
    radial.fill.end = {60, 0};
    auto disc = rasterizeGraphic(listOf({radial}), kW, kH);
    CHECK(near(at(*disc, 160, 90)[0], 1.0, 4.0 / 255.0));
    CHECK(near(at(*disc, 160 + 30, 90)[0], 0.5, 6.0 / 255.0));
    CHECK(near(at(*disc, 160 + 70, 90)[2], 1.0, 2.0 / 255.0));
}

TEST_CASE("Graphics: strokes with dashes, trim paths, opacity of the whole item, blend modes", "[render][graphics]") {
    // A dashed horizontal line: 20 on, 20 off, width 10.
    DrawItem line;
    line.kind = DrawKind::Shape;
    line.path = {MaskVertex{-120, 0}, MaskVertex{120, 0}};
    line.closed = false;
    line.fill.kind = PaintKind::None;
    line.stroke.enabled = true;
    line.stroke.width = 10.0;
    line.stroke.color = {1, 1, 1, 1};
    line.stroke.dashes = {20, 20};
    line.transform.e = 160;
    line.transform.f = 90;
    auto dashed = rasterizeGraphic(listOf({line}), kW, kH);
    save(*dashed, QStringLiteral("dashes.png"));
    for (int k = 0; k < 6; ++k) {
        INFO("dash " << k);
        CHECK(alphaAt(*dashed, 40 + 40 * k + 10, 90) > 0.9);  // inside dash k
        CHECK(alphaAt(*dashed, 40 + 40 * k + 30, 90) < 0.1);  // inside gap k
    }
    // Trim: the first half of the solid line only; then a quarter of it shifted by 180 degrees (wrapping on an open
    // path: from 0.5 + 0.0 to 0.5 + 0.25).
    DrawItem trim = line;
    trim.stroke.dashes.clear();
    trim.trimEnd = 0.5;
    auto half = rasterizeGraphic(listOf({trim}), kW, kH);
    CHECK(alphaAt(*half, 60, 90) > 0.9);
    CHECK(alphaAt(*half, 150, 90) > 0.9);
    CHECK(alphaAt(*half, 170, 90) < 0.1);
    CHECK(alphaAt(*half, 260, 90) < 0.1);
    trim.trimEnd = 0.25;
    trim.trimOffset = 180.0;
    auto shifted = rasterizeGraphic(listOf({trim}), kW, kH);
    CHECK(alphaAt(*shifted, 60, 90) < 0.1);
    CHECK(alphaAt(*shifted, 190, 90) > 0.9);
    CHECK(alphaAt(*shifted, 250, 90) < 0.1);
    // Fill and stroke at half opacity: one item, faded once (the stroke over the fill is not darker).
    DrawItem both = shape(rect(100, 60), {1, 1, 1, 1}, 160, 90);
    both.stroke.enabled = true;
    both.stroke.width = 12.0;
    both.stroke.color = {1, 1, 1, 1};
    both.opacity = 0.5;
    auto faded = rasterizeGraphic(listOf({both}), kW, kH);
    CHECK(near(alphaAt(*faded, 160, 90), 0.5, 2.0 / 255.0));
    CHECK(near(alphaAt(*faded, 160, 60), 0.5, 2.0 / 255.0)); // on the stroke over the fill's edge
    // Blend modes of a 50 % grey square over a red / cyan base, in encoded values.
    struct Case {
        wabcut::BlendMode mode;
        const char* name;
        std::array<double, 3> expected; // over red (1, 0, 0) ... at the red half
    };
    const Case cases[] = {{wabcut::BlendMode::Multiply, "multiply", {0.5, 0.0, 0.0}}, {wabcut::BlendMode::Screen, "screen", {1.0, 0.5, 0.5}},
                          {wabcut::BlendMode::Add, "add", {1.0, 0.5, 0.5}},           {wabcut::BlendMode::Darken, "darken", {0.5, 0.0, 0.0}},
                          {wabcut::BlendMode::Lighten, "lighten", {1.0, 0.5, 0.5}},   {wabcut::BlendMode::Difference, "difference", {0.5, 0.5, 0.5}}};
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            DrawItem top = shape(rect(80, 80), {0.5, 0.5, 0.5, 1.0}, 160, 90);
            top.blend = c.mode;
            auto blended = rasterizeGraphic(listOf({shape(rect(300, 160), {1, 0, 0, 1}, 160, 90), top}), kW, kH);
            const auto p = at(*blended, 160, 90);
            CHECK(near(p[0], c.expected[0], 3.0 / 255.0));
            CHECK(near(p[1], c.expected[1], 3.0 / 255.0));
            CHECK(near(p[2], c.expected[2], 3.0 / 255.0));
            CHECK(near(at(*blended, 40, 90)[0], 1.0)); // outside the square: the base
        }
    }
}

TEST_CASE("Graphics: drop shadows and masks", "[render][graphics]") {
    DrawItem square = shape(rect(60, 60), {1, 1, 1, 1}, 120, 80);
    square.shadow.enabled = true;
    square.shadow.color = {0, 0, 0, 1};
    square.shadow.opacity = 1.0;
    square.shadow.angle = 0.0; // to the right
    square.shadow.distance = 40.0;
    square.shadow.softness = 3.0;
    auto shadowed = rasterizeGraphic(listOf({square}), kW, kH);
    save(*shadowed, QStringLiteral("shadow.png"));
    CHECK(near(at(*shadowed, 120, 80)[0], 1.0));         // the item itself, above its shadow
    CHECK(alphaAt(*shadowed, 175, 80) > 0.95);           // the shadow, 40 px right (the item ends at 150)
    CHECK(near(at(*shadowed, 175, 80)[0], 0.0, 0.02));    // black
    CHECK(alphaAt(*shadowed, 215, 80) < 0.05);           // past the shadow (ends at 190 + its blur)
    CHECK(alphaAt(*shadowed, 120, 140) < 0.05);          // nothing below
    CHECK(shadowed->info().shadows == 1);

    // Masks: an Add rectangle keeps its inside; a Subtract of the same owner cuts a hole; another owner multiplies.
    DrawItem masked = shape(rect(300, 160), {0.9, 0.2, 0.2, 1.0}, 160, 90);
    const auto outline = [](double x, double y, double w, double h) {
        return std::vector<MaskVertex>{MaskVertex{x, y}, MaskVertex{x + w, y}, MaskVertex{x + w, y + h}, MaskVertex{x, y + h}};
    };
    DrawMask keep;
    keep.owner = 1;
    keep.outline = outline(60, 40, 200, 100);
    DrawMask hole;
    hole.owner = 1;
    hole.mode = MaskMode::Subtract;
    hole.outline = outline(140, 70, 40, 40);
    DrawMask group;
    group.owner = 2;
    group.outline = outline(0, 0, 200, 180);
    group.opacity = 0.5;
    masked.masks = {keep, hole, group};
    auto cut = rasterizeGraphic(listOf({masked}), kW, kH);
    save(*cut, QStringLiteral("masks.png"));
    CHECK(near(alphaAt(*cut, 100, 60), 0.5, 2.0 / 255.0)); // kept by owner 1, halved by owner 2
    CHECK(alphaAt(*cut, 160, 90) < 0.02);                    // the hole
    CHECK(alphaAt(*cut, 40, 60) < 0.02);                     // outside owner 1's rectangle
    CHECK(alphaAt(*cut, 230, 60) < 0.02);                    // outside owner 2's
    CHECK(cut->info().maskedItems == 1);
}

TEST_CASE("Graphics: images fitted into their box", "[render][graphics][images]") {
    const SolidPictures pictures;
    const auto imageItem = [](ImageFit fit, MediaId media = MediaId{7}) {
        DrawItem item;
        item.kind = DrawKind::Image;
        item.image.media = media;
        item.image.width = 120;
        item.image.height = 120;
        item.image.fit = fit;
        item.transform.e = 160;
        item.transform.f = 90;
        return item;
    };
    auto contain = rasterizeGraphic(listOf({imageItem(ImageFit::Contain)}), kW, kH, &pictures);
    CHECK(alphaAt(*contain, 160, 90 - 25) > 0.9); // 120 x 60 centred
    CHECK(alphaAt(*contain, 160, 90 - 35) < 0.1);
    CHECK(at(*contain, 120, 90)[0] > 0.9);       // the red half on the left
    CHECK(at(*contain, 200, 90)[1] > 0.9);       // green on the right
    auto cover = rasterizeGraphic(listOf({imageItem(ImageFit::Cover)}), kW, kH, &pictures);
    CHECK(alphaAt(*cover, 160, 90 - 55) > 0.9);   // fills the box's height
    CHECK(alphaAt(*cover, 160 - 65, 90) < 0.1);   // cropped to the box's width
    auto fill = rasterizeGraphic(listOf({imageItem(ImageFit::Fill)}), kW, kH, &pictures);
    CHECK(alphaAt(*fill, 160 - 55, 90 - 55) > 0.9);
    auto none = rasterizeGraphic(listOf({imageItem(ImageFit::None)}), kW, kH, &pictures);
    CHECK(alphaAt(*none, 160 - 15, 90) > 0.9);   // 40 x 20 at its own size
    CHECK(alphaAt(*none, 160 - 25, 90) < 0.1);
    auto missing = rasterizeGraphic(listOf({imageItem(ImageFit::Contain, MediaId{9})}), kW, kH, &pictures);
    CHECK(missing->info().missingImages == 1);
    CHECK(inkOf(*missing).isEmpty());
}

TEST_CASE("Graphics: text is measured as drawn; clusters animate after layout and Thai marks stay on their consonant",
          "[render][graphics][text]") {
    // The measurer and the drawing use one layout: the ink fits inside the measured block.
    DrawItem name = textItem("Avery Quinn", 40.0, 20, 40);
    auto drawn = rasterizeGraphic(listOf({name}), kW, kH);
    save(*drawn, QStringLiteral("text.png"));
    TextContent content;
    TextRun run;
    run.text = "Avery Quinn";
    run.style.fontSize = 40.0;
    content.runs = {run};
    const auto size = graphicTextMeasurer().measure(content, {""});
    const QRect ink = inkOf(*drawn);
    REQUIRE_FALSE(ink.isEmpty());
    INFO(std::format("measured {} x {}, ink {} {} {} {}", size[0], size[1], ink.x(), ink.y(), ink.width(), ink.height()));
    CHECK(ink.left() >= 20 - 2);
    CHECK(ink.right() <= 20 + size[0] + 2);
    CHECK(ink.top() >= 40 - 2);
    CHECK(ink.bottom() <= 40 + size[1] + 2);
    CHECK(ink.width() >= size[0] * 0.8); // and hugs it
    // Centred and right-aligned text hang from the origin's top-centre and top-right.
    auto centred = rasterizeGraphic(listOf({textItem("Avery Quinn", 40.0, 160, 40, TextAlign::Center)}), kW, kH);
    CHECK(std::abs(inkOf(*centred).center().x() - 160) <= 3);
    auto right = rasterizeGraphic(listOf({textItem("Avery Quinn", 40.0, 300, 40, TextAlign::Right)}), kW, kH);
    CHECK(std::abs(inkOf(*right).right() - 300) <= 4);

    // A typewriter at its middle: the first clusters shown, the rest hidden (layout unchanged: no reflow).
    DrawItem typing = textItem("ABCD", 48.0, 40, 60);
    for (std::size_t i = 0; i < 4; ++i) {
        typing.text.clusters.push_back(DrawCluster{i, i + 1, i < 2 ? 1.0 : 0.0});
    }
    auto typed = rasterizeGraphic(listOf({typing}), kW, kH);
    auto full = rasterizeGraphic(listOf({textItem("ABCD", 48.0, 40, 60)}), kW, kH);
    save(*typed, QStringLiteral("typewriter_half.png"));
    const QRect typedInk = inkOf(*typed);
    const QRect fullInk = inkOf(*full);
    CHECK(typedInk.left() == fullInk.left());
    CHECK(typedInk.right() < fullInk.left() + fullInk.width() * 0.6);
    CHECK(typed->info().animatedClusters == 2);

    // Thai: "กี่" is one cluster (a consonant with a vowel and a tone mark above it). Moved 100 px as a cluster, all of
    // its ink moves - the marks stay on the consonant.
    const std::string thai = "\xE0\xB8\x81\xE0\xB8\xB5\xE0\xB9\x88"; // ก ี ่
    DrawItem still = textItem(thai, 64.0, 40, 40);
    still.text.clusters = {DrawCluster{0, thai.size(), 1.0}};
    DrawItem moved = still;
    moved.text.clusters[0].offsetX = 100.0;
    auto a = rasterizeGraphic(listOf({still}), kW, kH);
    auto b = rasterizeGraphic(listOf({moved}), kW, kH);
    save(*b, QStringLiteral("thai_cluster_moved.png"));
    const QRect inkA = inkOf(*a);
    const QRect inkB = inkOf(*b);
    REQUIRE_FALSE(inkA.isEmpty());
    CHECK(inkB.translated(-100, 0) == inkA);
    for (int y = inkA.top(); y <= inkA.bottom(); ++y) {
        for (int x = inkA.left(); x <= inkA.right(); ++x) {
            if (std::abs(alphaAt(*a, x, y) - alphaAt(*b, x + 100, y)) > 3.0 / 255.0) {
                FAIL(std::format("pixel ({}, {}) did not move with its cluster", x, y));
            }
        }
    }
}

TEST_CASE("Graphics: the design canvas fits into the frame (contain, centred); equal lists reuse the raster",
          "[render][graphics][cache]") {
    // A 1920 x 1080 design into a 3840 x 2160 frame: twice the size, vector-sharp.
    const DrawList design = listOf({shape(rect(200, 100), {1, 1, 1, 1}, 960, 540)}, 1920, 1080);
    auto big = rasterizeGraphic(design, 3840, 2160);
    CHECK(alphaAt(*big, 1920 - 199, 1080) == 1.0);
    CHECK(alphaAt(*big, 1920 - 201, 1080) == 0.0);
    // A square design into a wide frame: pillarboxed.
    auto square = rasterizeGraphic(listOf({shape(rect(100, 100), {1, 1, 1, 1}, 50, 50)}, 100, 100), 320, 160);
    CHECK(alphaAt(*square, 160, 80) == 1.0);
    CHECK(alphaAt(*square, 50, 80) == 0.0); // the design spans x 80..240

    GraphicRasterCache cache(4);
    const DrawList list = listOf({shape(rect(60, 60), {1, 0, 0, 1}, 100, 90)});
    const auto first = cache.raster(1, list, kW, kH);
    const auto again = cache.raster(1, list, kW, kH);
    CHECK(first == again); // the same raster: no re-render while a graphic is still
    CHECK(cache.rasterizations() == 1);
    DrawList moved = list;
    moved.items[0].transform.e = 120;
    const auto third = cache.raster(1, moved, kW, kH);
    CHECK(third != first);
    CHECK(cache.rasterizations() == 2);
    CHECK(cache.raster(1, moved, 2 * kW, 2 * kH) != third); // another size, another raster
}

TEST_CASE("Graphics through the compositor (GPU) equal their CPU raster", "[render][gpu][graphics]") {
    DrawItem a = shape(rect(160, 90), {0.9, 0.5, 0.1, 1.0}, 120, 80);
    a.stroke.enabled = true;
    a.stroke.width = 6;
    a.stroke.color = {1, 1, 1, 1};
    DrawItem b = textItem("Lower third", 36.0, 40, 110);
    b.text.background = true;
    b.text.backgroundColor = {0.05, 0.05, 0.1, 0.7};
    b.text.backgroundPadding = 8;
    const auto raster = rasterizeGraphic(listOf({a, b}), kW, kH);
    CompositeLayer layer;
    layer.view = raster->view(raster);
    auto renderer = makeRenderer();
    auto image = renderer->render(compositeOf({kW, kH}, {layer}));
    REQUIRE(image);
    savePng(*image, QStringLiteral("graphics/gpu_composite.png"));
    // The CPU expectation: premultiplied sRGB over black, decoded to linear, BT.709-encoded (the default output).
    double worst = 0.0;
    for (int y = 0; y < kH; y += 3) {
        for (int x = 0; x < kW; x += 3) {
            const std::uint8_t* p = raster->rgba().data() + (static_cast<std::size_t>(y) * kW + x) * 4;
            const double alpha = p[3] / 255.0;
            Rgb expected{};
            for (int c = 0; c < 3; ++c) {
                const double straight = alpha > 0.0 ? std::min(1.0, p[c] / 255.0 / alpha) : 0.0;
                const double linear = straight <= 0.04045 ? straight / 12.92 : std::pow((straight + 0.055) / 1.055, 2.4);
                expected[static_cast<std::size_t>(c)] = linearToOutput(OutputTransform::Rec709, linear * alpha);
            }
            worst = std::max(worst, maxDiff(pixelOf(*image, x, y), expected));
        }
    }
    CHECK(worst <= 3.0 / 255.0);
}

// ---- C.13 content: the Lottie lower third, the typewriter preset, a pinned box --------------------------------------

namespace {

ResolveOptions realText() {
    ResolveOptions o;
    o.measurer = &graphicTextMeasurer();
    o.lineBreaker = &qtLineBreaker();
    return o;
}

std::string readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// PSNR (dB) of a raster against a golden, premultiplied RGB.
double psnrOf(const GraphicRaster& a, const QImage& golden) {
    if (golden.width() != a.width() || golden.height() != a.height()) {
        return 0.0;
    }
    const QImage g = golden.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    double sum = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        const std::uint8_t* p = a.rgba().data() + static_cast<std::size_t>(y) * a.width() * 4;
        const std::uint8_t* q = g.constScanLine(y);
        for (int x = 0; x < a.width() * 4; ++x) {
            if (x % 4 == 3) {
                continue;
            }
            const double d = static_cast<double>(p[x]) - q[x];
            sum += d * d;
        }
    }
    const double mse = sum / (static_cast<double>(a.width()) * a.height() * 3.0);
    return mse <= 0.0 ? 100.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

GraphicLayer textLayer(std::uint32_t id, const std::string& text, double size, double x, double y) {
    GraphicLayer layer;
    layer.id = id;
    layer.kind = LayerKind::Text;
    layer.transform.x = ParamValue(x);
    layer.transform.y = ParamValue(y);
    TextRun run;
    run.text = text;
    run.style.fontSize = size;
    layer.text.runs = {run};
    return layer;
}

/// Whether `text` starts with a Thai vowel above / below or a tone mark (U+0E31, U+0E34..U+0E3A, U+0E47..U+0E4E).
bool startsWithThaiMark(std::string_view text) {
    if (text.size() < 3 || static_cast<unsigned char>(text[0]) != 0xE0) {
        return false;
    }
    const auto b1 = static_cast<unsigned char>(text[1]);
    const auto b2 = static_cast<unsigned char>(text[2]);
    return (b1 == 0xB8 && (b2 == 0xB1 || (b2 >= 0xB4 && b2 <= 0xBA))) || (b1 == 0xB9 && b2 >= 0x87 && b2 <= 0x8E);
}

} // namespace

TEST_CASE("C.13: the Lottie lower third at frames 0, 15, 45 and 89 against its goldens", "[render][graphics][lottie]") {
    const auto fixture = std::filesystem::path(WABCUT_LOTTIE_FIXTURES) / "lower_third.json";
    REQUIRE(std::filesystem::is_regular_file(fixture));
    const auto imported = interchange::importLottie(readText(fixture), "lower_third.json");
    REQUIRE(imported);
    const Graphic& graphic = imported->graphic;
    const std::filesystem::path goldens = std::filesystem::path(WABCUT_RENDER_GOLDENS) / "graphics";
    const bool update = qEnvironmentVariableIsSet("WABCUT_UPDATE_GOLDENS");
    GraphicRasterInfo total;
    std::vector<std::vector<std::uint8_t>> pictures;
    for (const int frame : {0, 15, 45, 89}) {
        const Ticks t = framesToTicks(frame, imported->frameRate);
        const DrawList list = resolveGraphic(graphic, t, realText());
        CHECK(list.width == 1920);
        CHECK(list.height == 1080);
        const auto raster = rasterizeGraphic(list, 960, 540); // the design fitted into a half-size frame
        save(*raster, QStringLiteral("lottie_lower_third_f%1.png").arg(frame, 2, 10, QLatin1Char('0')));
        const GraphicRasterInfo& i = raster->info();
        total.shapes += i.shapes;
        total.texts += i.texts;
        total.maskedItems += i.maskedItems;
        pictures.push_back(raster->rgba());
        const auto golden = goldens / std::format("lottie_lower_third_f{:02}.png", frame);
        if (update) {
            std::filesystem::create_directories(goldens);
            REQUIRE(imageOf(*raster).save(QString::fromStdString(golden.string())));
        }
        const QImage reference(QString::fromStdString(golden.string()));
        INFO("golden " << golden.string() << " (WABCUT_UPDATE_GOLDENS=1 writes them)");
        REQUIRE_FALSE(reference.isNull());
        const double psnr = psnrOf(*raster, reference);
        WARN(std::format("lower third frame {}: PSNR {:.1f} dB against its golden", frame, psnr));
        CHECK(psnr >= 40.0); // anti-aliasing and font rasterisation differences only
        // The same list again is equal: the cache would reuse its raster.
        CHECK(resolveGraphic(graphic, t, realText()) == list);
    }
    // Gradients, the bar, the star, the masked backdrop and the name all drew; the animation moves (no two frames alike).
    CHECK(total.shapes >= 4);
    CHECK(total.texts >= 2);
    CHECK(total.maskedItems >= 1);
    for (std::size_t a = 0; a < pictures.size(); ++a) {
        for (std::size_t b = a + 1; b < pictures.size(); ++b) {
            CHECK(pictures[a] != pictures[b]);
        }
    }
}

TEST_CASE("C.13: a typewriter preset reveals the clusters in order; Thai clusters keep their marks", "[render][graphics][text]") {
    const std::string latin = "Hello world";
    const std::string thai = "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5"
                             "\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A"; // sawatdi khrap
    for (const std::string* each : {&latin, &thai}) {
        const std::string& text = *each;
        const bool isLatin = each == &latin;
        DYNAMIC_SECTION((isLatin ? "latin" : "thai")) {
            Graphic graphic;
            graphic.width = 960;
            graphic.height = 540;
            PresetOptions typewriter;
            typewriter.kind = PresetKind::Typewriter;
            typewriter.duration = Ticks::fromSeconds(1);
            typewriter.easing = PresetEasing::Linear;
            auto layer = applyPresetToLayer(textLayer(1, text, 64.0, 80, 200), typewriter, Ticks{}, Ticks::fromSeconds(3), 960, 540);
            REQUIRE(layer);
            graphic.layers = {*layer};
            int lastWidth = -1;
            std::size_t lastShown = 0;
            for (const double s : {0.0, 0.3, 0.6, 1.0, 2.0}) {
                const Ticks at{std::llround(s * static_cast<double>(kTicksPerSecond))};
                const DrawList list = resolveGraphic(graphic, at, realText());
                REQUIRE(list.items.size() == 1);
                const DrawText& drawn = list.items[0].text;
                std::size_t shown = 0;
                for (const DrawCluster& c : drawn.clusters) {
                    const std::string_view piece = std::string_view(text).substr(c.byteStart, c.byteEnd - c.byteStart);
                    INFO("cluster at byte " << c.byteStart);
                    CHECK_FALSE(startsWithThaiMark(piece)); // a mark never starts a cluster: it stays with its consonant
                    shown += c.opacity > 0.5 ? 1 : 0;
                }
                CHECK(shown >= lastShown); // in order, never hiding again
                lastShown = shown;
                const auto raster = rasterizeGraphic(list, 960, 540);
                save(*raster, QStringLiteral("typewriter_%1_%2.png").arg(isLatin ? QStringLiteral("latin") : QStringLiteral("thai")).arg(s));
                const QRect ink = inkOf(*raster);
                const int width = ink.isEmpty() ? 0 : ink.width();
                CHECK(width >= lastWidth);
                lastWidth = width;
            }
            CHECK(lastShown > 0);
        }
    }
}

TEST_CASE("C.13: a box pinned around a name hugs the drawn name (the rasteriser measures the pins)", "[render][graphics][text]") {
    Graphic graphic;
    graphic.width = 960;
    graphic.height = 540;
    GraphicLayer box;
    box.id = 2;
    box.kind = LayerKind::Shape;
    box.shape.kind = ShapeKind::Rectangle;
    box.shape.fill.kind = PaintKind::Solid;
    box.shape.fill.color.rgba = {0.1, 0.2, 0.6, 1.0};
    box.pin = LayerPin{1, LayerPin::kLeft | LayerPin::kTop | LayerPin::kRight | LayerPin::kBottom, {-16.0, -8.0, 16.0, 8.0}};
    graphic.layers = {box, textLayer(1, "Avery Quinn-Okafor", 48.0, 120, 200)};
    const DrawList list = resolveGraphic(graphic, Ticks{}, realText());
    REQUIRE(list.items.size() == 2);
    DrawList boxList{list.width, list.height, {list.items[0]}};
    DrawList nameList{list.width, list.height, {list.items[1]}};
    const auto boxOnly = rasterizeGraphic(boxList, 960, 540);
    const auto nameOnly = rasterizeGraphic(nameList, 960, 540);
    save(*rasterizeGraphic(list, 960, 540), QStringLiteral("pinned_box.png"));
    const QRect boxInk = inkOf(*boxOnly);
    const QRect nameInk = inkOf(*nameOnly);
    REQUIRE_FALSE(boxInk.isEmpty());
    REQUIRE_FALSE(nameInk.isEmpty());
    INFO(std::format("box {} {} {} {}, name ink {} {} {} {}", boxInk.x(), boxInk.y(), boxInk.width(), boxInk.height(), nameInk.x(),
                     nameInk.y(), nameInk.width(), nameInk.height()));
    CHECK(boxInk.contains(nameInk));
    // The box's sides sit the pin's offsets from the measured block, which is the drawn layout: within a few pixels of
    // the ink horizontally (side bearings); vertically the line box holds the ascent, and the descent but for a tail
    // such as Q's that may hang a pixel or two below it.
    CHECK(std::abs((nameInk.left() - boxInk.left()) - 16) <= 6);
    CHECK(std::abs((boxInk.right() - nameInk.right()) - 16) <= 6);
    CHECK(nameInk.top() - boxInk.top() >= 8);
    CHECK(boxInk.bottom() - nameInk.bottom() >= 8 - 4);
    CHECK(boxInk.height() <= nameInk.height() + 8 + 8 + 48); // not taller than the line box
}

// ---- 0.2: type on a path --------------------------------------------------------------------------------------------

TEST_CASE("0.2: text on a circle stands on the path, upright or turned", "[render][graphics][textpath]") {
    // A ring of text round a circle of radius 60 centred on (160, 90): ink only near the ring, none in the middle.
    Graphic graphic;
    graphic.width = kW;
    graphic.height = kH;
    GraphicLayer ring = textLayer(1, "AROUND THE WORLD AROUND THE", 18.0, 160, 90);
    TextPath path;
    bool closed = false;
    path.path = {MaskPathKey{Ticks{}, edit::textPathVertices(edit::TextPathShape::Circle, 120.0, 0.0, &closed),
                             Interpolation::Linear}};
    path.closed = closed;
    ring.text.path = path;
    graphic.layers = {ring};
    const DrawList list = resolveGraphic(graphic, Ticks{}, realText());
    REQUIRE(list.items.size() == 1);
    const auto raster = rasterizeGraphic(list, kW, kH);
    save(*raster, QStringLiteral("text_on_circle.png"));
    const QRect ink = inkOf(*raster);
    REQUIRE_FALSE(ink.isEmpty());
    INFO(std::format("ink {} {} {} {}", ink.x(), ink.y(), ink.width(), ink.height()));
    // Letters stand outside the circle (baseline on it): the ink reaches past radius 60 but stays within 60 + the size.
    CHECK(ink.left() >= 160 - 60 - 24);
    CHECK(ink.top() >= 90 - 60 - 24);
    CHECK(ink.left() <= 160 - 60);
    double inside = 0.0;
    double onRing = 0.0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const double r = std::hypot(x - 160.0, y - 90.0);
            const double a = alphaAt(*raster, x, y);
            if (r < 50.0) {
                inside += a;
            } else if (r <= 60.0 + 18.0) {
                onRing += a;
            }
        }
    }
    CHECK(inside == 0.0);
    CHECK(onRing > 200.0);
    CHECK(raster->info().animatedClusters > 10);

    // Upright characters (Perpendicular off) still follow the ring but do not turn: the ink differs.
    graphic.layers[0].text.path->perpendicular = false;
    const auto upright = rasterizeGraphic(resolveGraphic(graphic, Ticks{}, realText()), kW, kH);
    CHECK(upright->rgba() != raster->rgba());
}

TEST_CASE("0.2: text on a path through the compositor (GPU) equals its CPU raster", "[render][gpu][graphics][textpath]") {
    Graphic graphic;
    graphic.width = kW;
    graphic.height = kH;
    GraphicLayer ring = textLayer(1, "AROUND THE WORLD AROUND THE", 18.0, 160, 90);
    TextPath path;
    bool closed = false;
    path.path = {MaskPathKey{Ticks{}, edit::textPathVertices(edit::TextPathShape::Circle, 120.0, 0.0, &closed),
                             Interpolation::Linear}};
    path.closed = closed;
    ring.text.path = path;
    graphic.layers = {ring};
    const auto raster = rasterizeGraphic(resolveGraphic(graphic, Ticks{}, realText()), kW, kH);
    CompositeLayer layer;
    layer.view = raster->view(raster);
    auto renderer = makeRenderer();
    auto image = renderer->render(compositeOf({kW, kH}, {layer}));
    REQUIRE(image);
    savePng(*image, QStringLiteral("graphics/gpu_text_on_circle.png"));
    double worst = 0.0;
    for (int y = 0; y < kH; y += 2) {
        for (int x = 0; x < kW; x += 2) {
            const std::uint8_t* p = raster->rgba().data() + (static_cast<std::size_t>(y) * kW + x) * 4;
            const double alpha = p[3] / 255.0;
            Rgb expected{};
            for (int c = 0; c < 3; ++c) {
                const double straight = alpha > 0.0 ? std::min(1.0, p[c] / 255.0 / alpha) : 0.0;
                const double linear = straight <= 0.04045 ? straight / 12.92 : std::pow((straight + 0.055) / 1.055, 2.4);
                expected[static_cast<std::size_t>(c)] = linearToOutput(OutputTransform::Rec709, linear * alpha);
            }
            worst = std::max(worst, maxDiff(pixelOf(*image, x, y), expected));
        }
    }
    CHECK(worst <= 3.0 / 255.0);
}

TEST_CASE("0.2: text on a line starts at the first margin and Force Alignment spreads it to the last", "[render][graphics][textpath]") {
    Graphic graphic;
    graphic.width = kW;
    graphic.height = kH;
    GraphicLayer text = textLayer(1, "ABC", 24.0, 0, 100);
    TextPath path;
    path.path = {MaskPathKey{Ticks{}, {MaskVertex{20, 0, 0, 0, 0, 0}, MaskVertex{300, 0, 0, 0, 0, 0}}, Interpolation::Linear}};
    path.firstMargin = 40.0;
    text.text.path = path;
    graphic.layers = {text};
    const QRect atMargin = inkOf(*rasterizeGraphic(resolveGraphic(graphic, Ticks{}, realText()), kW, kH));
    REQUIRE_FALSE(atMargin.isEmpty());
    CHECK(std::abs(atMargin.left() - 60) <= 4);   // 20 + 40
    CHECK(atMargin.bottom() <= 100 + 2);          // standing on the line
    CHECK(atMargin.bottom() >= 100 - 3);
    graphic.layers[0].text.path->forceAlignment = true;
    graphic.layers[0].text.path->lastMargin = 20.0;
    const QRect spread = inkOf(*rasterizeGraphic(resolveGraphic(graphic, Ticks{}, realText()), kW, kH));
    INFO(std::format("margin {} {}, spread {} {}", atMargin.left(), atMargin.right(), spread.left(), spread.right()));
    CHECK(spread.width() > atMargin.width() * 3);
    CHECK(spread.right() <= 300 - 20 + 12);
    // Animated margin: the text travels along the path.
    Curve travel;
    travel.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{Ticks{kTicksPerSecond}, 200.0}};
    graphic.layers[0].text.path->forceAlignment = false;
    graphic.layers[0].text.path->firstMargin = ParamValue(travel);
    const QRect start = inkOf(*rasterizeGraphic(resolveGraphic(graphic, Ticks{}, realText()), kW, kH));
    const QRect later = inkOf(*rasterizeGraphic(resolveGraphic(graphic, Ticks{kTicksPerSecond / 2}, realText()), kW, kH));
    CHECK(std::abs((later.left() - start.left()) - 100) <= 4);
}
