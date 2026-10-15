// Masks on the GPU (shaders/mask.frag, I.6) against the CPU reference (render::maskCoverageCpu, exact distances):
// shapes, feather, expansion, opacity, invert, the add / subtract / intersect combination and the tracker's
// transform, as opacity masks (PSNR); masked effects as local corrections (the effect's CPU reference mixed by the
// reference coverage); the mosaic; redaction styles on a layer's own pixels, also on a transformed layer; the mask
// texture cache; and the GPU cost on a 1080p layer. Rendered pairs land in test-output/render/masks/.

#include "EffectsReference.h"
#include "RenderTestSupport.h"

#include <wabcut/color/Grade.h>
#include <wabcut/render/Compositor.h>
#include <wabcut/render/Mask.h>

#include <QDir>
#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

constexpr int kW = 480;
constexpr int kH = 270;

std::uint8_t byte(double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

std::array<std::uint8_t, 4> white(int, int) { return {255, 255, 255, 255}; }

/// A busy opaque picture (gradients, a checkerboard, lines): blurs and mosaics show.
std::array<std::uint8_t, 4> busy(int x, int y) {
    const bool check = ((x / 20) + (y / 20)) % 2 == 0;
    if (x % 40 == 7 || y % 30 == 11) {
        return {245, 245, 240, 255};
    }
    return {byte(x / double(kW)), byte(0.15 + 0.7 * y / double(kH)), check ? std::uint8_t(200) : std::uint8_t(40), 255};
}

QString masksDir() {
    const QString dir = outputDir() + QStringLiteral("/masks");
    QDir().mkpath(dir);
    return dir;
}

Mask ellipse(double cx, double cy, double w, double h) {
    Mask m;
    m.shape = MaskShape::Ellipse;
    m.set("center.x", cx);
    m.set("center.y", cy);
    m.set("size.x", w);
    m.set("size.y", h);
    return m;
}

Mask rectangle(double cx, double cy, double w, double h, double rotation = 0.0, double roundness = 0.0) {
    Mask m = ellipse(cx, cy, w, h);
    m.shape = MaskShape::Rectangle;
    m.set("rotation", rotation);
    m.set("roundness", roundness);
    return m;
}

Mask path(std::vector<MaskVertex> vertices) {
    Mask m;
    m.shape = MaskShape::Path;
    m.path = {MaskPathKey{Ticks{0}, std::move(vertices), Interpolation::Linear}};
    return m;
}

/// A free Bezier outline: a lopsided five-point blob with smooth and corner vertices.
Mask blob() {
    return path({{0.30, 0.25, -0.05, 0.05, 0.10, -0.08},
                 {0.62, 0.20, 0.0, 0.0, 0.0, 0.0},
                 {0.78, 0.55, 0.0, -0.12, 0.0, 0.12},
                 {0.50, 0.82, 0.12, 0.0, -0.15, 0.0},
                 {0.22, 0.60, 0.0, 0.10, 0.0, -0.10}});
}

Mask with(Mask m, std::initializer_list<std::pair<const char*, double>> values, MaskMode mode = MaskMode::Add,
          bool inverted = false) {
    for (const auto& [key, value] : values) {
        m.set(key, value);
    }
    m.mode = mode;
    m.inverted = inverted;
    return m;
}

std::shared_ptr<const MaskSpec> specOf(const std::vector<Mask>& masks, int w = kW, int h = kH) {
    auto spec = maskSpecAt(masks, Ticks{0}, static_cast<double>(w) / h);
    REQUIRE(spec);
    return spec;
}

LayerEffect opacityMask(std::shared_ptr<const MaskSpec> spec) {
    LayerEffect e;
    e.type = EffectType::OpacityMask;
    e.mask = std::move(spec);
    return e;
}

LayerEffect effect(EffectType type, std::initializer_list<double> params, std::shared_ptr<const MaskSpec> mask = {}) {
    LayerEffect e;
    e.type = type;
    std::copy(params.begin(), params.end(), e.params.begin());
    e.mask = std::move(mask);
    return e;
}

LinearImage coverageImage(const MaskSpec& spec, int w = kW, int h = kH) {
    const std::vector<float> c = maskCoverageCpu(spec, w, h);
    LinearImage out(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double v = c[static_cast<std::size_t>(y) * w + x];
            out.at(x, y) = {v, v, v, v};
        }
    }
    return out;
}

