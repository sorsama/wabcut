// 0.2 graphics: type on a path (TextPath): the walker the renderer places characters with, the preset paths, the JSON,
// the checks and edit::setTextPath.

#include "TimelineFixture.h"

#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Matchers::WithinAbs;

namespace {

GraphicLayer textLayer(std::uint32_t id, std::string text) {
    GraphicLayer l;
    l.id = id;
    l.kind = LayerKind::Text;
    l.text.runs = {TextRun{std::move(text), TextStyle{}}};
    return l;
}

TextPath onPath(std::vector<MaskVertex> vertices, bool closed = false) {
    TextPath p;
    p.path = {MaskPathKey{Ticks{}, std::move(vertices), Interpolation::Linear}};
    p.closed = closed;
    return p;
}

} // namespace

TEST_CASE("The text path walker measures by arc length and carries on past the ends", "[model][graphics][textpath]") {
    // A straight line 200 px long, left to right.
    const TextPathWalker line({MaskVertex{0, 0, 0, 0, 0, 0}, MaskVertex{200, 0, 0, 0, 0, 0}}, false, false);
    CHECK_THAT(line.length(), WithinAbs(200.0, 1e-9));
    const auto mid = line.at(50.0);
    CHECK_THAT(mid[0], WithinAbs(50.0, 1e-9));
    CHECK_THAT(mid[2], WithinAbs(0.0, 1e-9));
    const auto past = line.at(250.0); // straight on past the end
    CHECK_THAT(past[0], WithinAbs(250.0, 1e-9));
    const auto before = line.at(-20.0);
    CHECK_THAT(before[0], WithinAbs(-20.0, 1e-9));
    // Reversed: from the right end, heading left (180 degrees).
    const TextPathWalker back({MaskVertex{0, 0, 0, 0, 0, 0}, MaskVertex{200, 0, 0, 0, 0, 0}}, false, true);
    CHECK_THAT(back.at(0.0)[0], WithinAbs(200.0, 1e-9));
    CHECK_THAT(std::abs(back.at(10.0)[2]), WithinAbs(180.0, 1e-9));
    // Alignment between the margins.
    CHECK_THAT(line.lineStart(100.0, TextAlign::Left, 10.0, 0.0, false)[0], WithinAbs(10.0, 1e-9));
    CHECK_THAT(line.lineStart(100.0, TextAlign::Center, 0.0, 0.0, false)[0], WithinAbs(50.0, 1e-9));
    CHECK_THAT(line.lineStart(100.0, TextAlign::Right, 0.0, 20.0, false)[0], WithinAbs(80.0, 1e-9));
    const auto forced = line.lineStart(100.0, TextAlign::Left, 0.0, 0.0, true);
    CHECK_THAT(forced[1], WithinAbs(2.0, 1e-9)); // spread over the whole 200 px
}

TEST_CASE("The circle path runs clockwise round its centre and wraps", "[model][graphics][textpath]") {
    bool closed = false;
    const auto circle = edit::textPathVertices(edit::TextPathShape::Circle, 400.0, 0.0, &closed);
    CHECK(closed);
    const TextPathWalker walker(circle, true, false);
    CHECK_THAT(walker.length(), WithinAbs(2.0 * std::numbers::pi * 200.0, 2.0)); // a Bezier circle is within 0.03 %
    // A quarter of the way round from the left: the top, heading right (0 degrees).
    const auto top = walker.at(walker.length() / 4.0);
    CHECK_THAT(top[0], WithinAbs(0.0, 1.0));
    CHECK_THAT(top[1], WithinAbs(-200.0, 1.0));
    CHECK_THAT(top[2], WithinAbs(0.0, 2.0));
    const auto wrapped = walker.at(walker.length() * 1.25);
    CHECK_THAT(wrapped[0], WithinAbs(top[0], 1e-6));
    // An arc rises from its left end to its top and comes down to the right.
    const auto arc = edit::textPathVertices(edit::TextPathShape::Arc, 800.0, 300.0, &closed);
    CHECK_FALSE(closed);
    const TextPathWalker arcWalker(arc, false, false);
    const auto peak = arcWalker.at(arcWalker.length() / 2.0);
    CHECK_THAT(peak[1], WithinAbs(-300.0, 1.0));
    CHECK(arcWalker.at(10.0)[2] < -45.0); // heading up at the start
    // A wave crosses its axis at the ends and in the middle.
    const auto wave = edit::textPathVertices(edit::TextPathShape::Wave, 800.0, 120.0);
    REQUIRE(wave.size() == 9);
    CHECK(wave.front().x == -400.0);
    CHECK(wave[4].y == 0.0);
    CHECK(wave[1].y == -60.0);
}

