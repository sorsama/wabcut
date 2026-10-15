// Titles v1 (render::TitleRenderer): Thai shaping (cluster boundaries, marks that do not advance, no missing
// glyphs), deterministic pixels, alignment, stroke, and the title composited by the GPU over a background.

#include "RenderTestSupport.h"

#include <wabcut/render/Title.h>

#include <QColor>
#include <QImage>
#include <QPainter>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

TitleStyle styleOf(std::string text, double size = 64.0) {
    TitleStyle s;
    s.text = std::move(text);
    s.sizePx = size;
    s.frameWidth = 640;
    s.frameHeight = 360;
    return s;
}

QImage imageOf(const TitleRaster& raster) {
    return QImage(raster.rgba().data(), raster.width(), raster.height(), raster.width() * 4,
                  QImage::Format_RGBA8888_Premultiplied)
        .copy();
}

double alphaAt(const TitleRaster& raster, int x, int y) {
    return raster.rgba()[(static_cast<std::size_t>(y) * static_cast<std::size_t>(raster.width()) + static_cast<std::size_t>(x)) * 4 + 3] / 255.0;
}

} // namespace

TEST_CASE("Titles: Thai is shaped into clusters, marks do not advance, nothing is missing", "[render][titles]") {
    // sa wo (mai han-akat) sa do (sara ii) kho ro (mai han-akat) bo: three combining marks at offsets 2, 5 and 8.
    const std::string thai = "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5\xE0\xB8\x84"
                             "\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A";
    const std::vector<int> boundaries = TitleRenderer::clusterBoundaries(styleOf(thai));
    CHECK(boundaries == std::vector<int>{0, 1, 3, 4, 6, 7, 9, 10});

    const auto raster = TitleRenderer::renderUncached(styleOf(thai));
    const TitleLayoutInfo& info = raster->info();
    INFO("fonts used: " << (info.fonts.empty() ? std::string() : info.fonts.front()));
    CHECK(info.lines == 1);
    CHECK(info.missingGlyphs == 0);
    CHECK(info.glyphs >= 7);
    CHECK(info.glyphs <= 10);

    // The same text without its marks is (almost) as wide: marks sit on their base without advancing.
    const std::string bases = "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\x9A";
    const auto plain = TitleRenderer::renderUncached(styleOf(bases));
    INFO("width with marks " << info.bounds[2] << ", without " << plain->info().bounds[2]);
    CHECK(std::abs(info.bounds[2] - plain->info().bounds[2]) < 0.05 * plain->info().bounds[2]);
    // ...but taller: the marks stand above the line.
    CHECK(info.bounds[1] < plain->info().bounds[1] - 2.0);
    QImage onDark(raster->width(), raster->height(), QImage::Format_RGB32);
    onDark.fill(QColor(20, 24, 40));
    {
        QPainter painter(&onDark);
        painter.drawImage(0, 0, imageOf(*raster));
    }
    onDark.save(outputDir() + QStringLiteral("/title_thai.png"));
}

TEST_CASE("Titles: rendering is deterministic and cached", "[render][titles]") {
    TitleStyle style = styleOf("Wabcut 2026\nSecond line", 48.0);
    style.strokeWidth = 3.0;
    const auto a = TitleRenderer::renderUncached(style);
    const auto b = TitleRenderer::renderUncached(style);
    CHECK(a->rgba() == b->rgba());
    CHECK(a->info().lines == 2);

    TitleRenderer renderer(4);
    const auto first = renderer.render(style);
    const auto again = renderer.render(style);
    CHECK(first.get() == again.get()); // one raster: the importer keeps it resident
    TitleStyle other = style;
    other.text = "Other";
    CHECK(renderer.render(other).get() != first.get());
}