/// `effected` mixed into `input` by `coverage` (premultiplied linear), the masked effect's reference.
LinearImage mixed(const LinearImage& input, const LinearImage& effected, const LinearImage& coverage) {
    LinearImage out(input.width, input.height);
    for (std::size_t i = 0; i < out.px.size(); ++i) {
        const double c = coverage.px[i][3];
        for (int k = 0; k < 4; ++k) {
            out.px[i][k] = input.px[i][k] + (effected.px[i][k] - input.px[i][k]) * c;
        }
    }
    return out;
}

double renderAndCompare(OffscreenRenderer& renderer, const std::shared_ptr<PreparedComposite>& composite,
                        const LinearImage& expected, const std::string& name) {
    auto image = renderer.render(composite);
    REQUIRE(image);
    const std::vector<std::uint8_t> reference =
        encodeOverBlack(expected, outputTransformFor(composite->color, OutputRole::Export));
    savePng(*image, QStringLiteral("masks/%1.png").arg(QString::fromStdString(name)));
    QImage(reference.data(), expected.width, expected.height, expected.width * 3, QImage::Format_RGB888)
        .save(masksDir() + QStringLiteral("/%1.reference.png").arg(QString::fromStdString(name)));
    return psnrRgb(*image, reference);
}

/// Luma spread (standard deviation, 0..1) over a block of an output image.
double spread(const RenderedImage& image, int x0, int y0, int size) {
    double sum = 0.0;
    double sum2 = 0.0;
    for (int y = y0; y < y0 + size; ++y) {
        for (int x = x0; x < x0 + size; ++x) {
            const Rgb c = pixelOf(image, x, y);
            const double l = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
            sum += l;
            sum2 += l * l;
        }
    }
    const double n = static_cast<double>(size) * size;
    return std::sqrt(std::max(0.0, sum2 / n - (sum / n) * (sum / n)));
}

/// Largest difference between any pixel of a block and its first pixel.
double blockRange(const RenderedImage& image, int x0, int y0, int x1, int y1) {
    const Rgb first = pixelOf(image, x0, y0);
    double worst = 0.0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            worst = std::max(worst, maxDiff(pixelOf(image, x, y), first));
        }
    }
    return worst;
}

} // namespace

