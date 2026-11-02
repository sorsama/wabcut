// 0.6, text in Effect Controls (roadmap #75): the new text properties as the rasteriser draws them - strokes outside,
// on and inside the glyphs, faux bold and italic, All Caps and Small Caps, kerning, paragraph text placed at the top,
// centre or bottom of its box, a shadow's size, and keyed size and Source Text resolved per frame. Preview and export
// draw the same list, so these pixels are what both show. Images land in <build>/test-output/render/graphics/.

#include "RenderTestSupport.h"

#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/render/GraphicRaster.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QImage>

#include <cmath>
#include <format>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

DrawList listOf(std::vector<DrawItem> items) {
    DrawList list;
    list.width = kW;
    list.height = kH;
    list.items = std::move(items);
    return list;
}

std::array<double, 4> at(const GraphicRaster& r, int x, int y) {
    const std::uint8_t* p = r.rgba().data() + (static_cast<std::size_t>(y) * r.width() + x) * 4;
    const double a = p[3] / 255.0;
    if (a <= 0.0) {
        return {0, 0, 0, 0};
    }
    return {p[0] / 255.0 / a, p[1] / 255.0 / a, p[2] / 255.0 / a, a};
}

void save(const GraphicRaster& r, const QString& name) {
    QDir().mkpath(outputDir() + QStringLiteral("/graphics"));
    QImage image(r.rgba().data(), r.width(), r.height(), r.width() * 4, QImage::Format_RGBA8888_Premultiplied);
    image.copy().save(outputDir() + QStringLiteral("/graphics/") + name);
}

/// Ink (alpha > 0.5) bounding box.
QRect inkOf(const GraphicRaster& r) {
    QRect box;
    for (int y = 0; y < r.height(); ++y) {
        for (int x = 0; x < r.width(); ++x) {
            if (at(r, x, y)[3] > 0.5) {
                box = box.united(QRect(x, y, 1, 1));
            }
        }
    }
    return box;
}

/// Pixels mostly of `rgb` (opaque enough), counted.
int count(const GraphicRaster& r, std::array<double, 3> rgb) {
    int n = 0;
    for (int y = 0; y < r.height(); ++y) {
        for (int x = 0; x < r.width(); ++x) {
            const auto p = at(r, x, y);
            if (p[3] > 0.8 && std::abs(p[0] - rgb[0]) < 0.1 && std::abs(p[1] - rgb[1]) < 0.1 && std::abs(p[2] - rgb[2]) < 0.1) {
                ++n;
            }
        }
    }
    return n;
}

DrawItem textItem(const std::string& text, double size, double x, double y) {
    DrawItem item;
    item.kind = DrawKind::Text;
    DrawTextRun run;
    run.text = text;
    run.fontSize = size;
    run.fill = {1.0, 1.0, 1.0, 1.0};
    item.text.runs = {run};
    item.transform.e = x;
    item.transform.f = y;
    return item;
}

std::shared_ptr<const GraphicRaster> draw(const DrawItem& item) { return rasterizeGraphic(listOf({item}), kW, kH); }

} // namespace