TEST_CASE("Type on a path is saved, checked and resolved", "[model][graphics][textpath]") {
    Graphic g;
    GraphicLayer text = textLayer(1, "Around the world");
    TextPath path = onPath(edit::textPathVertices(edit::TextPathShape::Arc, 800.0, 300.0));
    path.reverse = true;
    path.perpendicular = false;
    path.forceAlignment = true;
    Curve margin;
    margin.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{Ticks{kTicksPerSecond}, 400.0}};
    path.firstMargin = ParamValue(margin);
    path.lastMargin = 12.0;
    text.text.path = path;
    g.layers = {text};
    REQUIRE_FALSE(graphicProblem(g));
    const std::string json = io::graphicToJson(g);
    CHECK(json.find("\"onPath\"") != std::string::npos);
    const auto back = io::graphicFromJson(json);
    REQUIRE(back);
    CHECK(*back == g);

    // The draw list carries the path at that time and the margins' values.
    const DrawList list = resolveGraphic(g, Ticks{kTicksPerSecond / 2});
    REQUIRE(list.items.size() == 1);
    const DrawText& t = list.items[0].text;
    CHECK(t.path.size() == 3);
    CHECK(t.pathReverse);
    CHECK_FALSE(t.pathPerpendicular);
    CHECK(t.pathForceAlignment);
    CHECK_THAT(t.firstMargin, WithinAbs(200.0, 1e-6));
    CHECK_THAT(t.lastMargin, WithinAbs(12.0, 1e-9));
    CHECK(t.clusters.size() == 16); // every cluster, spaces too (newlines would not be)
    // Its box covers the arc.
    const Box box = layerBounds(g.layers[0], Ticks{});
    CHECK(box.x <= -400.0);
    CHECK(box.y <= -300.0);

    // Checks: a path needs two vertices; only a text layer has one.
    Graphic bad = g;
    bad.layers[0].text.path->path.front().vertices.resize(1);
    CHECK(graphicProblem(bad));
    Graphic shape = g;
    shape.layers[0].kind = LayerKind::Shape;
    shape.layers[0].text.runs.clear();
    CHECK(graphicProblem(shape));
}

TEST_CASE("edit::setTextPath puts a text layer on a path and back, one undo step each", "[model][graphics][textpath]") {
    Timeline tl(R"(
        V2     |..........|
        V1 tgt |Aaaaaaaaaa|
    )");
    Graphic g;
    g.layers = {textLayer(1, "Hello"), GraphicLayer{}};
    g.layers[1].id = 2;
    g.layers[1].kind = LayerKind::Shape;
    const auto added = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, framesToTicks(10, kRate), g);
    REQUIRE(added);
    const ClipId clip = added->placed.at(0);
    const auto on = edit::setTextPath(added->state, tl.seq, clip, 1, onPath({MaskVertex{0, 0, 0, 0, 0, 0}, MaskVertex{300, 0, 0, 0, 0, 0}}));
    REQUIRE(on);
    const Graphic* got = on->state.findSequence(tl.seq)->findClip(clip)->clip().graphic();
    REQUIRE(got);
    REQUIRE(got->findLayer(1)->text.path);
    const auto off = edit::setTextPath(on->state, tl.seq, clip, 1, std::nullopt);
    REQUIRE(off);
    CHECK_FALSE(off->state.findSequence(tl.seq)->findClip(clip)->clip().graphic()->findLayer(1)->text.path);
    // Not a text layer; no such layer.
    CHECK_FALSE(edit::setTextPath(added->state, tl.seq, clip, 2, onPath({MaskVertex{}, MaskVertex{1, 0, 0, 0, 0, 0}})));
    CHECK_FALSE(edit::setTextPath(added->state, tl.seq, clip, 9, std::nullopt));
}