TEST_CASE("Masks: GPU coverage matches the CPU reference", "[render][gpu][masks]") {
    auto renderer = makeRenderer(OutputRole::Export);
    struct Case {
        std::string name;
        std::vector<Mask> masks;
        double minPsnr;
    };
    std::vector<Mask> six;
    for (int i = 0; i < 6; ++i) {
        six.push_back(ellipse(0.12 + 0.15 * i, 0.3 + 0.08 * (i % 3), 0.12, 0.2));
    }
    six[4].mode = MaskMode::Subtract;
    const std::vector<Case> cases = {
        {"ellipse", {ellipse(0.5, 0.5, 0.6, 0.5)}, 38.0},
        {"ellipse_feather", {with(ellipse(0.45, 0.55, 0.5, 0.6), {{"feather", 0.12}})}, 38.0},
        {"rectangle_rotated_round", {rectangle(0.5, 0.5, 0.5, 0.4, 30.0, 0.4)}, 38.0},
        {"bezier_path", {blob()}, 38.0},
        {"bezier_path_feather", {with(blob(), {{"feather", 0.05}})}, 38.0},
        {"inverted_half_opacity", {with(ellipse(0.5, 0.5, 0.4, 0.4), {{"opacity", 0.6}, {"feather", 0.03}}, MaskMode::Add, true)}, 38.0},
        {"expansion_grow", {with(rectangle(0.5, 0.5, 0.4, 0.3), {{"expansion", 0.06}, {"feather", 0.02}})}, 38.0},
        {"expansion_shrink", {with(blob(), {{"expansion", -0.04}})}, 38.0},
        {"subtract", {ellipse(0.5, 0.5, 0.7, 0.7), with(rectangle(0.5, 0.5, 0.25, 0.25), {{"feather", 0.02}}, MaskMode::Subtract)}, 38.0},
        {"intersect", {ellipse(0.4, 0.5, 0.5, 0.7), with(ellipse(0.6, 0.5, 0.5, 0.7), {}, MaskMode::Intersect)}, 38.0},
        {"first_subtracts", {with(ellipse(0.5, 0.5, 0.3, 0.3), {{"feather", 0.04}}, MaskMode::Subtract)}, 38.0},
        {"transform", {with(blob(), {{"transform.x", 0.08}, {"transform.y", -0.05}, {"transform.scaleX", 1.3},
                                     {"transform.scaleY", 0.7}, {"transform.rotation", 20.0}, {"transform.skew", 10.0}})},
         38.0},
        {"six_shapes_ping_pong", six, 38.0},
        {"offscreen_shape_translated_inside",
         {with(ellipse(2.5, 0.5, 0.6, 0.5), {{"transform.x", -2.0}, {"feather", 0.04}})}, 38.0},
        {"different_offscreen_shape_domains",
         {with(ellipse(2.4, 0.5, 0.5, 0.6), {{"transform.x", -2.0}, {"feather", 0.03}}),
          with(rectangle(-1.4, 0.5, 0.3, 0.4), {{"transform.x", 2.0}, {"feather", 0.03}}, MaskMode::Subtract)}, 38.0},
        // Codex wave-4 P2-32: a small shape near the top edge whose soft edge reaches past the distance texture's
        // fixed margin (feather / 2 + expansion, up to 0.9 frame heights): the lower part of the frame lies beyond the
        // texture, where the GPU used to plateau.
        {"feather_past_the_margin", {with(ellipse(0.5, 0.12, 0.1, 0.1), {{"feather", 1.0}, {"expansion", 0.4}})}, 38.0},
        {"expansion_past_the_margin", {with(rectangle(0.85, 0.1, 0.08, 0.08), {{"expansion", 0.75}, {"feather", 0.3}})}, 38.0},
        // Codex wave-4 P2-31: shapes that cover nothing (a two-point path, a collapsed transform) still combine: inverted
        // they cover everything, intersected they clear everything. The GPU used to skip them.
        {"inverted_two_point_path", {with(path({{0.2, 0.2, 0, 0, 0, 0}, {0.8, 0.7, 0, 0, 0, 0}}), {}, MaskMode::Add, true)}, 38.0},
        {"inverted_collapsed_transform", {with(ellipse(0.5, 0.5, 0.4, 0.4), {{"transform.scaleX", 0.0}}, MaskMode::Add, true)}, 38.0},
        {"intersect_collapsed_transform",
         {ellipse(0.5, 0.5, 0.6, 0.6), with(ellipse(0.5, 0.5, 0.3, 0.3), {{"transform.scaleY", 0.0}}, MaskMode::Intersect)}, 38.0},
        {"subtract_collapsed_transform",
         {ellipse(0.5, 0.5, 0.6, 0.6), with(ellipse(0.5, 0.5, 0.3, 0.3), {{"transform.scaleX", 0.0}}, MaskMode::Subtract)}, 38.0},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            const auto spec = specOf(c.masks);
            CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, white));
            layer.effects = {opacityMask(spec)};
            const double psnr = renderAndCompare(*renderer, compositeOf({kW, kH}, {layer}), coverageImage(*spec), c.name);
            WARN(c.name << ": PSNR " << psnr << " dB");
            CHECK(psnr >= c.minPsnr);
        }
    }
}