TEST_CASE("0.6: text strokes lie outside on or inside the glyphs and faux bold thickens them", "[render][graphics][text]") {
    const DrawItem plain = textItem("HI", 90.0, 40, 30);
    const auto base = draw(plain);
    const QRect ink = inkOf(*base);
    REQUIRE_FALSE(ink.isEmpty());
    CHECK(count(*base, {1.0, 0.0, 0.0}) == 0);

    DrawItem outside = plain;
    outside.text.strokes = {DrawTextStroke{{1.0, 0.0, 0.0, 1.0}, 6.0, StrokePosition::Outside}};
    const auto out = draw(outside);
    save(*out, QStringLiteral("text_stroke_outside.png"));
    const QRect outInk = inkOf(*out);
    INFO(std::format("plain {} {} {} {}, outside {} {} {} {}", ink.x(), ink.y(), ink.width(), ink.height(), outInk.x(),
                     outInk.y(), outInk.width(), outInk.height()));
    CHECK(outInk.left() <= ink.left() - 4);
    CHECK(outInk.right() >= ink.right() + 4);
    CHECK(outInk.top() <= ink.top() - 4);
    CHECK(count(*out, {1.0, 0.0, 0.0}) > 200);       // the red ring
    CHECK(count(*out, {1.0, 1.0, 1.0}) > count(*base, {1.0, 1.0, 1.0}) * 0.8); // the white fill on top of it

    DrawItem inside = plain;
    inside.text.strokes = {DrawTextStroke{{1.0, 0.0, 0.0, 1.0}, 4.0, StrokePosition::Inside}};
    const auto in = draw(inside);
    save(*in, QStringLiteral("text_stroke_inside.png"));
    const QRect inInk = inkOf(*in);
    CHECK(std::abs(inInk.left() - ink.left()) <= 1); // no bigger than the glyphs
    CHECK(std::abs(inInk.right() - ink.right()) <= 1);
    CHECK(count(*in, {1.0, 0.0, 0.0}) > 100);
    CHECK(count(*in, {1.0, 1.0, 1.0}) < count(*base, {1.0, 1.0, 1.0})); // drawn over the fill's edge

    DrawItem centre = plain;
    centre.text.strokes = {DrawTextStroke{{1.0, 0.0, 0.0, 1.0}, 8.0, StrokePosition::Center}};
    const QRect centreInk = inkOf(*draw(centre));
    CHECK(centreInk.left() <= ink.left() - 2);
    CHECK(centreInk.left() >= outInk.left() - 1); // half as far out as an outside stroke of that width

    // Two strokes: the second frames the first (a wider one behind it).
    DrawItem two = plain;
    two.text.strokes = {DrawTextStroke{{1.0, 0.0, 0.0, 1.0}, 4.0, StrokePosition::Outside},
                        DrawTextStroke{{0.0, 0.0, 1.0, 1.0}, 10.0, StrokePosition::Outside}};
    const auto both = draw(two);
    save(*both, QStringLiteral("text_strokes_two.png"));
    CHECK(count(*both, {1.0, 0.0, 0.0}) > 100);
    CHECK(count(*both, {0.0, 0.0, 1.0}) > 100);
    CHECK(inkOf(*both).left() <= ink.left() - 8);

    // Faux bold: the same glyphs, thicker (more ink, a little wider).
    DrawItem bold = plain;
    bold.text.runs[0].fauxBold = true;
    const auto thick = draw(bold);
    save(*thick, QStringLiteral("text_faux_bold.png"));
    CHECK(count(*thick, {1.0, 1.0, 1.0}) > count(*base, {1.0, 1.0, 1.0}) * 1.05);
    CHECK(inkOf(*thick).width() >= ink.width());
}

TEST_CASE("0.6: faux italic slants the glyphs and capitals change how the letters draw", "[render][graphics][text]") {
    // A tall upright stroke: slanted, its top sits to the right of its foot.
    DrawItem bar = textItem("I", 120.0, 120, 20);
    bar.text.runs[0].fauxItalic = true;
    const auto slanted = draw(bar);
    save(*slanted, QStringLiteral("text_faux_italic.png"));
    const QRect ink = inkOf(*slanted);
    REQUIRE(ink.height() > 40);
    const auto rowCentre = [&](int y) {
        double sum = 0.0, n = 0.0;
        for (int x = 0; x < kW; ++x) {
            if (at(*slanted, x, y)[3] > 0.5) {
                sum += x;
                n += 1.0;
            }
        }
        return n > 0 ? sum / n : 0.0;
    };
    CHECK(rowCentre(ink.top() + 4) > rowCentre(ink.bottom() - 4) + 10.0);

    // All Caps draws "ace" as "ACE"; Small Caps draws smaller capitals (between the two).
    const QRect lower = inkOf(*draw(textItem("ace", 72.0, 20, 30)));
    const QRect upper = inkOf(*draw(textItem("ACE", 72.0, 20, 30)));
    DrawItem caps = textItem("ace", 72.0, 20, 30);
    caps.text.runs[0].caps = TextCaps::AllCaps;
    const QRect allCaps = inkOf(*draw(caps));
    caps.text.runs[0].caps = TextCaps::SmallCaps;
    const auto small = draw(caps);
    save(*small, QStringLiteral("text_small_caps.png"));
    const QRect smallCaps = inkOf(*small);
    INFO(std::format("lower top {}, upper top {}, all caps top {}, small caps top {}", lower.top(), upper.top(), allCaps.top(),
                     smallCaps.top()));
    CHECK(std::abs(allCaps.top() - upper.top()) <= 1);
    CHECK(std::abs(allCaps.width() - upper.width()) <= 2);
    CHECK(lower.top() > upper.top() + 5);
    CHECK(smallCaps.top() > allCaps.top());

    // Kerning off spaces every pair by the advances alone: never tighter than with the font's kerning.
    DrawItem kerned = textItem("AVAVAV", 60.0, 10, 40);
    DrawItem loose = kerned;
    loose.text.runs[0].kerning = false;
    CHECK(inkOf(*draw(loose)).width() >= inkOf(*draw(kerned)).width());
}

