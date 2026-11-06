// 0.7 (card 116): bilingual captions in the renderer - blocks of two caption tracks stack at the bottom or at the top
// edge without overlapping (CaptionFrame::closer), a custom position is never moved, and a stack taller than the picture
// stays inside the frame.

#include <wabcut/render/Caption.h>

#include <QDir>
#include <QImage>
#include <QPainter>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace wabcut;
using namespace wabcut::render;

namespace {

constexpr int kW = 640;
constexpr int kH = 360;

CaptionFrame frameOf(std::string text, CaptionStyle style = {}) {
    CaptionFrame f;
    f.text = std::move(text);
    f.frameWidth = kW;
    f.frameHeight = kH;
    f.style = style;
    return f;
}

} // namespace

TEST_CASE("Bilingual captions: blocks of two tracks stack at the bottom and at the top without overlapping",
          "[render][captions][bilingual]") {
    CaptionStyle style;
    style.box = true;
    // Bottom: the Spanish block at the edge, the English one above it (it lists the Spanish block as closer).
    CaptionFrame spanish = frameOf("Hola mundo", style);
    CaptionFrame english = frameOf("Hello world", style);
    english.closer.push_back({spanish.text, spanish.style});
    const auto lower = CaptionRenderer::renderUncached(spanish);
    const auto upper = CaptionRenderer::renderUncached(english);
    const auto& l = lower->info().bounds;
    const auto& u = upper->info().bounds;
    CHECK(l[1] + l[3] <= kH * 0.9 + 0.5); // the nearer block sits where a lone caption does
    CHECK(l[1] + l[3] >= kH * 0.9 - 0.5);
    CHECK(u[1] + u[3] < l[1]);            // the other one is above it, with a gap
    CHECK(std::abs((u[0] + u[2] / 2.0) - kW / 2.0) < 1.0);
    // Alone, the same text sits at the edge: the stack moved it up by the other block's height and the gap.
    const auto alone = CaptionRenderer::renderUncached(frameOf("Hello world", style));
    CHECK(alone->info().bounds[1] > u[1]);
    CHECK(std::abs((alone->info().bounds[1] - u[1]) - (l[3] + 0.25 * 48.0 * kH / 1080.0)) < 1.5);

    // Top: the first block at the edge, the second below it.
    CaptionStyle top = style;
    top.position = CaptionPosition::Top;
    CaptionFrame first = frameOf("Hello world", top);
    CaptionFrame second = frameOf("Hola mundo", top);
    second.closer.push_back({first.text, first.style});
    const auto firstRaster = CaptionRenderer::renderUncached(first);
    const auto secondRaster = CaptionRenderer::renderUncached(second);
    CHECK(firstRaster->info().bounds[1] >= kH * 0.1 - 0.5);
    CHECK(secondRaster->info().bounds[1] > firstRaster->info().bounds[1] + firstRaster->info().bounds[3]);

    // A custom position is never moved by a neighbour.
    CaptionStyle custom = style;
    custom.position = CaptionPosition::Custom;
    CaptionFrame placed = frameOf("Hello world", custom);
    const auto before = CaptionRenderer::renderUncached(placed);
    placed.closer.push_back({"Hola mundo", custom});
    const auto after = CaptionRenderer::renderUncached(placed);
    CHECK(before->info().bounds == after->info().bounds);
}

TEST_CASE("Bilingual captions: an English line over a smaller yellow Spanish one; picture written for a look",
          "[render][captions][bilingual]") {
    CaptionStyle english;
    english.sizePx = 60.0;
    CaptionStyle spanish;
    spanish.sizePx = 42.0;
    spanish.fill = {1.0, 0.85, 0.25};
    CaptionFrame lower = frameOf("Esto es una prueba de subtitulos en dos idiomas", spanish);
    CaptionFrame upper = frameOf("This is a test of subtitles in two languages", english);
    upper.closer.push_back({lower.text, lower.style});
    const auto lowerRaster = CaptionRenderer::renderUncached(lower);
    const auto upperRaster = CaptionRenderer::renderUncached(upper);
    CHECK(upperRaster->info().bounds[1] + upperRaster->info().bounds[3] < lowerRaster->info().bounds[1]);

    QImage picture(kW, kH, QImage::Format_ARGB32_Premultiplied);
    picture.fill(QColor(70, 100, 130));
    QPainter painter(&picture);
    for (const auto* raster : {upperRaster.get(), lowerRaster.get()}) {
        const QImage layer(raster->rgba().data(), raster->width(), raster->height(), raster->width() * 4,
                           QImage::Format_RGBA8888_Premultiplied);
        painter.drawImage(0, 0, layer);
    }
    painter.end();
    const QString dir = QStringLiteral(WABCUT_TEST_OUTPUT_DIR "/ui");
    QDir().mkpath(dir);
    CHECK(picture.save(dir + QStringLiteral("/bilingual-captions.png")));
}

TEST_CASE("Bilingual captions: the stack is part of the cached picture and a stack taller than the frame stays inside it",
          "[render][captions][bilingual]") {
    CaptionStyle style;
    CaptionFrame plain = frameOf("Hello world", style);
    CaptionFrame stacked = plain;
    stacked.closer.push_back({"Hola mundo", style});
    CHECK_FALSE(plain == stacked); // two frames that differ only in the stack must not share a cached raster
    CaptionRenderer renderer(4);
    CHECK(renderer.render(plain) != renderer.render(stacked));

    CaptionFrame tall = frameOf("Hello world", style);
    for (int i = 0; i < 12; ++i) {
        tall.closer.push_back({"A line of another track\nand a second line", style});
    }
    const auto raster = CaptionRenderer::renderUncached(tall);
    const auto& b = raster->info().bounds;
    CHECK(b[1] >= 0.0);
    CHECK(b[1] + b[3] <= kH + 0.5);

    CHECK(captionBoxHeight("Hello world", style, kW, kH) > 0.0);
    CHECK(captionBoxHeight("Hello world\nand more", style, kW, kH) > captionBoxHeight("Hello world", style, kW, kH));
}