// Codex wave-4 P2-31, the reference itself: a shape that covers nothing (fewer than three points, or a collapsed
// transform) has coverage 0 before invert, opacity and the combination, like any shape. The CPU reference used to skip
// collapsed transforms, so an Intersect with one changed nothing (docs/masks.md says it clears everything). Failed
// before the fix.
TEST_CASE("Masks: the CPU reference combines shapes that cover nothing", "[render][masks]") {
    const auto range = [](const std::vector<Mask>& masks) {
        const std::vector<float> c = maskCoverageCpu(*specOf(masks), 64, 36);
        const auto [lo, hi] = std::minmax_element(c.begin(), c.end());
        return std::pair{static_cast<double>(*lo), static_cast<double>(*hi)};
    };
    const Mask collapsed = with(ellipse(0.5, 0.5, 0.3, 0.3), {{"transform.scaleX", 0.0}});
    CHECK(range({collapsed}) == std::pair{0.0, 0.0});
    CHECK(range({with(collapsed, {}, MaskMode::Add, true)}) == std::pair{1.0, 1.0});           // inverted: everything
    CHECK(range({ellipse(0.5, 0.5, 0.6, 0.6), with(collapsed, {}, MaskMode::Intersect)}) == std::pair{0.0, 0.0});
    CHECK(range({ellipse(0.5, 0.5, 0.6, 0.6), with(collapsed, {}, MaskMode::Subtract)}).second == 1.0); // unchanged
    const Mask twoPoints = path({{0.2, 0.2, 0, 0, 0, 0}, {0.8, 0.7, 0, 0, 0, 0}});
    CHECK(range({with(twoPoints, {}, MaskMode::Add, true)}) == std::pair{1.0, 1.0});
}

TEST_CASE("Masks: masked effects change only the masked area (local corrections)", "[render][gpu][masks]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const auto feathered = specOf({with(ellipse(0.5, 0.5, 0.5, 0.6), {{"feather", 0.08}})});
    const LinearImage input = linearFromRgba8(kW, kH, busy);
    const LinearImage coverage = coverageImage(*feathered);

    SECTION("correction and blur match their references mixed by the coverage") {
        const std::vector<std::pair<std::string, LayerEffect>> cases = {
            {"local_correction", effect(EffectType::BasicCorrection, {1.0, 30.0, 150.0, 40.0, -20.0}, feathered)},
            {"local_blur", effect(EffectType::GaussianBlur, {12.0, 1.0}, feathered)},
        };
        for (const auto& [name, e] : cases) {
            CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, busy));
            layer.effects = {e};
            const LinearImage expected = mixed(input, referenceEffect(input, e), coverage);
            const double psnr = renderAndCompare(*renderer, compositeOf({kW, kH}, {layer}), expected, name);
            WARN(name << ": PSNR " << psnr << " dB");
            CHECK(psnr >= 40.0);
        }
    }

    SECTION("a masked grade is the grade inside and the picture outside") {
        color::GradeSpec g;
        g.params.exposure = 1.0;
        g.params.saturation = 40;
        REQUIRE(g.active());
        LayerEffect grade;
        grade.type = EffectType::Grade;
        grade.grade = std::make_shared<const color::GradeSpec>(g);
        const auto hard = specOf({ellipse(0.5, 0.5, 0.5, 0.6)});
        const auto renderWith = [&](std::vector<LayerEffect> effects, const char* name) {
            CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, busy));
            layer.effects = std::move(effects);
            auto image = renderer->render(compositeOf({kW, kH}, {layer}));
            REQUIRE(image);
            savePng(*image, QStringLiteral("masks/%1.png").arg(QLatin1String(name)));
            return std::move(*image);
        };
        LayerEffect masked = grade;
        masked.mask = hard;
        const RenderedImage plain = renderWith({}, "grade_none");
        const RenderedImage whole = renderWith({grade}, "grade_whole");
        const RenderedImage local = renderWith({masked}, "local_grade");
        for (const auto& [x, y] : std::vector<std::pair<int, int>>{{240, 135}, {200, 110}, {290, 170}}) {
            INFO("inside " << x << "," << y);
            CHECK(maxDiff(pixelOf(local, x, y), pixelOf(whole, x, y)) <= 1.5 / 255);
        }
        for (const auto& [x, y] : std::vector<std::pair<int, int>>{{10, 10}, {470, 260}, {20, 135}}) {
            INFO("outside " << x << "," << y);
            CHECK(maxDiff(pixelOf(local, x, y), pixelOf(plain, x, y)) <= 1.5 / 255);
        }
        CHECK(maxDiff(pixelOf(whole, 10, 10), pixelOf(plain, 10, 10)) > 0.05); // the grade does show
    }
}