TEST_CASE("0.6: paragraph text sits at the top centre or bottom of a taller box", "[render][graphics][text]") {
    DrawItem box = textItem("Hi", 30.0, 40, 10);
    box.text.boxWidth = 200.0;
    box.text.boxHeight = 150.0;
    std::array<int, 3> tops{};
    for (int i = 0; i < 3; ++i) {
        box.text.verticalAlign = static_cast<TextVerticalAlign>(i);
        const auto r = draw(box);
        tops[static_cast<std::size_t>(i)] = inkOf(*r).top();
        if (i == 1) {
            save(*r, QStringLiteral("text_vertical_center.png"));
        }
    }
    INFO(std::format("tops {} {} {}", tops[0], tops[1], tops[2]));
    CHECK(tops[0] < 10 + 20);
    CHECK(tops[1] > tops[0] + 40);
    CHECK(tops[2] > tops[1] + 40);
    CHECK(tops[2] < 10 + 150);
    // Point text ignores it.
    DrawItem point = textItem("Hi", 30.0, 40, 10);
    const int pointTop = inkOf(*draw(point)).top();
    point.text.verticalAlign = TextVerticalAlign::Bottom;
    CHECK(inkOf(*draw(point)).top() == pointTop);
}

TEST_CASE("0.6: a shadow's size grows its shape before the blur", "[render][graphics][text]") {
    DrawItem square;
    square.kind = DrawKind::Shape;
    square.path = {MaskVertex{-20, -20}, MaskVertex{20, -20}, MaskVertex{20, 20}, MaskVertex{-20, 20}};
    square.fill.kind = PaintKind::Solid;
    square.fill.color = {1.0, 1.0, 1.0, 1.0};
    square.transform.e = 100;
    square.transform.f = 90;
    square.shadow = DrawShadow{true, {0.0, 0.0, 0.0, 1.0}, 1.0, 0.0, 60.0, 0.0, 0.0};
    const auto plain = rasterizeGraphic(listOf({square}), kW, kH);
    square.shadow.size = 10.0;
    const auto grown = rasterizeGraphic(listOf({square}), kW, kH);
    save(*grown, QStringLiteral("shadow_size.png"));
    // The shadow lies 60 px to the right: its right edge at 180 (plain), at 190 grown; and above / below the square too.
    CHECK(at(*plain, 185, 90)[3] < 0.1);
    CHECK(at(*grown, 185, 90)[3] > 0.9);
    CHECK(at(*plain, 160, 115)[3] < 0.1);
    CHECK(at(*grown, 160, 115)[3] > 0.9);
}

TEST_CASE("0.6: keyed size and Source Text draw per frame", "[render][graphics][text]") {
    GraphicLayer l;
    l.id = 1;
    l.kind = LayerKind::Text;
    TextStyle style;
    style.fontSize = 30.0;
    l.text.runs = {TextRun{"Wide", style}};
    l.transform.x = ParamValue(20.0);
    l.transform.y = ParamValue(20.0);
    Curve size;
    size.keys = {Keyframe{Ticks{}, 30.0}, Keyframe{Ticks{kTicksPerSecond}, 90.0}};
    REQUIRE(edit::setLayerParam(l, "text.fontSize", ParamValue(size)));
    Curve source;
    source.keys = {Keyframe{Ticks{}, 0.0, Interpolation::Hold}};
    REQUIRE(edit::setLayerParam(l, "text.source", ParamValue(source)));
    REQUIRE(edit::editSourceRunsAt(l.text, Ticks{kTicksPerSecond / 2}, [](std::vector<TextRun>& runs) {
        runs[0].text = "I";
        return true;
    }));
    Graphic g;
    g.width = kW;
    g.height = kH;
    g.layers = {l};
    REQUIRE_FALSE(graphicProblem(g));
    const auto frame = [&](double s) {
        return rasterizeGraphic(resolveGraphic(g, Ticks{static_cast<std::int64_t>(s * kTicksPerSecond)}, {}), kW, kH);
    };
    const QRect early = inkOf(*frame(0.0));
    const QRect later = inkOf(*frame(0.4));
    const auto narrowFrame = frame(0.6);
    save(*narrowFrame, QStringLiteral("text_source_key.png"));
    const QRect narrow = inkOf(*narrowFrame);
    CHECK(later.height() > early.height() + 10); // bigger as the size key moves on
    CHECK(narrow.width() < later.width() / 2);   // "I" from its key on
}