TEST_CASE("Titles: alignment, stroke and fill", "[render][titles]") {
    TitleStyle left = styleOf("Short\nA much longer line", 40.0);
    left.align = TitleAlign::Left;
    TitleStyle right = left;
    right.align = TitleAlign::Right;
    const auto l = TitleRenderer::renderUncached(left);
    const auto r = TitleRenderer::renderUncached(right);
    // The block is centred: the same overall bounds, but the short line sits at the left or the right.
    CHECK(std::abs(l->info().bounds[0] - r->info().bounds[0]) < 2.0);
    const int shortLineY = static_cast<int>(l->info().bounds[1] + 15);
    const int leftX = static_cast<int>(l->info().bounds[0] + 10);
    const int rightX = static_cast<int>(l->info().bounds[0] + l->info().bounds[2] - 10);
    double leftInk = 0;
    double rightInk = 0;
    for (int dy = -8; dy <= 8; ++dy) {
        for (int dx = -8; dx <= 8; ++dx) {
            leftInk += alphaAt(*l, leftX + dx, shortLineY + dy) - alphaAt(*r, leftX + dx, shortLineY + dy);
            rightInk += alphaAt(*r, rightX + dx, shortLineY + dy) - alphaAt(*l, rightX + dx, shortLineY + dy);
        }
    }
    CHECK(leftInk > 5.0);
    CHECK(rightInk > 5.0);

    TitleStyle stroked = styleOf("O", 160.0);
    stroked.fill = {1.0, 1.0, 0.0};
    stroked.strokeWidth = 10.0;
    stroked.stroke = {0.0, 0.0, 1.0};
    const auto s = TitleRenderer::renderUncached(stroked);
    TitleStyle bare = stroked;
    bare.strokeWidth = 0.0;
    const auto b = TitleRenderer::renderUncached(bare);
    CHECK(s->info().bounds[2] == b->info().bounds[2] + 20.0);
    // Along the middle row, the first inked pixel is the blue stroke, then the yellow fill.
    const QImage image = imageOf(*s);
    const int y = 180;
    int first = -1;
    for (int x = 0; x < image.width(); ++x) {
        if (qAlpha(image.pixel(x, y)) > 250) {
            first = x;
            break;
        }
    }
    REQUIRE(first >= 0);
    const QColor stroke = image.pixelColor(first + 3, y);
    CHECK(stroke.blue() > 200);
    CHECK(stroke.red() < 60);
    const QColor fill = image.pixelColor(first + 18, y);
    CHECK(fill.red() > 200);
    CHECK(fill.green() > 200);
    CHECK(fill.blue() < 60);
}

TEST_CASE("Titles: composited by the GPU over a background", "[render][gpu][titles]") {
    auto renderer = makeRenderer(OutputRole::Export);
    TitleStyle style = styleOf("TITLE", 120.0);
    style.fill = {1.0, 1.0, 1.0};
    auto raster = TitleRenderer::shared().render(style);
    CompositeLayer background = layerOf(makeRgbaFrame(640, 360, [](int, int) {
        return std::array<std::uint8_t, 4>{30, 60, 120, 255};
    }));
    CompositeLayer title = layerOf(raster->view(raster));
    title.transform.position = {0.0, 100.0}; // moved down by Motion
    auto image = renderer->render(compositeOf({640, 360}, {background, title}));
    REQUIRE(image);
    savePng(*image, QStringLiteral("title_composite.png"));
    const auto& bounds = raster->info().bounds;
    // Ink of the title, 100 px lower than in its own frame; the background elsewhere.
    int white = 0;
    for (int x = static_cast<int>(bounds[0]); x < static_cast<int>(bounds[0] + bounds[2]); ++x) {
        const Rgb c = pixelOf(*image, x, static_cast<int>(bounds[1] + bounds[3] / 2) + 100);
        white += c[0] > 0.9 && c[1] > 0.9 && c[2] > 0.9 ? 1 : 0;
    }
    CHECK(white > 20);
    const Rgb bg = pixelOf(*image, 20, 20);
    CHECK(bg[2] > bg[0]);
    const Rgb above = pixelOf(*image, static_cast<int>(bounds[0] + bounds[2] / 2), static_cast<int>(bounds[1] + bounds[3] / 2));
    CHECK(maxDiff(above, bg) < 0.02); // the title is no longer at its frame position
}