TEST_CASE("Masks: an opacity mask cuts a layer out over the one below", "[render][gpu][masks]") {
    auto renderer = makeRenderer(OutputRole::Export);
    CompositeLayer below = layerOf(makeRgbaFrame(kW, kH, [](int, int) { return std::array<std::uint8_t, 4>{255, 0, 0, 255}; }));
    CompositeLayer above = layerOf(makeRgbaFrame(kW, kH, busy));
    above.effects = {opacityMask(specOf({ellipse(0.5, 0.5, 0.5, 0.5)}))};
    auto image = renderer->render(compositeOf({kW, kH}, {below, above}));
    REQUIRE(image);
    savePng(*image, QStringLiteral("masks/cut_out.png"));
    CHECK(maxDiff(pixelOf(*image, 5, 5), {1.0, 0.0, 0.0}) <= 1.0 / 255);
    CHECK(maxDiff(pixelOf(*image, 470, 260), {1.0, 0.0, 0.0}) <= 1.0 / 255);
    const Rgb centre = pixelOf(*image, 245, 140);
    CHECK(maxDiff(centre, {1.0, 0.0, 0.0}) > 0.2);
}

TEST_CASE("Masks: mosaic and the redaction styles", "[render][gpu][masks][redaction]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const auto render = [&](CompositeLayer layer, const char* name) {
        auto image = renderer->render(compositeOf({kW, kH}, {std::move(layer)}));
        REQUIRE(image);
        savePng(*image, QStringLiteral("masks/%1.png").arg(QLatin1String(name)));
        return std::move(*image);
    };
    const RenderedImage plain = render(layerOf(makeRgbaFrame(kW, kH, busy)), "plain");
    const auto region = specOf({ellipse(0.5, 0.5, 0.4, 0.5)});
    const auto redacted = [&](LayerRedaction::Style style, std::array<double, 3> color = {}) {
        CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, busy));
        LayerRedaction r;
        r.style = style;
        r.strength = 0.05; // 13.5 canvas px
        r.color = color;
        r.mask = region;
        layer.redactions = {r};
        return layer;
    };
    // Mosaic cells are 0.05 * 270 = 13.5 canvas px; cell (17, 9) spans x 229.5..243, y 121.5..135.
    SECTION("the mosaic effect") {
        CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, busy));
        layer.effects = {effect(EffectType::Mosaic, {0.05})};
        const RenderedImage image = render(layer, "mosaic");
        CHECK(blockRange(image, 231, 123, 241, 133) <= 1.0 / 255);
        CHECK(blockRange(image, 15, 15, 25, 25) <= 1.0 / 255); // cell (1, 1)
        CHECK(maxDiff(pixelOf(image, 236, 128), pixelOf(image, 236 + 27, 128)) > 0.0);
    }
    SECTION("solid") {
        const RenderedImage image = render(redacted(LayerRedaction::Style::Solid, {1.0, 0.0, 0.0}), "redaction_solid");
        CHECK(maxDiff(pixelOf(image, 240, 135), {1.0, 0.0, 0.0}) <= 1.0 / 255);
        CHECK(maxDiff(pixelOf(image, 200, 100), {1.0, 0.0, 0.0}) <= 1.0 / 255);
        CHECK(maxDiff(pixelOf(image, 10, 10), pixelOf(plain, 10, 10)) <= 1.0 / 255);
        CHECK(maxDiff(pixelOf(image, 470, 260), pixelOf(plain, 470, 260)) <= 1.0 / 255);
    }
    SECTION("blur") {
        const RenderedImage image = render(redacted(LayerRedaction::Style::Blur), "redaction_blur");
        const double before = spread(plain, 225, 120, 30);
        const double after = spread(image, 225, 120, 30);
        WARN("luma spread in the region: " << before << " -> " << after);
        CHECK(after < 0.4 * before);
        CHECK(maxDiff(pixelOf(image, 10, 10), pixelOf(plain, 10, 10)) <= 1.0 / 255);
    }
    SECTION("mosaic") {
        const RenderedImage image = render(redacted(LayerRedaction::Style::Mosaic), "redaction_mosaic");
        CHECK(blockRange(image, 231, 123, 241, 133) <= 1.0 / 255);
        CHECK(maxDiff(pixelOf(image, 10, 10), pixelOf(plain, 10, 10)) <= 1.0 / 255);
    }
    SECTION("on a transformed layer, before its effects") {
        // Frame point (0.25, 0.25) of a half-size centred layer lands at canvas (180, 101.25).
        CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, busy), LayerTransform{{0, 0}, 0.5, 0.0, {0, 0}});
        LayerRedaction r;
        r.style = LayerRedaction::Style::Solid;
        r.color = {1.0, 0.0, 0.0};
        r.mask = specOf({ellipse(0.25, 0.25, 0.1, 0.12)});
        layer.redactions = {r};
        layer.effects = {effect(EffectType::Crop, {0.0, 0.0, 0.5, 0.0, 0.0})}; // keeps the left half
        const RenderedImage image = render(layer, "redaction_transformed");
        CHECK(maxDiff(pixelOf(image, 180, 101), {1.0, 0.0, 0.0}) <= 1.0 / 255);
        CHECK(maxDiff(pixelOf(image, 300, 169), {0.0, 0.0, 0.0}) <= 1.0 / 255); // cropped away after the redaction
        CHECK(maxDiff(pixelOf(image, 150, 160), {1.0, 0.0, 0.0}) > 0.2);
    }
}

TEST_CASE("Masks: textures re-render only when their content changes", "[render][gpu][masks]") {
    OffscreenOptions options;
    auto created = OffscreenRenderer::create(options);
    REQUIRE(created);
    OffscreenRenderer& renderer = **created;
    const media::VideoFrame frame = makeRgbaFrame(kW, kH, busy);
    const auto statsOf = [&](std::vector<Mask> masks) {
        CompositeLayer layer = layerOf(frame);
        layer.effects = {effect(EffectType::GaussianBlur, {10.0, 1.0}, specOf(masks))};
        auto t = renderer.renderTimed(compositeOf({kW, kH}, {layer}));
        REQUIRE(t);
        return t->composite;
    };
    const Mask a = with(blob(), {{"feather", 0.03}});
    const CompositeStats first = statsOf({a});
    CHECK(first.maskTexturesRendered == 2); // the distances and the coverage
    const CompositeStats again = statsOf({a});
    CHECK(again.maskTexturesRendered == 0); // static: from the cache
    CHECK(again.maskPasses == 1);           // only the masked mix
    const CompositeStats moved = statsOf({with(a, {{"transform.x", 0.05}, {"transform.rotation", 5.0}})});
    CHECK(moved.maskTexturesRendered == 1); // tracked: the coverage only, the distances are reused
    const CompositeStats reshaped = statsOf({with(a, {{"feather", 0.03}}), ellipse(0.2, 0.2, 0.1, 0.1)});
    CHECK(reshaped.maskTexturesRendered == 2); // the new shape's distances and the coverage
}

TEST_CASE("Masks: GPU cost on a 1080p layer", "[render][gpu][masks][perf]") {
    OffscreenOptions options;
    options.timestamps = true;
    auto created = OffscreenRenderer::create(options);
    REQUIRE(created);
    OffscreenRenderer& renderer = **created;
    const media::VideoFrame frame = makeRgbaFrame(1920, 1080, [](int x, int y) {
        return std::array<std::uint8_t, 4>{byte(x / 1920.0), byte(y / 1080.0), byte(0.5), 255};
    });
    const auto blob1080 = [](double shift) { return with(blob(), {{"feather", 0.02}, {"transform.x", shift}}); };
    // Median GPU ms of a frame; `effectsAt(i)` gives frame i's effects (animated masks change every frame).
    const auto timeOf = [&](const std::function<std::vector<LayerEffect>(int)>& effectsAt) {
        std::vector<double> gpu;
        for (int i = 0; i < 40; ++i) {
            CompositeLayer layer = layerOf(frame);
            layer.effects = effectsAt(i);
            auto t = renderer.renderTimed(compositeOf({1920, 1080}, {layer}));
            REQUIRE(t);
            if (i >= 5 && t->gpuMs >= 0) {
                gpu.push_back(t->gpuMs);
            }
        }
        std::sort(gpu.begin(), gpu.end());
        return gpu.empty() ? -1.0 : gpu[gpu.size() / 2];
    };
    const auto spec1080 = [](std::vector<Mask> masks) { return specOf(masks, 1920, 1080); };
    const auto staticSpec = spec1080({blob1080(0.0)});
    const double plain = timeOf([](int) { return std::vector<LayerEffect>{}; });
    const double staticMask = timeOf([&](int) { return std::vector<LayerEffect>{opacityMask(staticSpec)}; });
    const double tracked = timeOf([&](int i) { return std::vector<LayerEffect>{opacityMask(spec1080({blob1080(0.001 * i)}))}; });
    const double reshaped = timeOf([&](int i) {
        Mask m = blob1080(0.0);
        m.path[0].vertices[1].x += 0.001 * i; // the outline itself animates: distances every frame
        return std::vector<LayerEffect>{opacityMask(spec1080({m}))};
    });
    const double fourShapes = timeOf([&](int i) {
        return std::vector<LayerEffect>{opacityMask(spec1080({blob1080(0.001 * i), ellipse(0.3, 0.3, 0.2, 0.2),
                                                              with(ellipse(0.6, 0.6, 0.3, 0.2), {}, MaskMode::Subtract),
                                                              rectangle(0.7, 0.3, 0.2, 0.2, 15.0, 0.3)}))};
    });
    const double localBlur = timeOf([&](int) {
        return std::vector<LayerEffect>{effect(EffectType::GaussianBlur, {20.0, 1.0}, staticSpec)};
    });
    const double wholeBlur = timeOf([&](int) { return std::vector<LayerEffect>{effect(EffectType::GaussianBlur, {20.0, 1.0})}; });
    WARN("1080p GPU frame: plain " << plain << " ms, static mask " << staticMask << ", tracked " << tracked << ", reshaped "
                                   << reshaped << ", four shapes " << fourShapes << ", masked blur " << localBlur
                                   << " (blur " << wholeBlur << ") (" << renderer.adapterName() << ")");
    std::ofstream report((masksDir() + QStringLiteral("/perf.md")).toStdString(), std::ios::trunc);
    report << "# Mask GPU cost, one 1920x1080 layer (" << renderer.backendName() << ", " << renderer.adapterName() << ")\n\n"
           << "Median GPU time of a whole frame (layer, effects, output), 35 frames each:\n\n"
           << "| frame | GPU ms | mask ms |\n|---|---|---|\n"
           << "| no mask | " << plain << " | - |\n"
           << "| static Bezier mask (cached coverage, the alpha pass only) | " << staticMask << " | " << staticMask - plain << " |\n"
           << "| tracked mask (transform changes every frame: coverage re-rendered) | " << tracked << " | " << tracked - plain << " |\n"
           << "| reshaped mask (outline changes every frame: distances + coverage) | " << reshaped << " | " << reshaped - plain << " |\n"
           << "| four shapes, one tracked | " << fourShapes << " | " << fourShapes - plain << " |\n"
           << "| masked 20 px blur (vs the unmasked blur) | " << localBlur << " | " << localBlur - wholeBlur << " |\n";
    if (plain >= 0) {
        CHECK(staticMask - plain < 2.0);
        CHECK(reshaped - plain < 8.0);
    }
}
